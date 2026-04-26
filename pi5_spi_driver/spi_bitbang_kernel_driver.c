/**
 * @file spi_bitbang_kernel_driver.c
 * @brief Raspberry Pi 5 Kernel-Space Bit-Banged SPI Driver for SSD1306 OLED
 *
 * This Linux Kernel Module (LKM) implements a bit-banged 4-Wire SPI protocol
 * specifically tailored for an SSD1306 display.  It exposes the following
 * hardware features of the SSD1306 controller via IOCTL:
 *
 *   - SPI clock frequency control
 *   - Command / Data (D/C) mode switching
 *   - Hardware RST, Sleep, Wakeup, Init
 *   - Contrast (256 levels)                 [NEW v3.0]
 *   - Horizontal mirror  (H-flip)           [NEW v3.0]
 *   - Vertical flip      (V-flip / 180°)    [NEW v3.0]
 *   - Pixel inversion                        [NEW v3.0]
 *   - Autonomous H-scroll & diagonal scroll [NEW v3.0]
 *   - GDDRAM addressing mode (H/V/Page)     [NEW v3.0]
 *   - Column & page address windowing       [NEW v3.0]
 *
 * See ssd1306_ioctl.h for the full IOCTL command list and parameter structs.
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
#include <linux/mutex.h>        /* Mutex for exclusive SPI bus protection */

/* ============================================================================
2. MACROS & DEFINITIONS
============================================================================ */

#define DRIVER_NAME "spi_ssd1306_bitbang"
#define DRIVER_CLASS "spi_ssd1306_class"

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
 *
 */
#define RIO_OFFSET              0x0e0000
#define RIO_OUT                 0x0000
#define RIO_OE                  0x0004
#define RIO_IN                  0x0008
#define RIO_XOR_ALIAS           0x1000   /* XOR/toggle — confirmed */
#define RIO_SET_ALIAS           0x2000   /* atomic SET — hardware verified */
#define RIO_CLR_ALIAS           0x3000   /* atomic CLR — hardware verified */
#define RIO_OUT_SET             (RIO_SET_ALIAS + RIO_OUT)   /* 0x2000 */
#define RIO_OE_SET              (RIO_SET_ALIAS + RIO_OE)    /* 0x2004 */
#define RIO_OUT_CLR             (RIO_CLR_ALIAS + RIO_OUT)   /* 0x3000 */
#define RIO_OE_CLR              (RIO_CLR_ALIAS + RIO_OE)    /* 0x3004 */

/*
 * Custom GPIO Pins assigned for 4-Wire SPI (SSD1306)
 * Note: MISO is intentionally omitted as SSD1306 receives data only.
 */
#define SPI_SCLK_PIN            17        /* Serial Clock         (GPIO 17) */
#define SPI_MOSI_PIN            27        /* Master Out Slave In  (GPIO 27) */
#define SPI_CS_PIN              22        /* Chip Select          (GPIO 22) */
#define SSD1306_DC_PIN          5         /* Data / Command toggle (GPIO  5) */
#define SSD1306_RST_PIN         6         /* Hardware Reset pin    (GPIO  6) */

/* All IOCTL definitions and parameter structs live in the shared header */
#include "ssd1306_ioctl.h"

/*
 * Maximum permitted write() payload.
 * SSD1306 full frame = 1024 bytes; 4096 gives headroom for init sequences
 * while preventing unbounded kmalloc() calls from user space.
 */
#define SPI_MAX_WRITE_LEN           4096

/* ============================================================================
3. VARIABLE DECLARATIONS
============================================================================ */

static dev_t dev_num;
static struct cdev spi_cdev;
static struct class *spi_class = NULL;
static struct device *spi_device = NULL;
static void __iomem *rp1_base_ptr;

/* Dynamic tracker for SPI bus speed (Defaults to 500ns gap ~ 1MHz) */
static uint32_t current_spi_delay_ns = 500;

/*
 * D/C mode guard.
 * Set to true the first time the user calls SET_CMD_MODE or SET_DATA_MODE.
 * driver_write() checks this and emits a kernel warning if it is still false,
 * indicating the caller forgot to declare the transfer type via IOCTL.
 */
static bool dc_mode_set = false;

/*
 * Mutex protecting concurrent access to the SPI bus, GPIO registers,
 * and the shared current_spi_delay_ns variable. Prevents bit-stream corruption
 * when two processes open /dev/spi_ssd1306_bitbang simultaneously.
 */
static DEFINE_MUTEX(spi_mutex);

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

/* --- HARDWARE INTERACTION FUNCTIONS --- */

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

/**
 * spi_transfer_byte() - Bit-bang one byte MSB-first over the SPI bus.
 */
static void spi_transfer_byte(uint8_t data_out)
{
    int i;
    for (i = 7; i >= 0; i--) {
        /* Present data bit on MOSI */
        gpio_write(SPI_MOSI_PIN, (data_out >> i) & 1);
        ndelay(current_spi_delay_ns / 2);

        /* Rising clock edge — SSD1306 samples on this edge */
        gpio_write(SPI_SCLK_PIN, 1);
        ndelay(current_spi_delay_ns / 2);

        /* Falling clock edge */
        gpio_write(SPI_SCLK_PIN, 0);

        /* Hold time — keep MOSI stable after clock falls */
        ndelay(current_spi_delay_ns / 2);
    }
}

/**
 * ssd1306_send_cmd_buf() - Send a sequence of SSD1306 command bytes over SPI.
 *
 * Handles D/C pin (LOW = command), CS assert/deassert, and the PCIe settle
 * delays.  All IOCTL cases that send SSD1306 commands use this helper to
 * avoid duplicated CS/DC boilerplate.
 *
 * Caller MUST hold spi_mutex before calling.
 *
 * @cmds: pointer to command byte array
 * @len:  number of bytes to send
 */
static void ssd1306_send_cmd_buf(const uint8_t *cmds, int len)
{
    int i;
    gpio_write(SSD1306_DC_PIN, 0);   /* D/C LOW  → command mode   */
    gpio_write(SPI_CS_PIN,     0);   /* CS  LOW  → begin transfer  */
    ndelay(current_spi_delay_ns);
    for (i = 0; i < len; i++)
        spi_transfer_byte(cmds[i]);
    ndelay(current_spi_delay_ns);
    gpio_write(SPI_CS_PIN, 1);       /* CS  HIGH → end transfer    */
    dc_mode_set = true;              /* DC is now in command mode   */
}


/**
 * ssd1306_hardware_init_sequence() - Push the SSD1306 power-on init sequence.
 *
 * Initialises display geometry, charge-pump, contrast, and turns the panel on.
 * Must be called while spi_mutex is held by the caller (ModuleInit acquires it
 * indirectly because no other task can have opened the device yet at that point).
 */
static void ssd1306_hardware_init_sequence(void)
{
    uint8_t init_cmds[] = {
        0xAE,       /*  1. Display OFF (Sleep mode)                             */
        0xD5, 0x80, /*  2. Clock divide ratio / oscillator frequency            */
        0xA8, 0x3F, /*  3. Multiplex ratio  (0x3F = 64 rows for 128x64)        */
        0xD3, 0x00, /*  4. Display offset   (0 = no vertical shift)            */
        0x40,       /*  5. Display start line = 0                               */
        0x8D, 0x14, /*  6. ENABLE CHARGE PUMP  (critical — screen is black without) */
        0x20, 0x00, /*  7. Memory addressing mode: Horizontal                   */
        0xA1,       /*  8. Segment re-map (column 127 → SEG0)                  */
        0xC8,       /*  9. COM output scan direction (remapped)                 */
        0xDA, 0x12, /* 10. COM pins hardware configuration                      */
        0x81, 0x7F, /* 11. Contrast control (0x7F = mid-level)                  */
        0xD9, 0xF1, /* 12. Pre-charge period                                    */
        0xDB, 0x40, /* 13. VCOMH deselect level                                 */
        0xA4,       /* 14. Entire display ON — resume from RAM content          */
        0xA6,       /* 15. Normal (non-inverted) display                        */
        0xAF        /* 16. DISPLAY ON — wake panel up                           */
    };
    int i;

    gpio_write(SSD1306_DC_PIN, 0); /* D/C LOW  → command mode */
    gpio_write(SPI_CS_PIN,     0); /* CS  LOW  → begin transaction */
    ndelay(current_spi_delay_ns);

    for (i = 0; i < (int)sizeof(init_cmds); i++)
        spi_transfer_byte(init_cmds[i]);

    ndelay(current_spi_delay_ns);
    gpio_write(SPI_CS_PIN, 1);     /* CS HIGH → end transaction */
    pr_info("spi_ssd1306: Initialization sequence written to display\n");
}

/* --- VFS FILE OPERATIONS --- */

/**
 * driver_open() - Validate the open mode before allowing access.
 */
static int driver_open(struct inode *device_file, struct file *instance)
{
    if ((instance->f_flags & O_ACCMODE) == O_RDONLY) {
        pr_warn("spi_ssd1306: Rejected O_RDONLY open — device is write-only\n");
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
 */
static long driver_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long ret = 0;

    if (mutex_lock_interruptible(&spi_mutex))
        return -ERESTARTSYS;

    switch (cmd) {

    case SPI_IOC_SET_FREQ: {
        uint32_t user_delay_ns;
        if (copy_from_user(&user_delay_ns, (uint32_t __user *)arg,
                           sizeof(user_delay_ns))) {
            ret = -EFAULT;
            break;
        }
        if (user_delay_ns < 10)
            user_delay_ns = 10;
        current_spi_delay_ns = user_delay_ns;
        pr_info("spi_ssd1306: SPI delay updated to %u ns\n",
                current_spi_delay_ns);
        break;
    }

    case SSD1306_IOC_SET_CMD_MODE:
        gpio_write(SSD1306_DC_PIN, 0);
        dc_mode_set = true;
        break;

    case SSD1306_IOC_SET_DATA_MODE:
        gpio_write(SSD1306_DC_PIN, 1);
        dc_mode_set = true;
        break;

    case SSD1306_IOC_HW_RESET:
        gpio_write(SSD1306_RST_PIN, 0);
        msleep(10);
        gpio_write(SSD1306_RST_PIN, 1);
        msleep(50);
        pr_info("spi_ssd1306: Hardware reset complete\n");
        break;

    case SSD1306_IOC_SLEEP: {
        uint8_t c = 0xAE;
        ssd1306_send_cmd_buf(&c, 1);
        pr_info("spi_ssd1306: OLED sleep (0xAE)\n");
        break;
    }

    case SSD1306_IOC_WAKEUP: {
        uint8_t c = 0xAF;
        ssd1306_send_cmd_buf(&c, 1);
        pr_info("spi_ssd1306: OLED wakeup (0xAF)\n");
        break;
    }

    case SSD1306_IOC_INIT:
        ssd1306_hardware_init_sequence();
        break;
    
        case SSD1306_IOC_SET_CONTRAST: {
        uint8_t contrast;
        uint8_t cmds[2];
        if (copy_from_user(&contrast, (uint8_t __user *)arg, sizeof(contrast))) {
            ret = -EFAULT;
            break;
        }
        cmds[0] = 0x81;
        cmds[1] = contrast;
        ssd1306_send_cmd_buf(cmds, 2);
        g_disp_state.contrast = contrast;
        pr_info("spi_ssd1306: contrast → %u\n", contrast);
        break;
    }

    case SSD1306_IOC_FLIP_H: {
        uint8_t enable, cmd_byte;
        if (copy_from_user(&enable, (uint8_t __user *)arg, sizeof(enable))) {
            ret = -EFAULT;
            break;
        }
        cmd_byte = enable ? 0xA1 : 0xA0;
        ssd1306_send_cmd_buf(&cmd_byte, 1);
        g_disp_state.flip_h = enable ? 1 : 0;
        pr_info("spi_ssd1306: H-flip %s\n", enable ? "ON" : "OFF");
        break;
    }

    case SSD1306_IOC_FLIP_V: {
        uint8_t enable, cmd_byte;
        if (copy_from_user(&enable, (uint8_t __user *)arg, sizeof(enable))) {
            ret = -EFAULT;
            break;
        }
        cmd_byte = enable ? 0xC8 : 0xC0;
        ssd1306_send_cmd_buf(&cmd_byte, 1);
        g_disp_state.flip_v = enable ? 1 : 0;
        pr_info("spi_ssd1306: V-flip %s\n", enable ? "ON" : "OFF");
        break;
    }

    case SSD1306_IOC_INVERT: {
        uint8_t enable, cmd_byte;
        if (copy_from_user(&enable, (uint8_t __user *)arg, sizeof(enable))) {
            ret = -EFAULT;
            break;
        }
        cmd_byte = enable ? 0xA7 : 0xA6;
        ssd1306_send_cmd_buf(&cmd_byte, 1);
        g_disp_state.inverted = enable ? 1 : 0;
        pr_info("spi_ssd1306: display %s\n", enable ? "inverted" : "normal");
        break;
    }

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
        ssd1306_send_cmd_buf(&stop, 1);
        cmds[0] = cfg.direction ? 0x27 : 0x26; /* 0x27=left, 0x26=right */
        cmds[1] = 0x00;                          /* dummy A               */
        cmds[2] = cfg.start_page;
        cmds[3] = cfg.speed;
        cmds[4] = cfg.end_page;
        cmds[5] = 0x00;                          /* dummy B               */
        cmds[6] = 0xFF;                          /* dummy C               */
        cmds[7] = 0x2F;                          /* activate              */
        ssd1306_send_cmd_buf(cmds, 8);
        g_disp_state.scrolling = 1;
        pr_info("spi_ssd1306: H-scroll %s pages %u-%u speed 0x%02x\n",
                cfg.direction ? "left" : "right",
                cfg.start_page, cfg.end_page, cfg.speed);
        break;
    }

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
        ssd1306_send_cmd_buf(&stop, 1);
        /* Set vertical scroll area: 0 fixed rows, all 64 rows scroll */
        area[0] = 0xA3; area[1] = 0x00; area[2] = 0x40;
        ssd1306_send_cmd_buf(area, 3);
        cmds[0] = cfg.direction ? 0x2A : 0x29; /* 0x2A=left, 0x29=right */
        cmds[1] = 0x00;
        cmds[2] = cfg.start_page;
        cmds[3] = cfg.speed;
        cmds[4] = cfg.end_page;
        cmds[5] = cfg.vert_offset;
        cmds[6] = 0x2F;
        ssd1306_send_cmd_buf(cmds, 7);
        g_disp_state.scrolling = 1;
        pr_info("spi_ssd1306: diagonal scroll %s pages %u-%u voff %u\n",
                cfg.direction ? "left" : "right",
                cfg.start_page, cfg.end_page, cfg.vert_offset);
        break;
    }

    case SSD1306_IOC_SCROLL_STOP: {
        uint8_t c = 0x2E;
        ssd1306_send_cmd_buf(&c, 1);
        g_disp_state.scrolling = 0;
        pr_info("spi_ssd1306: scroll stopped\n");
        break;
    }

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
        ssd1306_send_cmd_buf(cmds, 2);
        g_disp_state.addr_mode = mode;
        pr_info("spi_ssd1306: addr mode → %u (%s)\n", mode,
                mode == 0 ? "Horizontal" : mode == 1 ? "Vertical" : "Page");
        break;
    }

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
        ssd1306_send_cmd_buf(cmds, 3);
        pr_info("spi_ssd1306: column range → %u-%u\n", r.start, r.end);
        break;
    }

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
        ssd1306_send_cmd_buf(cmds, 3);
        pr_info("spi_ssd1306: page range → %u-%u\n", r.start, r.end);
        break;
    }

    default:
        ret = -ENOTTY;
    }

    mutex_unlock(&spi_mutex);
    return ret;
}

/**
 * @brief Pushes data buffer across the bus. Relies on the user calling IOCTL first 
 * to declare if this buffer is Data or Config Commands.
 */
static ssize_t driver_write(struct file *file, const char __user *user_buffer,
                            size_t count, loff_t *offs)
{
    uint8_t *kbuf;
    size_t   i;
    ssize_t  ret;

    /* Reject zero-length and oversized transfers */
    if (count == 0 || count > SPI_MAX_WRITE_LEN) {
        pr_err("spi_ssd1306: write() size %zu out of valid range [1, %d]\n",
               count, SPI_MAX_WRITE_LEN);
        return -EINVAL;
    }

    /* Warn if caller never declared command vs. data mode */
    if (!dc_mode_set)
        pr_warn("spi_ssd1306: write() called before D/C mode set via IOCTL — "
                "bus state is ambiguous!\n");

    /* Acquire mutex before touching hardware or shared state */
    if (mutex_lock_interruptible(&spi_mutex))
        return -ERESTARTSYS;

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf) {
        mutex_unlock(&spi_mutex);
        return -ENOMEM;
    }

    if (copy_from_user(kbuf, user_buffer, count)) {
        kfree(kbuf);
        mutex_unlock(&spi_mutex);
        return -EFAULT;
    }

    gpio_write(SPI_CS_PIN, 0);         /* Assert CS — begin transaction */
    ndelay(current_spi_delay_ns);

    for (i = 0; i < count; i++) {
        spi_transfer_byte(kbuf[i]);
    }

    ndelay(current_spi_delay_ns);
    gpio_write(SPI_CS_PIN, 1);         /* De-assert CS — end transaction */

    ret = (ssize_t)count;
    kfree(kbuf);
    mutex_unlock(&spi_mutex);
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
 * On any failure the 'goto err_*' chain unwinds all prior successful
 * registrations in strict reverse order, preventing zombie /dev and
 * /sys entries and resource leaks.
 */
static int __init ModuleInit(void)
{
    int ret;
    pr_info("spi_ssd1306: Initializing SSD1306 Bit-Banged SPI Driver v2.1...\n");

    /* Step 1: Allocate a dynamic major/minor number pair */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        pr_err("spi_ssd1306: alloc_chrdev_region() failed (%d)\n", ret);
        return ret;
    }

    /* Step 2: Create the /sys/class/<DRIVER_CLASS> entry */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    spi_class = class_create(DRIVER_CLASS);
#else
    spi_class = class_create(THIS_MODULE, DRIVER_CLASS);
#endif
    if (IS_ERR(spi_class)) {
        pr_err("spi_ssd1306: class_create() failed\n");
        ret = PTR_ERR(spi_class);
        goto err_chrdev;
    }

    /* Step 3: Create the /dev/spi_ssd1306_bitbang device node */
    spi_device = device_create(spi_class, NULL, dev_num, NULL, DRIVER_NAME);
    if (IS_ERR(spi_device)) {
        pr_err("spi_ssd1306: device_create() failed\n");
        ret = PTR_ERR(spi_device);
        goto err_class;
    }

    /* Step 4: Initialise and register the character device */
    cdev_init(&spi_cdev, &fops);
    ret = cdev_add(&spi_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("spi_ssd1306: cdev_add() failed (%d)\n", ret);
        goto err_device;
    }

    /* Step 5: Map the RP1 peripheral block into kernel virtual address space */
    rp1_base_ptr = ioremap(RP1_PERIPHERAL_BASE, RP1_MAP_SIZE);
    if (!rp1_base_ptr) {
        pr_err("spi_ssd1306: ioremap() failed for RP1 peripheral base\n");
        ret = -ENOMEM;
        goto err_cdev; /* cdev must be deleted before returning */
    }

    /* Step 6: Configure all five GPIO lines as SIO outputs */
    gpio_set_direction(SPI_SCLK_PIN,  1);
    gpio_set_direction(SPI_MOSI_PIN,  1);
    gpio_set_direction(SPI_CS_PIN,    1);
    gpio_set_direction(SSD1306_DC_PIN, 1);
    gpio_set_direction(SSD1306_RST_PIN, 1);

    /* Step 7: Drive all lines to safe idle state */
    gpio_write(SPI_CS_PIN,   1);  /* CS idle-high */
    gpio_write(SPI_SCLK_PIN, 0);  /* Clock idle-low (CPOL = 0) */
    gpio_write(SPI_MOSI_PIN, 0);  /* MOSI idle-low */

    /* Step 8: Hardware reset sequence (ensures internal controller state is clean) */
    pr_info("spi_ssd1306: Applying SSD1306 hardware reset...\n");
    gpio_write(SSD1306_RST_PIN, 0);
    msleep(10);
    gpio_write(SSD1306_RST_PIN, 1);
    msleep(50);

    /* Step 9: Default to command mode; mark dc_mode_set so write() does not warn */
    gpio_write(SSD1306_DC_PIN, 0);
    dc_mode_set = true;

    /* Step 10: Pump the full initialization command sequence over SPI */
    ssd1306_hardware_init_sequence();

    pr_info("spi_ssd1306: Driver loaded and ready at /dev/%s\n", DRIVER_NAME);
    return 0;

    /*
     * Error unwind — executed only on failure; each label undoes one step
     * in reverse order to guarantee no resource is left orphaned.
     */
err_cdev:
    cdev_del(&spi_cdev);
err_device:
    device_destroy(spi_class, dev_num);
err_class:
    class_destroy(spi_class);
err_chrdev:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

/**
 * ModuleExit() - Gracefully shut down the display and unregister the driver.
 *
 * spi_mutex acquired before any hardware access so we cannot race
 * with a concurrent write() or ioctl() still in flight.
 * CS is explicitly released (HIGH) before iounmap().
 */
static void __exit ModuleExit(void)
{
    if (rp1_base_ptr) {
        /* Prevent race with in-flight write()/ioctl() */
        mutex_lock(&spi_mutex);

        /* 1. Send 0xAE (Display OFF / Sleep) for a graceful shutdown */
        gpio_write(SSD1306_DC_PIN, 0); /* Command mode */
        gpio_write(SPI_CS_PIN,     0); /* Begin transaction */
        ndelay(current_spi_delay_ns);
        spi_transfer_byte(0xAE);       /* Sleep command */
        ndelay(current_spi_delay_ns);

        /* 2. Release CS, idle clock; pull RST low to power-gate the panel */
        gpio_write(SPI_CS_PIN,      1); /* CS released before iounmap */
        gpio_write(SPI_SCLK_PIN,    0);
        gpio_write(SSD1306_RST_PIN, 0); /* RST LOW — panel shuts off cleanly */

        mutex_unlock(&spi_mutex);
        iounmap(rp1_base_ptr);
    }

    cdev_del(&spi_cdev);
    device_destroy(spi_class, dev_num);
    class_destroy(spi_class);
    unregister_chrdev_region(dev_num, 1);

    pr_info("spi_ssd1306: Driver removed cleanly.\n");
}

module_init(ModuleInit);
module_exit(ModuleExit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pi5 SSD1306 Driver");
MODULE_DESCRIPTION("Bit-Banged SSD1306 SPI Kernel Driver for Raspberry Pi 5 (RP1) "
                   "— contrast, flip, invert, scroll, addressing modes");
MODULE_VERSION("1.0");
