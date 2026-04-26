/**
 * @file ssd1306_ioctl.h
 * @brief Shared IOCTL definitions for the SSD1306 bit-banged SPI kernel driver.
 * All structs use uint8_t / __u8 so the sizes are unambiguous on any arch.
 */
#ifndef SSD1306_IOCTL_H
#define SSD1306_IOCTL_H

#ifdef __KERNEL__
# include <linux/ioctl.h>
# include <linux/types.h>
#else
# include <stdint.h>
# include <sys/ioctl.h>
#endif

/* Magic byte that uniquely identifies this driver's IOCTL namespace */
#define SPI_IOC_MAGIC   's'

/* =========================================================================
 * EXISTING IOCTLs (1 – 7)  — do not renumber, Python scripts depend on these
 * ========================================================================= */

/** Set SPI half-period delay in nanoseconds (arg: uint32_t) */
#define SPI_IOC_SET_FREQ            _IOW(SPI_IOC_MAGIC,  1, uint32_t)

/** Tell driver: subsequent write() bytes are SSD1306 *commands* (DC = LOW) */
#define SSD1306_IOC_SET_CMD_MODE    _IO (SPI_IOC_MAGIC,  2)

/** Tell driver: subsequent write() bytes are pixel *data* (DC = HIGH) */
#define SSD1306_IOC_SET_DATA_MODE   _IO (SPI_IOC_MAGIC,  3)

/** Pulse hardware RST pin LOW→HIGH to force SSD1306 controller reset */
#define SSD1306_IOC_HW_RESET        _IO (SPI_IOC_MAGIC,  4)

/** Send 0xAE — Display OFF / sleep (charge pump stays on, RAM preserved) */
#define SSD1306_IOC_SLEEP           _IO (SPI_IOC_MAGIC,  5)

/** Send 0xAF — Display ON / wake */
#define SSD1306_IOC_WAKEUP          _IO (SPI_IOC_MAGIC,  6)

/** Re-run the full hardware initialisation sequence */
#define SSD1306_IOC_INIT            _IO (SPI_IOC_MAGIC,  7)

/* =========================================================================
 * NEW HARDWARE FEATURE IOCTLs (8 – 17)
 * ========================================================================= */

/**
 * Set display contrast / brightness.
 * arg: uint8_t  0x00 = minimum  |  0x7F = mid (default)  |  0xFF = maximum
 * SSD1306 command: 0x81, <value>
 */
#define SSD1306_IOC_SET_CONTRAST    _IOW(SPI_IOC_MAGIC,  8, uint8_t)

/**
 * Horizontal mirror (column re-map).
 * arg: uint8_t  0 = normal (SEG0→left)  |  1 = mirrored (SEG0→right)
 * SSD1306 command: 0xA0 / 0xA1
 * Note: takes effect immediately with no GDDRAM rewrite.
 */
#define SSD1306_IOC_FLIP_H          _IOW(SPI_IOC_MAGIC,  9, uint8_t)

/**
 * Vertical flip (COM scan direction).
 * arg: uint8_t  0 = normal (top-down)  |  1 = flipped (bottom-up)
 * SSD1306 command: 0xC0 / 0xC8
 * Combine with FLIP_H for a full 180° hardware rotation.
 */
#define SSD1306_IOC_FLIP_V          _IOW(SPI_IOC_MAGIC, 10, uint8_t)

/**
 * Invert all pixels (hardware — no GDDRAM change).
 * arg: uint8_t  0 = normal (1=ON)  |  1 = inverted (0=ON, white background)
 * SSD1306 command: 0xA6 / 0xA7
 */
#define SSD1306_IOC_INVERT          _IOW(SPI_IOC_MAGIC, 11, uint8_t)

/**
 * Start autonomous horizontal scroll.
 * arg: struct ssd1306_scroll_cfg  (vert_offset field is ignored)
 * SSD1306 commands: 0x2E (stop), 0x26/0x27 (configure), 0x2F (activate)
 * WARNING: Do NOT write to GDDRAM while scroll is active — RAM corruption!
 *          Call SSD1306_IOC_SCROLL_STOP before any framebuffer write.
 */
#define SSD1306_IOC_SCROLL_H        _IOW(SPI_IOC_MAGIC, 12, struct ssd1306_scroll_cfg)

/**
 * Start autonomous diagonal (vertical + horizontal) scroll.
 * arg: struct ssd1306_scroll_cfg  (all fields used including vert_offset)
 * SSD1306 commands: 0x2E, 0xA3 (scroll area), 0x29/0x2A, 0x2F
 */
#define SSD1306_IOC_SCROLL_DIAG     _IOW(SPI_IOC_MAGIC, 13, struct ssd1306_scroll_cfg)

/**
 * Stop any active scroll immediately.
 * Must be called before writing pixel data to GDDRAM.
 * SSD1306 command: 0x2E
 */
#define SSD1306_IOC_SCROLL_STOP     _IO (SPI_IOC_MAGIC, 14)

/**
 * Set GDDRAM addressing mode.
 * arg: uint8_t  0 = Horizontal  |  1 = Vertical  |  2 = Page (default)
 * SSD1306 command: 0x20, <mode>
 *   Horizontal: pointer auto-wraps column then page — best for full framebuffer writes
 *   Page:       pointer stays in page — best for single-line partial updates
 */
#define SSD1306_IOC_SET_ADDR_MODE   _IOW(SPI_IOC_MAGIC, 15, uint8_t)

/**
 * Set active column address window (Horizontal / Vertical modes).
 * arg: struct ssd1306_range  { start: 0–127, end: 0–127 }
 * SSD1306 command: 0x21, start, end
 */
#define SSD1306_IOC_SET_COL_RANGE   _IOW(SPI_IOC_MAGIC, 16, struct ssd1306_range)

/**
 * Set active page address window (Horizontal / Vertical modes).
 * arg: struct ssd1306_range  { start: 0–7, end: 0–7 }
 * SSD1306 command: 0x22, start, end
 */
#define SSD1306_IOC_SET_PAGE_RANGE  _IOW(SPI_IOC_MAGIC, 17, struct ssd1306_range)

/* =========================================================================
 * PARAMETER STRUCTURES
 * ========================================================================= */

/**
 * struct ssd1306_scroll_cfg - Configuration for hardware scroll commands.
 *
 * @direction:   Scroll direction. 0 = right,  1 = left.
 * @start_page:  First page included in scroll window (0–7).
 * @end_page:    Last  page included in scroll window (0–7). Must be >= start_page.
 * @speed:       Frame-interval code from datasheet:
 *                 0x07 = 2 frames  (fastest)
 *                 0x04 = 5 frames
 *                 0x03 = 25 frames
 *                 0x02 = 64 frames
 *                 0x00 = 256 frames (slowest / crawl)
 * @vert_offset: Rows shifted per step in diagonal mode (1–63).
 *               Ignored for SSD1306_IOC_SCROLL_H (pure horizontal).
 */
struct ssd1306_scroll_cfg {
    uint8_t direction;
    uint8_t start_page;
    uint8_t end_page;
    uint8_t speed;
    uint8_t vert_offset;
};

/**
 * struct ssd1306_range - Generic start/end address pair.
 *
 * Used for:
 *   SSD1306_IOC_SET_COL_RANGE  — column window (0–127)
 *   SSD1306_IOC_SET_PAGE_RANGE — page window   (0–7)
 *
 * @start: First address (inclusive).
 * @end:   Last  address (inclusive). Must be >= start.
 */
struct ssd1306_range {
    uint8_t start;
    uint8_t end;
};

#endif /* SSD1306_IOCTL_H */
