/**
 * @file i2c_bitbang_kernel_driver.c
 * @brief Raspberry Pi 5 Kernel-Space Bit-Banged I2C Driver for SSD1306 OLED
 *
 * This Linux Kernel Module (LKM) implements a fully custom bit-banged I2C
 * protocol over FREELY CHOSEN GPIO pins on the Raspberry Pi 5 RP1
 * chip. It does NOT use the Linux I2C subsystem (i2c-dev, i2c-gpio, i2c_adapter)
 * or any hardware I2C peripheral — every SCL/SDA transition is directly
 * written to the RP1 RIO (Register IO) atomic GPIO registers via ioremap.
 *
 * It exposes the following hardware features of the SSD1306 controller via IOCTL:
 *
 *   - I2C clock frequency control
 *   - Command / Data (D/C) mode switching
 *   - Hardware RST, Sleep, Wakeup, Init
 *   - Contrast (256 levels)
 *   - Horizontal mirror  (H-flip)
 *   - Vertical flip      (V-flip / 180°)
 *   - Pixel inversion
 *   - Autonomous H-scroll & diagonal scroll
 *   - GDDRAM addressing mode (H/V/Page)
 *   - Column & page address windowing
 *
 * I2C Protocol Summary (Standard Mode / Fast Mode):
 *   - START  : SDA falls while SCL is HIGH.
 *   - STOP   : SDA rises  while SCL is HIGH.
 *   - Data   : MSB-first, 8 bits per byte, ACK on 9th clock pulse.
 *   - SSD1306 slave address : 0x3C (SA0=GND.
 *
 * Module compatibility:
 *   Targets 4-pin I2C OLED modules (VCC / GND / SCL / SDA) — the most common
 *   SSD1306 breakout variant. There is no reset pin, the driver performs a
 *   software reset by re-sending the full SSD1306 init sequence instead.
 *
 * RP1 Hardware Notes (Raspberry Pi 5):
 *   The RP1 south-bridge sits behind a PCIe link. Every iowrite32() is a
 *   posted write that may be coalesced by the PCIe controller. A subsequent
 *   ioread32() from the same region flushes the write queue, guaranteeing the
 *   pin actually toggles before we advance in the bit-bang loop.
 */

/* ============================================================================
 1. KERNEL INCLUSIONS
============================================================================ */
#include <linux/module.h>       /* Core header for loading LKMs into the kernel */
#include <linux/init.h>         /* Macros for module_init and module_exit */
#include <linux/fs.h>           /* VFS file operations and device registration */
#include <linux/cdev.h>         /* Character device abstractions */
#include <linux/uaccess.h>      /* copy_to_user, copy_from_user */
#include <asm/io.h>             /* Memory mapping (ioremap, iounmap, iowrite32) */
#include <linux/delay.h>        /* Kernel delay functions (ndelay, msleep) */
#include <linux/device.h>       /* Device / Class creation (sysfs) */
#include <linux/slab.h>         /* kmalloc, kfree */
#include <linux/version.h>      /* LINUX_VERSION_CODE, KERNEL_VERSION macros */
#include <linux/ioctl.h>        /* User-space to kernel-space IO control macros */
#include <linux/mutex.h>        /* Mutex for exclusive I2C bus protection */

/* ============================================================================
2. MACROS & DEFINITIONS
============================================================================ */

#define DRIVER_NAME  "i2c_ssd1306_bitbang"
#define DRIVER_CLASS "i2c_ssd1306_class"

/* RP1 Memory Map Base for Pi 5 */
#define RP1_PERIPHERAL_BASE     0x1F00000000ULL
#define RP1_MAP_SIZE            0x400000

/* IO_BANK0 Offset for PIN Control (FUNCSEL) */
#define IO_BANK0_OFFSET         0x0D0000

/* RIO Offsets for RP1 Atomic GPIO Toggling
 *
 * RP1 SYS_RIO register map.
 *
 * Base registers (relative to RIO_OFFSET):
 *   OUT  0x0000 — GPIO output value
 *   OE   0x0004 — Output enable
 *   IN   0x0008 — Pad input level (read-only)
 *
 * Atomic alias pages:
 *   +0x2000  SET  — write 1 to set bit HIGH,  0 = no change
 *   +0x3000  CLR  — write 1 to clear bit LOW, 0 = no change
 *   +0x1000  XOR  — write 1 to toggle bit
 */
#define RIO_OFFSET              0x0e0000
#define RIO_OUT                 0x0000
#define RIO_OE                  0x0004
#define RIO_IN                  0x0008
#define RIO_XOR_ALIAS           0x1000   
#define RIO_SET_ALIAS           0x2000   
#define RIO_CLR_ALIAS           0x3000   
#define RIO_OUT_SET             (RIO_SET_ALIAS + RIO_OUT)   /* 0x2000 */
#define RIO_OE_SET              (RIO_SET_ALIAS + RIO_OE)    /* 0x2004 */
#define RIO_OUT_CLR             (RIO_CLR_ALIAS + RIO_OUT)   /* 0x3000 */
#define RIO_OE_CLR              (RIO_CLR_ALIAS + RIO_OE)    /* 0x3004 */

/*
 * GPIO Pins assigned for bit-banged I2C (SSD1306 — 4-pin module).
 */
#define I2C_SCL_PIN             23    /* Serial Clock         (GPIO 23, Pin 16) */
#define I2C_SDA_PIN             24    /* Serial Data          (GPIO 24, Pin 18) */

/*
 * SSD1306 I2C slave address.
 */
#define SSD1306_I2C_ADDR        0x3C

/*
 * I2C control bytes prepended to every SSD1306 transfer (after the address):
 *   0x00 → next bytes are Commands
 *   0x40 → next bytes are Data (GDDRAM pixel buffer)
 */
#define SSD1306_CTRL_CMD        0x00
#define SSD1306_CTRL_DATA       0x40

/*
 * Maximum clock-stretch wait iterations.
 * After asserting SCL HIGH we poll until the line actually reads HIGH
 * (a slave may hold it LOW — "clock stretching"). We bail after this many
 * iterations to avoid an infinite loop in the kernel.
 */
#define I2C_SCL_STRETCH_TIMEOUT 1000

/*
 * IOCTL (Input/Output Control) Definitions
 * Magic number 'i' identifies our specific I2C driver's commands safely.
 */
#define I2C_IOC_MAGIC           'i'

/* User provides the half-period delay in nanoseconds to adjust bus speed */
#define I2C_IOC_SET_FREQ            _IOW(I2C_IOC_MAGIC,  1, uint32_t)

/* Tells driver next write() bytes are Configuration Commands (Co=0, D/C#=0) */
#define SSD1306_IOC_SET_CMD_MODE    _IO(I2C_IOC_MAGIC,   2)

/* Tells driver next write() bytes are Pixel Framebuffer Data (Co=0, D/C#=1) */
#define SSD1306_IOC_SET_DATA_MODE   _IO(I2C_IOC_MAGIC,   3)

/* Forces a hardware reboot of the OLED panel via RST pin */
#define SSD1306_IOC_HW_RESET        _IO(I2C_IOC_MAGIC,   4)

/* Power Management — Send Display-OFF (0xAE) via I2C */
#define SSD1306_IOC_SLEEP           _IO(I2C_IOC_MAGIC,   5)

/* Power Management — Send Display-ON (0xAF) via I2C */
#define SSD1306_IOC_WAKEUP          _IO(I2C_IOC_MAGIC,   6)

/* Re-run the full SSD1306 init sequence */
#define SSD1306_IOC_INIT            _IO(I2C_IOC_MAGIC,   7)

/* Set display contrast (0x00–0xFF) */
#define SSD1306_IOC_SET_CONTRAST    _IOW(I2C_IOC_MAGIC,  8, uint8_t)

/* Horizontal mirror: 0=normal, 1=mirrored (column re-map) */
#define SSD1306_IOC_FLIP_H          _IOW(I2C_IOC_MAGIC,  9, uint8_t)

/* Vertical flip: 0=normal, 1=flipped (COM scan direction) */
#define SSD1306_IOC_FLIP_V          _IOW(I2C_IOC_MAGIC, 10, uint8_t)

/* Pixel inversion: 0=normal, 1=inverted */
#define SSD1306_IOC_INVERT          _IOW(I2C_IOC_MAGIC, 11, uint8_t)

/* Horizontal scroll configuration (see struct ssd1306_scroll_cfg) */
#define SSD1306_IOC_SCROLL_H        _IOW(I2C_IOC_MAGIC, 12, struct ssd1306_scroll_cfg)

/* Diagonal (vertical + horizontal) scroll */
#define SSD1306_IOC_SCROLL_DIAG     _IOW(I2C_IOC_MAGIC, 13, struct ssd1306_scroll_cfg)

/* Stop any active scroll */
#define SSD1306_IOC_SCROLL_STOP     _IO(I2C_IOC_MAGIC,  14)

/* GDDRAM addressing mode: 0=Horizontal, 1=Vertical, 2=Page */
#define SSD1306_IOC_SET_ADDR_MODE   _IOW(I2C_IOC_MAGIC, 15, uint8_t)

/* Column address window (start/end, 0–127) */
#define SSD1306_IOC_SET_COL_RANGE   _IOW(I2C_IOC_MAGIC, 16, struct ssd1306_range)

/* Page address window (start/end, 0–7) */
#define SSD1306_IOC_SET_PAGE_RANGE  _IOW(I2C_IOC_MAGIC, 17, struct ssd1306_range)

/*
 * Scroll configuration passed via IOCTL for horizontal and diagonal scrolls.
 *   direction   : 0 = right, 1 = left
 *   start_page  : first page row to scroll (0–7)
 *   end_page    : last  page row to scroll (0–7, >= start_page)
 *   speed       : scroll step interval (0–7, maps to SSD1306 frame counts)
 *   vert_offset : vertical offset per step (1–63, diagonal scroll only)
 */
struct ssd1306_scroll_cfg {
    uint8_t direction;
    uint8_t start_page;
    uint8_t end_page;
    uint8_t speed;
    uint8_t vert_offset;
};

/*
 * Address window passed via IOCTL for column / page range commands.
 *   start : first column (0–127) or first page (0–7)
 *   end   : last  column (0–127) or last  page (0–7)
 */
struct ssd1306_range {
    uint8_t start;
    uint8_t end;
};

/*
 * Maximum permitted write() payload.
 * SSD1306 full frame = 1024 bytes; 4096 gives headroom for init sequences
 * while preventing unbounded kmalloc() calls from user space.
 */
#define I2C_MAX_WRITE_LEN           4096

/* ========================================================================
3. VARIABLE DECLARATIONS
============================================================================ */

static dev_t dev_num;
static struct cdev i2c_cdev;
static struct class  *i2c_class  = NULL;
static struct device *i2c_device = NULL;
static void __iomem  *rp1_base_ptr;

/*
 * Half-period of the I2C clock in nanoseconds.
 * Default 2500 ns → SCL period ≈ 5 µs → ~100 kHz (Standard Mode).
 * Set to 625 ns for Fast Mode (~400 kHz).
 */
static uint32_t i2c_half_period_ns = 2500;

/*
 * Transfer-type guard: set to 'true' after the user calls SET_CMD_MODE or
 * SET_DATA_MODE via IOCTL. driver_write() warns if it is still false,
 * indicating the caller forgot to declare the transfer type.
 */
static bool dc_mode_set = false;

/*
 * Current control byte selected by IOCTL:
 *   SSD1306_CTRL_CMD  (0x00) or SSD1306_CTRL_DATA (0x40).
 */
static uint8_t current_ctrl_byte = SSD1306_CTRL_CMD;

/*
 * Mutex protecting concurrent access to the I2C bus, GPIO registers, and the
 * shared i2c_half_period_ns variable. Prevents bit-stream corruption when two
 * processes open /dev/i2c_ssd1306_bitbang simultaneously.
 */
static DEFINE_MUTEX(i2c_mutex);

/*
 * Runtime display state — tracks what the hardware is currently configured to.
 * Initialised to the values set by ssd1306_hardware_init_sequence().
 */
static struct {
    uint8_t contrast;   /* 0x00–0xFF, default 0x7F                  */
    uint8_t flip_h;     /* 0=normal,  1=mirrored                     */
    uint8_t flip_v;     /* 0=normal,  1=flipped                      */
    uint8_t inverted;   /* 0=normal,  1=white-bg                     */
    uint8_t addr_mode;  /* 0=Horizontal 1=Vertical 2=Page, default 0 */
    uint8_t scrolling;  /* 0=idle, 1=scroll active                   */
} g_disp_state = { 0x7F, 1, 1, 0, 0, 0 };

/* ============================================================================
4. FUNCTION IMPLEMENTATIONS
============================================================================ */

/**
 * gpio_write() - Drive a single RP1 GPIO pin high or low via RIO atomic regs.
 */
static void gpio_write(uint8_t gpio_pin, uint8_t level)
{
    u8 __iomem *base = (u8 __iomem *)rp1_base_ptr;
    if (level)
        iowrite32(1U << gpio_pin, base + RIO_OFFSET + RIO_OUT_SET);
    else
        iowrite32(1U << gpio_pin, base + RIO_OFFSET + RIO_OUT_CLR);

    /*
     * Because the RP1 is on a PCIe bus, 'iowrite32' calls are "posted"
     * (buffered). Reading from the same region forces the PCIe controller to halt, flush
     * the write queues, and guarantee the pin actually flipped before proceeding.
     */
    ioread32(base + RIO_OFFSET + RIO_IN);
}

/**
 * gpio_read() - Sample the current logical level of a GPIO pin via RIO.
 *
 * Used when reading the SDA line (ACK detection, clock-stretch polling).
 */
static uint8_t gpio_read(uint8_t gpio_pin)
{
    u8 __iomem *base = (u8 __iomem *)rp1_base_ptr;
    return (ioread32(base + RIO_OFFSET + RIO_IN) >> gpio_pin) & 1U;
}

/**
 * gpio_set_direction() - Set a GPIO as SIO output or input via IO_BANK0 FUNCSEL.
 */
static void gpio_set_direction(uint8_t gpio_pin, uint8_t is_output)
{
    u8 __iomem *base = (u8 __iomem *)rp1_base_ptr;
    u32 ctrl_addr, ctrl_val;

    /* Route pin to 'SIO' (Software IO — function 5 on RP1) */
    ctrl_addr = IO_BANK0_OFFSET + (gpio_pin * 8) + 4;
    ctrl_val  = ioread32(base + ctrl_addr);
    ctrl_val  = (ctrl_val & ~0x1fU) | 5U;
    iowrite32(ctrl_val, base + ctrl_addr);

    /* Update the Output Enable register in RIO */
    if (is_output)
        iowrite32(1U << gpio_pin, base + RIO_OFFSET + RIO_OE_SET);
    else
        iowrite32(1U << gpio_pin, base + RIO_OFFSET + RIO_OE_CLR);
}

/* --- I2C OPEN-DRAIN BUS HELPERS --- */

/**
 * sda_high() - Release SDA to the pull-up (set pin as input / high-Z).
 *
 * True I2C is open-drain: a master must never actively drive the line to
 * VCC because a slave may be holding it LOW (ACK or clock-stretch). Instead,
 * the pull-up resistor pull SDA HIGH.
 */
static inline void sda_high(void)
{
    gpio_set_direction(I2C_SDA_PIN, 0); /* Input / high-Z → pull-up takes over */
}

/**
 * sda_low() - Drive SDA to GND (set pin as output-low).
 *
 * The RIO OUT register was pre-loaded with 0 during init, so switching to
 * output immediately pulls the line LOW without a glitch.
 */
static inline void sda_low(void)
{
    gpio_set_direction(I2C_SDA_PIN, 1); /* Output — value is already 0 */
}

/**
 * scl_high() - Release SCL to the pull-up and wait for clock-stretch to end.
 *
 * Returns 0 on success, -ETIMEDOUT if the slave holds SCL LOW too long.
 */
static int scl_high(void)
{
    int retries = I2C_SCL_STRETCH_TIMEOUT;

    gpio_set_direction(I2C_SCL_PIN, 0); /* Release SCL — pull-up takes over */
    ndelay(i2c_half_period_ns);

    /* Poll until SCL is actually HIGH (clock-stretching slaves hold it LOW) */
    while (!gpio_read(I2C_SCL_PIN)) {
        if (--retries == 0) {
            pr_warn("i2c_ssd1306: clock-stretch timeout — slave holding SCL LOW\n");
            return -ETIMEDOUT;
        }
        ndelay(i2c_half_period_ns);
    }
    return 0;
}

/**
 * scl_low() - Drive SCL to GND (set pin as output-low).
 */
static inline void scl_low(void)
{
    gpio_set_direction(I2C_SCL_PIN, 1); /* Output — value is already 0 */
    ndelay(i2c_half_period_ns);
}

/* --- I2C BUS-LEVEL PRIMITIVES --- */

/**
 * i2c_start() - Generate an I2C START condition.
 *
 * START: SDA falls while SCL is HIGH.
 *
 * Precondition : bus idle (SCL HIGH, SDA HIGH).
 * Postcondition: SCL LOW, SDA LOW (ready for first data bit).
 */
static void i2c_start(void)
{
    sda_high();
    ndelay(i2c_half_period_ns);
    scl_high();           /* SCL HIGH — ignore stretch return; bus is idle */
    ndelay(i2c_half_period_ns);

    sda_low();            /* SDA falls while SCL is HIGH  → START */
    ndelay(i2c_half_period_ns);
    scl_low();            /* Pull SCL LOW — data phase begins */
}

/**
 * i2c_stop() - Generate an I2C STOP condition.
 *
 * STOP: SDA rises while SCL is HIGH.
 *
 * Precondition : SCL LOW, SDA in any state.
 * Postcondition: bus idle (SCL HIGH, SDA HIGH).
 */
static void i2c_stop(void)
{
    sda_low();
    ndelay(i2c_half_period_ns);
    scl_high();           /* SCL HIGH */
    ndelay(i2c_half_period_ns);

    sda_high();           /* SDA rises while SCL is HIGH → STOP */
    ndelay(i2c_half_period_ns);
}

/**
 * i2c_write_byte() - Clock one byte onto the I2C bus MSB-first and check ACK.
 *
 * After the 8th bit the master releases SDA and clocks one more SCL pulse.
 * The slave pulls SDA LOW during this 9th clock to acknowledge (ACK).
 * A HIGH SDA on the 9th clock means NACK — the slave is absent or busy.
 *
 * Returns: 0 = ACK received, -EIO = NACK (no acknowledgement from slave).
 */
static int i2c_write_byte(uint8_t byte)
{
    int i;
    uint8_t ack;

    for (i = 7; i >= 0; i--) {
        /* Set SDA to the current bit MSB-first */
        if ((byte >> i) & 1)
            sda_high();
        else
            sda_low();

        ndelay(i2c_half_period_ns);

        /* Rising clock edge — slave samples SDA on this edge */
        scl_high();
        ndelay(i2c_half_period_ns);

        /* Falling clock edge — master prepares next bit */
        scl_low();
    }

    /* 9th clock — ACK/NACK phase */
    sda_high();                  /* Release SDA so slave can drive it LOW */
    ndelay(i2c_half_period_ns);
    scl_high();                  /* Rising edge — slave drives ACK */
    ndelay(i2c_half_period_ns);

    ack = gpio_read(I2C_SDA_PIN); /* 0 = ACK, 1 = NACK */

    scl_low();
    ndelay(i2c_half_period_ns);

    if (ack) {
        pr_warn("i2c_ssd1306: NACK received for byte 0x%02X\n", byte);
        return -EIO;
    }
    return 0;
}

/* --- SSD1306 TRANSPORT HELPERS --- */

/**
 * ssd1306_i2c_send_cmd_buf() - Send a sequence of SSD1306 command bytes over I2C.
 *
 * Handles the full I2C transaction: START, address frame, control byte (0x00 =
 * command mode), payload bytes, and STOP. All IOCTL cases that send SSD1306
 * commands use this helper to avoid duplicated address/control boilerplate.
 *
 * Caller MUST hold i2c_mutex before calling.
 *
 * I2C transaction format:
 *   [START][ADDR W][0x00][cmd_0][cmd_1]...[cmd_N-1][STOP]
 *
 * @cmds: pointer to command byte array
 * @len:  number of bytes to send
 *
 * Returns: 0 on success, negative errno on first NACK.
 */
static int ssd1306_i2c_send_cmd_buf(const uint8_t *cmds, int len)
{
    int i, ret = 0;

    i2c_start();

    ret = i2c_write_byte((SSD1306_I2C_ADDR << 1) | 0); /* Address + Write bit */
    if (ret) goto out_stop;

    ret = i2c_write_byte(SSD1306_CTRL_CMD);             /* Control byte: command mode */
    if (ret) goto out_stop;

    for (i = 0; i < len; i++) {
        ret = i2c_write_byte(cmds[i]);
        if (ret) goto out_stop;
    }

    dc_mode_set = true;  /* I2C bus is now in command mode */

out_stop:
    i2c_stop();
    return ret;
}

/**
 * ssd1306_hardware_init_sequence() - Push the SSD1306 power-on init sequence.
 *
 * Initialises display geometry, charge-pump, contrast, and turns the panel on.
 * Must be called while i2c_mutex is held by the caller (ModuleInit acquires it
 * indirectly because no other task can have opened the device yet at that point).
 */
static int ssd1306_hardware_init_sequence(void)
{
    static const uint8_t init_cmds[] = {
        0xAE,       /*  1. Display OFF (Sleep mode)                              */
        0xD5, 0x80, /*  2. Clock divide ratio / oscillator frequency             */
        0xA8, 0x3F, /*  3. Multiplex ratio  (0x3F = 64 rows for 128×64)         */
        0xD3, 0x00, /*  4. Display offset   (0 = no vertical shift)             */
        0x40,       /*  5. Display start line = 0                                */
        0x8D, 0x14, /*  6. ENABLE CHARGE PUMP (critical — screen is black without) */
        0x20, 0x00, /*  7. Memory addressing mode: Horizontal                    */
        0xA1,       /*  8. Segment re-map (column 127 → SEG0)                   */
        0xC8,       /*  9. COM output scan direction (remapped)                  */
        0xDA, 0x12, /* 10. COM pins hardware configuration                       */
        0x81, 0x7F, /* 11. Contrast control (0x7F = mid-level)                   */
        0xD9, 0xF1, /* 12. Pre-charge period                                     */
        0xDB, 0x40, /* 13. VCOMH deselect level                                  */
        0xA4,       /* 14. Entire display ON — resume from RAM content           */
        0xA6,       /* 15. Normal (non-inverted) display                         */
        0xAF        /* 16. DISPLAY ON — wake panel up                            */
    };
    int ret;

    ret = ssd1306_i2c_send_cmd_buf(init_cmds, (int)ARRAY_SIZE(init_cmds));
    if (ret) {
        pr_err("i2c_ssd1306: init sequence failed (ret=%d)\n", ret);
        return ret;
    }

    pr_info("i2c_ssd1306: Initialization sequence written to display\n");
    return 0;
}

/* --- VFS FILE OPERATIONS --- */

/**
 * driver_open() - Validate the open mode before allowing access.
 *
 * Reject O_RDONLY opens. The SSD1306 is a write-only device;
 * there is no meaningful data to read back from the bus.
 */
static int driver_open(struct inode *device_file, struct file *instance)
{
    if ((instance->f_flags & O_ACCMODE) == O_RDONLY) {
        pr_warn("i2c_ssd1306: Rejected O_RDONLY open — device is write-only\n");
        return -EACCES;
    }
    return 0;
}

static int driver_close(struct inode *device_file, struct file *instance)
{
    return 0;
}

/**
 * driver_ioctl() - Handle user-space control commands.
 *
 * i2c_mutex is acquired at the top and released at every exit path via a
 * single common unlock, using a local 'ret' variable instead of scattered
 * 'return' statements inside the switch.
 */
static long driver_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long ret = 0;

    if (mutex_lock_interruptible(&i2c_mutex))
        return -ERESTARTSYS;

    switch (cmd) {

    /* ------------------------------------------------------------------ */
    /* Core commands (1–7)                                                 */
    /* ------------------------------------------------------------------ */
    case I2C_IOC_SET_FREQ: {
        uint32_t user_half_period_ns;
        if (copy_from_user(&user_half_period_ns, (uint32_t __user *)arg,
                           sizeof(user_half_period_ns))) {
            ret = -EFAULT;
            break;
        }
        if (user_half_period_ns < 10)
            user_half_period_ns = 10;
        i2c_half_period_ns = user_half_period_ns;
        pr_info("i2c_ssd1306: I2C half-period updated to %u ns "
                "(SCL period ≈ %u ns)\n",
                i2c_half_period_ns, i2c_half_period_ns * 2);
        break;
    }

    case SSD1306_IOC_SET_CMD_MODE:
        current_ctrl_byte = SSD1306_CTRL_CMD;   /* 0x00 */
        dc_mode_set = true;
        pr_info("i2c_ssd1306: Transfer mode set to COMMAND (0x00)\n");
        break;

    case SSD1306_IOC_SET_DATA_MODE:
        current_ctrl_byte = SSD1306_CTRL_DATA;  /* 0x40 */
        dc_mode_set = true;
        pr_info("i2c_ssd1306: Transfer mode set to DATA (0x40)\n");
        break;

    case SSD1306_IOC_HW_RESET:
        /*
         * 4-pin I2C OLED modules have no physical RST pin.
         * Perform a software reset by re-sending the full init sequence.
         */
        ret = ssd1306_hardware_init_sequence();
        if (!ret)
            pr_info("i2c_ssd1306: Software reset (re-init) complete\n");
        break;

    case SSD1306_IOC_SLEEP: {
        uint8_t c = 0xAE;
        ret = ssd1306_i2c_send_cmd_buf(&c, 1);
        if (!ret)
            pr_info("i2c_ssd1306: OLED sleep (0xAE)\n");
        break;
    }

    case SSD1306_IOC_WAKEUP: {
        uint8_t c = 0xAF;
        ret = ssd1306_i2c_send_cmd_buf(&c, 1);
        if (!ret)
            pr_info("i2c_ssd1306: OLED wakeup (0xAF)\n");
        break;
    }

    case SSD1306_IOC_INIT:
        ret = ssd1306_hardware_init_sequence();
        break;

    /* ------------------------------------------------------------------ */
    /* Contrast / brightness (8)                                           */
    /* ------------------------------------------------------------------ */
    case SSD1306_IOC_SET_CONTRAST: {
        uint8_t contrast;
        uint8_t cmds[2];
        if (copy_from_user(&contrast, (uint8_t __user *)arg, sizeof(contrast))) {
            ret = -EFAULT;
            break;
        }
        cmds[0] = 0x81;
        cmds[1] = contrast;
        ret = ssd1306_i2c_send_cmd_buf(cmds, 2);
        if (!ret) {
            g_disp_state.contrast = contrast;
            pr_info("i2c_ssd1306: contrast → %u\n", contrast);
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Horizontal mirror / column re-map (9)                              */
    /* ------------------------------------------------------------------ */
    case SSD1306_IOC_FLIP_H: {
        uint8_t enable, cmd_byte;
        if (copy_from_user(&enable, (uint8_t __user *)arg, sizeof(enable))) {
            ret = -EFAULT;
            break;
        }
        cmd_byte = enable ? 0xA1 : 0xA0;
        ret = ssd1306_i2c_send_cmd_buf(&cmd_byte, 1);
        if (!ret) {
            g_disp_state.flip_h = enable ? 1 : 0;
            pr_info("i2c_ssd1306: H-flip %s\n", enable ? "ON" : "OFF");
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Vertical flip / COM scan direction (10)                            */
    /* ------------------------------------------------------------------ */
    case SSD1306_IOC_FLIP_V: {
        uint8_t enable, cmd_byte;
        if (copy_from_user(&enable, (uint8_t __user *)arg, sizeof(enable))) {
            ret = -EFAULT;
            break;
        }
        cmd_byte = enable ? 0xC8 : 0xC0;
        ret = ssd1306_i2c_send_cmd_buf(&cmd_byte, 1);
        if (!ret) {
            g_disp_state.flip_v = enable ? 1 : 0;
            pr_info("i2c_ssd1306: V-flip %s\n", enable ? "ON" : "OFF");
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Pixel inversion (11)                                                */
    /* ------------------------------------------------------------------ */
    case SSD1306_IOC_INVERT: {
        uint8_t enable, cmd_byte;
        if (copy_from_user(&enable, (uint8_t __user *)arg, sizeof(enable))) {
            ret = -EFAULT;
            break;
        }
        cmd_byte = enable ? 0xA7 : 0xA6;
        ret = ssd1306_i2c_send_cmd_buf(&cmd_byte, 1);
        if (!ret) {
            g_disp_state.inverted = enable ? 1 : 0;
            pr_info("i2c_ssd1306: display %s\n", enable ? "inverted" : "normal");
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Horizontal scroll (12)                                              */
    /* ------------------------------------------------------------------ */
    case SSD1306_IOC_SCROLL_H: {
        struct ssd1306_scroll_cfg cfg;
        uint8_t cmds[8];
        uint8_t stop = 0x2E;
        if (copy_from_user(&cfg,
                           (struct ssd1306_scroll_cfg __user *)arg,
                           sizeof(cfg))) {
            ret = -EFAULT;
            break;
        }
        if (cfg.start_page > 7 || cfg.end_page > 7 ||
            cfg.end_page < cfg.start_page || cfg.speed > 7) {
            ret = -EINVAL;
            break;
        }
        /* Always stop before reconfiguring to avoid RAM corruption */
        ssd1306_i2c_send_cmd_buf(&stop, 1);
        cmds[0] = cfg.direction ? 0x27 : 0x26; /* 0x27=left, 0x26=right */
        cmds[1] = 0x00;                          /* dummy A               */
        cmds[2] = cfg.start_page;
        cmds[3] = cfg.speed;
        cmds[4] = cfg.end_page;
        cmds[5] = 0x00;                          /* dummy B               */
        cmds[6] = 0xFF;                          /* dummy C               */
        cmds[7] = 0x2F;                          /* activate              */
        ret = ssd1306_i2c_send_cmd_buf(cmds, 8);
        if (!ret) {
            g_disp_state.scrolling = 1;
            pr_info("i2c_ssd1306: H-scroll %s pages %u-%u speed 0x%02x\n",
                    cfg.direction ? "left" : "right",
                    cfg.start_page, cfg.end_page, cfg.speed);
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Diagonal (vertical + horizontal) scroll (13)                       */
    /* ------------------------------------------------------------------ */
    case SSD1306_IOC_SCROLL_DIAG: {
        struct ssd1306_scroll_cfg cfg;
        uint8_t stop = 0x2E;
        uint8_t area[3];
        uint8_t cmds[7];
        if (copy_from_user(&cfg,
                           (struct ssd1306_scroll_cfg __user *)arg,
                           sizeof(cfg))) {
            ret = -EFAULT;
            break;
        }
        if (cfg.start_page > 7 || cfg.end_page > 7 ||
            cfg.end_page < cfg.start_page || cfg.speed > 7 ||
            cfg.vert_offset < 1 || cfg.vert_offset > 63) {
            ret = -EINVAL;
            break;
        }
        ssd1306_i2c_send_cmd_buf(&stop, 1);
        /* Set vertical scroll area: 0 fixed rows, all 64 rows scroll */
        area[0] = 0xA3; area[1] = 0x00; area[2] = 0x40;
        ssd1306_i2c_send_cmd_buf(area, 3);
        cmds[0] = cfg.direction ? 0x2A : 0x29; /* 0x2A=left, 0x29=right */
        cmds[1] = 0x00;
        cmds[2] = cfg.start_page;
        cmds[3] = cfg.speed;
        cmds[4] = cfg.end_page;
        cmds[5] = cfg.vert_offset;
        cmds[6] = 0x2F;
        ret = ssd1306_i2c_send_cmd_buf(cmds, 7);
        if (!ret) {
            g_disp_state.scrolling = 1;
            pr_info("i2c_ssd1306: diagonal scroll %s pages %u-%u voff %u\n",
                    cfg.direction ? "left" : "right",
                    cfg.start_page, cfg.end_page, cfg.vert_offset);
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Stop scroll (14)                                                    */
    /* ------------------------------------------------------------------ */
    case SSD1306_IOC_SCROLL_STOP: {
        uint8_t c = 0x2E;
        ret = ssd1306_i2c_send_cmd_buf(&c, 1);
        if (!ret) {
            g_disp_state.scrolling = 0;
            pr_info("i2c_ssd1306: scroll stopped\n");
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    /* GDDRAM addressing mode (15)                                         */
    /* ------------------------------------------------------------------ */
    case SSD1306_IOC_SET_ADDR_MODE: {
        uint8_t mode;
        uint8_t cmds[2];
        if (copy_from_user(&mode, (uint8_t __user *)arg, sizeof(mode))) {
            ret = -EFAULT;
            break;
        }
        if (mode > 2) { ret = -EINVAL; break; }
        cmds[0] = 0x20;
        cmds[1] = mode;
        ret = ssd1306_i2c_send_cmd_buf(cmds, 2);
        if (!ret) {
            g_disp_state.addr_mode = mode;
            pr_info("i2c_ssd1306: addr mode → %u (%s)\n", mode,
                    mode == 0 ? "Horizontal" : mode == 1 ? "Vertical" : "Page");
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Column address window (16)                                          */
    /* ------------------------------------------------------------------ */
    case SSD1306_IOC_SET_COL_RANGE: {
        struct ssd1306_range r;
        uint8_t cmds[3];
        if (copy_from_user(&r, (struct ssd1306_range __user *)arg, sizeof(r))) {
            ret = -EFAULT;
            break;
        }
        if (r.start > 127 || r.end > 127 || r.end < r.start) {
            ret = -EINVAL;
            break;
        }
        cmds[0] = 0x21; cmds[1] = r.start; cmds[2] = r.end;
        ret = ssd1306_i2c_send_cmd_buf(cmds, 3);
        if (!ret)
            pr_info("i2c_ssd1306: column range → %u-%u\n", r.start, r.end);
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Page address window (17)                                            */
    /* ------------------------------------------------------------------ */
    case SSD1306_IOC_SET_PAGE_RANGE: {
        struct ssd1306_range r;
        uint8_t cmds[3];
        if (copy_from_user(&r, (struct ssd1306_range __user *)arg, sizeof(r))) {
            ret = -EFAULT;
            break;
        }
        if (r.start > 7 || r.end > 7 || r.end < r.start) {
            ret = -EINVAL;
            break;
        }
        cmds[0] = 0x22; cmds[1] = r.start; cmds[2] = r.end;
        ret = ssd1306_i2c_send_cmd_buf(cmds, 3);
        if (!ret)
            pr_info("i2c_ssd1306: page range → %u-%u\n", r.start, r.end);
        break;
    }

    default:
        ret = -ENOTTY;
    }

    mutex_unlock(&i2c_mutex);
    return ret;
}

/**
 * driver_write() - Push a data buffer across the I2C bus to the SSD1306.
 *
 * The caller must first use IOCTL SET_CMD_MODE or SET_DATA_MODE to declare
 * whether the payload is commands or pixel data.
 *
 * I2C transaction format (for N bytes):
 *   [START][ADDR W][control_byte][byte_0][byte_1]...[byte_N-1][STOP]
 *
 * Sending all bytes in a single I2C transaction (one START / one STOP) is
 * the most efficient and matches the SSD1306 "multiple data bytes" mode.
 */
static ssize_t driver_write(struct file *file, const char __user *user_buffer,
                             size_t count, loff_t *offs)
{
    uint8_t *kbuf;
    size_t   i;
    ssize_t  ret;
    int      err;

    /* Reject zero-length and oversized transfers */
    if (count == 0 || count > I2C_MAX_WRITE_LEN) {
        pr_err("i2c_ssd1306: write() size %zu out of valid range [1, %d]\n",
               count, I2C_MAX_WRITE_LEN);
        return -EINVAL;
    }

    /* Warn if caller never declared command vs. data mode */
    if (!dc_mode_set)
        pr_warn("i2c_ssd1306: write() called before mode set via IOCTL — "
                "control byte is ambiguous!\n");

    /* Acquire mutex before touching hardware or shared state */
    if (mutex_lock_interruptible(&i2c_mutex))
        return -ERESTARTSYS;

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf) {
        mutex_unlock(&i2c_mutex);
        return -ENOMEM;
    }

    if (copy_from_user(kbuf, user_buffer, count)) {
        kfree(kbuf);
        mutex_unlock(&i2c_mutex);
        return -EFAULT;
    }

    /* --- Begin I2C transaction --- */
    i2c_start();

    /* Address frame: 7-bit slave address + Write bit (0) */
    err = i2c_write_byte((SSD1306_I2C_ADDR << 1) | 0);
    if (err) { ret = err; goto out_stop; }

    /* Control byte: 0x00 = command stream, 0x40 = data stream */
    err = i2c_write_byte(current_ctrl_byte);
    if (err) { ret = err; goto out_stop; }

    /* Payload bytes */
    for (i = 0; i < count; i++) {
        err = i2c_write_byte(kbuf[i]);
        if (err) {
            pr_err("i2c_ssd1306: NACK at payload byte %zu — aborting\n", i);
            ret = err;
            goto out_stop;
        }
    }

    ret = (ssize_t)count;

out_stop:
    i2c_stop(); /* Always STOP to release the bus */
    kfree(kbuf);
    mutex_unlock(&i2c_mutex);
    return ret;
}

static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = driver_open,
    .release        = driver_close,
    .write          = driver_write,
    .unlocked_ioctl = driver_ioctl,
};

/* --- KERNEL MODULE LIFECYCLE --- */

/**
 * ModuleInit() - Load driver, register char device, map RP1, init GPIO.
 *
 * Every registration step checks its return value. On any failure the
 * 'goto err_*' chain unwinds all prior successful registrations in strict
 * reverse order, preventing zombie /dev and /sys entries and resource leaks.
 */
static int __init ModuleInit(void)
{
    int ret;
    pr_info("i2c_ssd1306: Initializing SSD1306 Bit-Banged I2C Driver v2.0...\n");

    /* Allocate a dynamic major/minor number pair */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        pr_err("i2c_ssd1306: alloc_chrdev_region() failed (%d)\n", ret);
        return ret;
    }

    /* Create the /sys/class/<DRIVER_CLASS> entry */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    i2c_class = class_create(DRIVER_CLASS);
#else
    i2c_class = class_create(THIS_MODULE, DRIVER_CLASS);
#endif
    if (IS_ERR(i2c_class)) {
        pr_err("i2c_ssd1306: class_create() failed\n");
        ret = PTR_ERR(i2c_class);
        goto err_chrdev;
    }

    /* Create the /dev/i2c_ssd1306_bitbang device node */
    i2c_device = device_create(i2c_class, NULL, dev_num, NULL, DRIVER_NAME);
    if (IS_ERR(i2c_device)) {
        pr_err("i2c_ssd1306: device_create() failed\n");
        ret = PTR_ERR(i2c_device);
        goto err_class;
    }

    /* Initialise and register the character device */
    cdev_init(&i2c_cdev, &fops);
    ret = cdev_add(&i2c_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("i2c_ssd1306: cdev_add() failed (%d)\n", ret);
        goto err_device;
    }

    /* Map the RP1 peripheral block into kernel virtual address space */
    rp1_base_ptr = ioremap(RP1_PERIPHERAL_BASE, RP1_MAP_SIZE);
    if (!rp1_base_ptr) {
        pr_err("i2c_ssd1306: ioremap() failed for RP1 peripheral base\n");
        ret = -ENOMEM;
        goto err_cdev;
    }

    /* SCL — GPIO 16: pre-load LOW, then release (idle HIGH via pull-up) */
    gpio_set_direction(I2C_SCL_PIN, 1);  /* Output — drives LOW to pre-load RIO */
    gpio_write(I2C_SCL_PIN, 0);          /* Ensure RIO OUT bit is 0 */
    gpio_set_direction(I2C_SCL_PIN, 0);  /* Input pull-up takes SCL HIGH */

    /* SDA — GPIO 17: same open-drain pre-load sequence */
    gpio_set_direction(I2C_SDA_PIN, 1);
    gpio_write(I2C_SDA_PIN, 0);
    gpio_set_direction(I2C_SDA_PIN, 0);  /* Input (high-Z) — pull-up takes SDA HIGH */

    /* Default to command mode; mark dc_mode_set so write() does not warn */
    current_ctrl_byte = SSD1306_CTRL_CMD;
    dc_mode_set = true;

    /* Pump the full initialization command sequence over I2C */
    ret = ssd1306_hardware_init_sequence();
    if (ret) {
        pr_err("i2c_ssd1306: Hardware init sequence failed (%d)\n", ret);
        goto err_ioremap;
    }

    pr_info("i2c_ssd1306: Driver loaded and ready at /dev/%s\n", DRIVER_NAME);
    return 0;

    /*
     * Error unwind — executed only on failure; each label undoes one step
     * in reverse order to guarantee no resource is left orphaned.
     */
err_ioremap:
    iounmap(rp1_base_ptr);
err_cdev:
    cdev_del(&i2c_cdev);
err_device:
    device_destroy(i2c_class, dev_num);
err_class:
    class_destroy(i2c_class);
err_chrdev:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

/**
 * ModuleExit() - Gracefully shut down the display and unregister the driver.
 *
 * i2c_mutex is acquired before any hardware access so we cannot race with
 * a concurrent write() or ioctl() still in flight.
 */
static void __exit ModuleExit(void)
{
    uint8_t sleep_cmd = 0xAE;

    if (rp1_base_ptr) {
        /* Prevent race with in-flight write()/ioctl() */
        mutex_lock(&i2c_mutex);

        /* Send 0xAE (Display OFF / Sleep) for a graceful shutdown */
        ssd1306_i2c_send_cmd_buf(&sleep_cmd, 1);
        /* No RST pin on 4-pin I2C module — sleep command is sufficient */

        mutex_unlock(&i2c_mutex);
        iounmap(rp1_base_ptr);
    }

    cdev_del(&i2c_cdev);
    device_destroy(i2c_class, dev_num);
    class_destroy(i2c_class);
    unregister_chrdev_region(dev_num, 1);

    pr_info("i2c_ssd1306: Driver removed cleanly.\n");
}

module_init(ModuleInit);
module_exit(ModuleExit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pi5 SSD1306 Driver");
MODULE_DESCRIPTION("Bit-Banged SSD1306 I2C Kernel Driver for Raspberry Pi 5 (RP1) "
                   "— contrast, flip, invert, scroll, addressing modes");
MODULE_VERSION("1.0");
