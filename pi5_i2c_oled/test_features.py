#!/usr/bin/env python3
"""
test_features.py  —  SSD1306 I2C v2.0 Interactive Feature Demo
================================================================
Demonstrates every hardware feature exposed by the I2C bit-bang
kernel driver via IOCTL.  Renders real text on the display so
each demo is self-explanatory on screen.

Run:
    sudo python3 test_features.py

Device: /dev/i2c_ssd1306_bitbang  (loaded via i2c_bitbang_kernel_driver.ko)
Wiring (4-pin I2C OLED — VCC / GND / SCL / SDA):
    GPIO 23 (Pin 16) → SCL  +  4.7 kΩ pull-up to 3.3 V
    GPIO 24 (Pin 18) → SDA  +  4.7 kΩ pull-up to 3.3 V
    Pin  1  3.3 V    → VCC
    Pin  6  GND      → GND

GPIO 23/24 chosen to avoid conflict with SPI driver pins {17, 27, 22, 5, 6}.
Both drivers can be loaded simultaneously without any GPIO clash.

Note: There is no RST pin on a standard 4-pin I2C module.
The HW_RESET IOCTL performs a software reset (re-sends the full init sequence).
"""

import fcntl
import struct
import time
import sys
import os

# ============================================================================
# IOCTL helpers  (mirrors i2c_bitbang_kernel_driver.c macro definitions)
# ============================================================================

_IOC_NONE  = 0
_IOC_WRITE = 1

def _IOC(d, t, n, s): return (d << 30) | (s << 16) | (ord(t) << 8) | n
def _IO (t, n):        return _IOC(_IOC_NONE,  t, n, 0)
def _IOW(t, n, s):     return _IOC(_IOC_WRITE, t, n, s)

# Magic byte 'i' — matches I2C_IOC_MAGIC in the kernel driver
M = 'i'

I2C_IOC_SET_FREQ          = _IOW(M,  1, 4)   # uint32_t half-period ns
SSD1306_IOC_SET_CMD_MODE  = _IO (M,  2)
SSD1306_IOC_SET_DATA_MODE = _IO (M,  3)
SSD1306_IOC_HW_RESET      = _IO (M,  4)
SSD1306_IOC_SLEEP         = _IO (M,  5)
SSD1306_IOC_WAKEUP        = _IO (M,  6)
SSD1306_IOC_INIT          = _IO (M,  7)
SSD1306_IOC_SET_CONTRAST  = _IOW(M,  8, 1)   # uint8_t 0..255
SSD1306_IOC_FLIP_H        = _IOW(M,  9, 1)   # uint8_t 0=normal 1=mirror
SSD1306_IOC_FLIP_V        = _IOW(M, 10, 1)   # uint8_t 0=normal 1=flip
SSD1306_IOC_INVERT        = _IOW(M, 11, 1)   # uint8_t 0=normal 1=inverted
SSD1306_IOC_SCROLL_H      = _IOW(M, 12, 5)   # struct ssd1306_scroll_cfg
SSD1306_IOC_SCROLL_DIAG   = _IOW(M, 13, 5)   # struct ssd1306_scroll_cfg
SSD1306_IOC_SCROLL_STOP   = _IO (M, 14)
SSD1306_IOC_SET_ADDR_MODE = _IOW(M, 15, 1)   # uint8_t 0=H 1=V 2=Page
SSD1306_IOC_SET_COL_RANGE = _IOW(M, 16, 2)   # struct ssd1306_range {u8,u8}
SSD1306_IOC_SET_PAGE_RANGE= _IOW(M, 17, 2)   # struct ssd1306_range {u8,u8}

DEVICE_PATH = "/dev/i2c_ssd1306_bitbang"
_fd = None

def open_device():
    global _fd
    if not os.path.exists(DEVICE_PATH):
        print(f"ERROR: {DEVICE_PATH} not found.")
        print("  Is the module loaded?  ->  sudo insmod i2c_bitbang_kernel_driver.ko")
        sys.exit(1)
    _fd = open(DEVICE_PATH, "wb", buffering=0)

def close_device():
    global _fd
    if _fd:
        _fd.close()
        _fd = None

# ---- IOCTL wrappers ----

def ioc_io(cmd):
    """Fire an argument-less IOCTL (_IO type)."""
    fcntl.ioctl(_fd, cmd, 0)

def ioc_u8(cmd, v):
    """Fire an IOCTL that takes a single uint8_t argument."""
    fcntl.ioctl(_fd, cmd, struct.pack('B', v & 0xFF))

def ioc_u32(cmd, v):
    """Fire an IOCTL that takes a uint32_t argument (little-endian)."""
    fcntl.ioctl(_fd, cmd, struct.pack('<I', v))

def ioc_scroll(cmd, direction, start_page, end_page, speed, vert_offset=1):
    """Fire a scroll IOCTL (maps to struct ssd1306_scroll_cfg)."""
    fcntl.ioctl(_fd, cmd,
                struct.pack('BBBBB', direction, start_page, end_page,
                            speed, vert_offset))

def ioc_range(cmd, start, end):
    """Fire a range IOCTL (maps to struct ssd1306_range)."""
    fcntl.ioctl(_fd, cmd, struct.pack('BB', start, end))

def set_i2c_speed(half_period_ns: int):
    """Adjust I2C SCL frequency.  Default 2500 ns → ~100 kHz Standard Mode."""
    ioc_u32(I2C_IOC_SET_FREQ, half_period_ns)

# ============================================================================
# I2C page-mode address helper
# ----------------------------------------------------------------------------
# In page-mode the SSD1306 uses three command bytes to position the cursor:
#   0xB0|page  — set page address (0–7)
#   0x00|lo    — set column lower nibble
#   0x10|hi    — set column upper nibble
# We send these as a raw command stream via SET_CMD_MODE + write().
# This is I2C-specific because SPI uses SET_CMD_MODE + DC-pin, but both
# drivers expose the same write() interface so the sequence is identical.
# ============================================================================

def i2c_page_mode_goto(page: int, col: int = 0):
    """Position cursor in Page Addressing Mode (mode 2)."""
    lo = col & 0x0F
    hi = (col >> 4) & 0x0F
    ioc_io(SSD1306_IOC_SET_CMD_MODE)
    _fd.write(bytes([0xB0 | (page & 0x07), 0x00 | lo, 0x10 | hi]))

# ============================================================================
# 5×7 bitmap font  (ASCII 32–127)
# Each character = 5 bytes representing 5 columns.
# Bit-0 of each byte = topmost pixel, bit-6 = bottom.
# Directly compatible with SSD1306 GDDRAM page layout.
# ============================================================================

FONT = [
    [0x00,0x00,0x00,0x00,0x00],  # 0x20  (space)
    [0x00,0x00,0x5F,0x00,0x00],  # 0x21  !
    [0x00,0x07,0x00,0x07,0x00],  # 0x22  "
    [0x14,0x7F,0x14,0x7F,0x14],  # 0x23  #
    [0x24,0x2A,0x7F,0x2A,0x12],  # 0x24  $
    [0x23,0x13,0x08,0x64,0x62],  # 0x25  %
    [0x36,0x49,0x55,0x22,0x50],  # 0x26  &
    [0x00,0x05,0x03,0x00,0x00],  # 0x27  '
    [0x00,0x1C,0x22,0x41,0x00],  # 0x28  (
    [0x00,0x41,0x22,0x1C,0x00],  # 0x29  )
    [0x14,0x08,0x3E,0x08,0x14],  # 0x2A  *
    [0x08,0x08,0x3E,0x08,0x08],  # 0x2B  +
    [0x00,0x50,0x30,0x00,0x00],  # 0x2C  ,
    [0x08,0x08,0x08,0x08,0x08],  # 0x2D  -
    [0x00,0x60,0x60,0x00,0x00],  # 0x2E  .
    [0x20,0x10,0x08,0x04,0x02],  # 0x2F  /
    [0x3E,0x51,0x49,0x45,0x3E],  # 0x30  0
    [0x00,0x42,0x7F,0x40,0x00],  # 0x31  1
    [0x42,0x61,0x51,0x49,0x46],  # 0x32  2
    [0x21,0x41,0x45,0x4B,0x31],  # 0x33  3
    [0x18,0x14,0x12,0x7F,0x10],  # 0x34  4
    [0x27,0x45,0x45,0x45,0x39],  # 0x35  5
    [0x3C,0x4A,0x49,0x49,0x30],  # 0x36  6
    [0x01,0x71,0x09,0x05,0x03],  # 0x37  7
    [0x36,0x49,0x49,0x49,0x36],  # 0x38  8
    [0x06,0x49,0x49,0x29,0x1E],  # 0x39  9
    [0x00,0x36,0x36,0x00,0x00],  # 0x3A  :
    [0x00,0x56,0x36,0x00,0x00],  # 0x3B  ;
    [0x08,0x14,0x22,0x41,0x00],  # 0x3C  <
    [0x14,0x14,0x14,0x14,0x14],  # 0x3D  =
    [0x00,0x41,0x22,0x14,0x08],  # 0x3E  >
    [0x02,0x01,0x51,0x09,0x06],  # 0x3F  ?
    [0x32,0x49,0x79,0x41,0x3E],  # 0x40  @
    [0x7E,0x11,0x11,0x11,0x7E],  # 0x41  A
    [0x7F,0x49,0x49,0x49,0x36],  # 0x42  B
    [0x3E,0x41,0x41,0x41,0x22],  # 0x43  C
    [0x7F,0x41,0x41,0x22,0x1C],  # 0x44  D
    [0x7F,0x49,0x49,0x49,0x41],  # 0x45  E
    [0x7F,0x09,0x09,0x09,0x01],  # 0x46  F
    [0x3E,0x41,0x49,0x49,0x7A],  # 0x47  G
    [0x7F,0x08,0x08,0x08,0x7F],  # 0x48  H
    [0x00,0x41,0x7F,0x41,0x00],  # 0x49  I
    [0x20,0x40,0x41,0x3F,0x01],  # 0x4A  J
    [0x7F,0x08,0x14,0x22,0x41],  # 0x4B  K
    [0x7F,0x40,0x40,0x40,0x40],  # 0x4C  L
    [0x7F,0x02,0x0C,0x02,0x7F],  # 0x4D  M
    [0x7F,0x04,0x08,0x10,0x7F],  # 0x4E  N
    [0x3E,0x41,0x41,0x41,0x3E],  # 0x4F  O
    [0x7F,0x09,0x09,0x09,0x06],  # 0x50  P
    [0x3E,0x41,0x51,0x21,0x5E],  # 0x51  Q
    [0x7F,0x09,0x19,0x29,0x46],  # 0x52  R
    [0x46,0x49,0x49,0x49,0x31],  # 0x53  S
    [0x01,0x01,0x7F,0x01,0x01],  # 0x54  T
    [0x3F,0x40,0x40,0x40,0x3F],  # 0x55  U
    [0x1F,0x20,0x40,0x20,0x1F],  # 0x56  V
    [0x3F,0x40,0x38,0x40,0x3F],  # 0x57  W
    [0x63,0x14,0x08,0x14,0x63],  # 0x58  X
    [0x07,0x08,0x70,0x08,0x07],  # 0x59  Y
    [0x61,0x51,0x49,0x45,0x43],  # 0x5A  Z
    [0x00,0x7F,0x41,0x41,0x00],  # 0x5B  [
    [0x02,0x04,0x08,0x10,0x20],  # 0x5C  backslash
    [0x00,0x41,0x41,0x7F,0x00],  # 0x5D  ]
    [0x04,0x02,0x01,0x02,0x04],  # 0x5E  ^
    [0x40,0x40,0x40,0x40,0x40],  # 0x5F  _
    [0x00,0x01,0x02,0x04,0x00],  # 0x60  `
    [0x20,0x54,0x54,0x54,0x78],  # 0x61  a
    [0x7F,0x48,0x44,0x44,0x38],  # 0x62  b
    [0x38,0x44,0x44,0x44,0x20],  # 0x63  c
    [0x38,0x44,0x44,0x48,0x7F],  # 0x64  d
    [0x38,0x54,0x54,0x54,0x18],  # 0x65  e
    [0x08,0x7E,0x09,0x01,0x02],  # 0x66  f
    [0x0C,0x52,0x52,0x52,0x3E],  # 0x67  g
    [0x7F,0x08,0x04,0x04,0x78],  # 0x68  h
    [0x00,0x44,0x7D,0x40,0x00],  # 0x69  i
    [0x20,0x40,0x44,0x3D,0x00],  # 0x6A  j
    [0x7F,0x10,0x28,0x44,0x00],  # 0x6B  k
    [0x00,0x41,0x7F,0x40,0x00],  # 0x6C  l
    [0x7C,0x04,0x18,0x04,0x78],  # 0x6D  m
    [0x7C,0x08,0x04,0x04,0x78],  # 0x6E  n
    [0x38,0x44,0x44,0x44,0x38],  # 0x6F  o
    [0x7C,0x14,0x14,0x14,0x08],  # 0x70  p
    [0x08,0x14,0x14,0x18,0x7C],  # 0x71  q
    [0x7C,0x08,0x04,0x04,0x08],  # 0x72  r
    [0x48,0x54,0x54,0x54,0x20],  # 0x73  s
    [0x04,0x3F,0x44,0x40,0x20],  # 0x74  t
    [0x3C,0x40,0x40,0x40,0x3C],  # 0x75  u
    [0x1C,0x20,0x40,0x20,0x1C],  # 0x76  v
    [0x3C,0x40,0x30,0x40,0x3C],  # 0x77  w
    [0x44,0x28,0x10,0x28,0x44],  # 0x78  x
    [0x0C,0x50,0x50,0x50,0x3C],  # 0x79  y
    [0x44,0x64,0x54,0x4C,0x44],  # 0x7A  z
    [0x00,0x08,0x36,0x41,0x00],  # 0x7B  {
    [0x00,0x00,0x7F,0x00,0x00],  # 0x7C  |
    [0x00,0x41,0x36,0x08,0x00],  # 0x7D  }
    [0x10,0x08,0x08,0x10,0x08],  # 0x7E  ~
    [0x78,0x46,0x41,0x46,0x78],  # 0x7F
]

# ============================================================================
# Framebuffer  (128 cols × 8 pages = 1024 bytes, horizontal mode layout)
# ============================================================================

WIDTH  = 128
PAGES  = 8
_fb    = bytearray(WIDTH * PAGES)

def fb_clear(fill: int = 0x00):
    for i in range(len(_fb)):
        _fb[i] = fill

def fb_set_pixel_col(page: int, col: int, byte_val: int):
    if 0 <= page < PAGES and 0 <= col < WIDTH:
        _fb[page * WIDTH + col] = byte_val

def fb_or_pixel_col(page: int, col: int, byte_val: int):
    if 0 <= page < PAGES and 0 <= col < WIDTH:
        _fb[page * WIDTH + col] |= byte_val

def fb_draw_char(ch: str, page: int, col: int) -> int:
    """Draw one character at (page, col). Returns next column position."""
    idx = ord(ch) - 0x20
    if idx < 0 or idx >= len(FONT):
        idx = 0
    for cx, byte in enumerate(FONT[idx]):
        fb_set_pixel_col(page, col + cx, byte)
    fb_set_pixel_col(page, col + 5, 0x00)   # 1-pixel gap
    return col + 6

def fb_text(text: str, page: int, col: int = 0) -> int:
    """Draw string starting at (page, col). Returns column after last char."""
    for ch in text:
        if col >= WIDTH:
            break
        col = fb_draw_char(ch, page, col)
    return col

def fb_text_center(text: str, page: int):
    """Draw text horizontally centred on a page row."""
    w = len(text) * 6
    col = max(0, (WIDTH - w) // 2)
    fb_text(text, page, col)

def fb_hline(page: int, col_s: int, col_e: int, mask: int = 0xFF):
    """Draw a horizontal pixel stripe within a page."""
    for c in range(max(0, col_s), min(WIDTH, col_e + 1)):
        fb_or_pixel_col(page, c, mask)

def fb_vline(col: int, page_s: int, page_e: int, mask: int = 0xFF):
    for p in range(page_s, page_e + 1):
        fb_or_pixel_col(p, col, mask)

def fb_fill_rect(page_s: int, page_e: int, col_s: int, col_e: int,
                 fill: int = 0xFF):
    for p in range(page_s, page_e + 1):
        for c in range(max(0, col_s), min(WIDTH, col_e + 1)):
            _fb[p * WIDTH + c] = fill

def fb_rect_border(page_s: int, page_e: int, col_s: int, col_e: int):
    """Draw a 1-pixel rectangular border."""
    fb_hline(page_s, col_s, col_e, 0x01)   # top  (bit 0 = topmost pixel)
    fb_hline(page_e, col_s, col_e, 0x80)   # bottom (bit 7 = bottom pixel)
    for p in range(page_s, page_e + 1):
        fb_or_pixel_col(p, col_s, 0xFF)
        fb_or_pixel_col(p, col_e, 0xFF)

def fb_progress_bar(page: int, col_s: int, col_e: int,
                    value: int, max_val: int = 255):
    """Draw a filled progress bar within a single page row."""
    total_w = col_e - col_s
    filled  = int((value / max_val) * total_w)
    fb_hline(page, col_s, col_e, 0x18)              # empty track (2 mid bits)
    fb_hline(page, col_s, col_s + filled, 0x7E)     # filled bar (6 bits)
    fb_or_pixel_col(page, col_s,  0xFF)              # left cap
    fb_or_pixel_col(page, col_e,  0xFF)              # right cap

def fb_send():
    """Push framebuffer to display via Horizontal Addressing Mode."""
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)          # horizontal mode
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  0, 127)
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE, 0, 7)
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes(_fb))

def fb_send_and_wait(seconds: float, label: str = ""):
    fb_send()
    if label:
        print(f"        {label}")
    time.sleep(seconds)

# ============================================================================
# Shared screen layouts
# ============================================================================

def show_header(title: str):
    """Draw a header bar: thin top border, centred text, thin bottom border."""
    fb_hline(0, 0, 127, 0x01)            # top border (bit 0 = topmost pixel)
    fb_text_center(title, 0)
    fb_hline(1, 0, 127, 0x80)            # bottom border (bit 7 = bottom pixel)

def show_status(status: str, page: int = 6):
    """Draw a status line near the bottom."""
    fb_fill_rect(page, page, 0, 127, 0x00)
    fb_text_center(status, page)

def display_two_line(line1: str, line2: str, info: str = ""):
    """Full-screen two-line message with optional info at bottom."""
    fb_clear()
    fb_text_center(line1, 2)
    fb_text_center(line2, 4)
    if info:
        fb_hline(6, 0, 127, 0x01)
        fb_text_center(info, 7)
    fb_send()

# ============================================================================
# DEMO 0:  I2C Bus Speed
# ============================================================================

def demo_i2c_speed():
    print("\n  DEMO: I2C Bus Speed (I2C_IOC_SET_FREQ)")
    print("  Adjusts the SCL half-period — watch the framebuffer push time.")
    print("  Default 2500 ns half-period → ~100 kHz Standard Mode.")

    speeds = [
        (5000, "~50 kHz  (very slow, 5000 ns half)"),
        (2500, "~100 kHz (Standard Mode, 2500 ns half)"),
        (1250, "~200 kHz (mid-range,    1250 ns half)"),
        ( 625, "~400 kHz (Fast Mode,     625 ns half)"),
        (2500, "Reset to 100 kHz default"),
    ]

    for half_ns, label in speeds:
        fb_clear()
        show_header(" I2C SPEED DEMO ")
        fb_text_center(f"Half-period: {half_ns} ns", 3)
        khz = 1_000_000 // (half_ns * 2)
        fb_text_center(f"SCL  ~  {khz} kHz", 4)
        fb_text_center("Pushing 1024 bytes...", 6)
        set_i2c_speed(half_ns)
        fb_send()
        time.sleep(1.2)
        print(f"        {label}")

    set_i2c_speed(2500)   # restore default
    fb_clear()
    fb_text_center("Speed reset to", 3)
    fb_text_center("100 kHz default.", 4)
    fb_send_and_wait(1.0, "Restored 100 kHz")

# ============================================================================
# DEMO 1:  Contrast / brightness
# ============================================================================

def demo_contrast():
    print("\n  DEMO: Contrast — hardware brightness control (0x81 command)")
    print("  Screen is flooded with white pixels so dimming is clearly visible.")
    ioc_u8(SSD1306_IOC_INVERT, 0)

    # Build a checkerboard framebuffer: alternating 0xAA / 0x55 columns
    # giving ~50 % pixel density across the entire 128x64 screen.  At low
    # contrast the display will look nearly off; at 255 it blazes white.
    def _build_contrast_fb(val: int):
        fb_clear(0x00)
        # Fill pages 0-6 with dense checkerboard (alternating column bytes)
        for p in range(7):
            for c in range(WIDTH):
                _fb[p * WIDTH + c] = 0xAA if (c % 2 == 0) else 0x55
        # Page 7: black strip for the label text so it stays readable
        for c in range(WIDTH):
            _fb[7 * WIDTH + c] = 0x00
        # Overlay progress bar on page 6 (XOR onto checkerboard)
        total_w = 120
        filled  = int((val / 255) * total_w)
        for c in range(4, 124):
            byte = 0xAA if (c % 2 == 0) else 0x55   # base checkerboard
            if c < 4 + filled:
                byte = 0xFF   # fully lit columns for the filled portion
            elif c in (4, 123):
                byte = 0xFF   # end-caps
            else:
                byte = byte & 0x18   # thin track in empty portion
            _fb[6 * WIDTH + c] = byte
        # Text on page 7
        msg = f"Contrast: {val:3d}/255"
        col = max(0, (WIDTH - len(msg) * 6) // 2)
        for ch in msg:
            idx = ord(ch) - 0x20
            if 0 <= idx < len(FONT):
                for cx, b in enumerate(FONT[idx]):
                    if col + cx < WIDTH:
                        _fb[7 * WIDTH + col + cx] = b
            col += 6

    steps = [16, 48, 80, 112, 144, 176, 208, 255,
             208, 160, 112, 64, 127]
    for v in steps:
        _build_contrast_fb(v)
        ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
        ioc_range(SSD1306_IOC_SET_COL_RANGE,  0, 127)
        ioc_range(SSD1306_IOC_SET_PAGE_RANGE, 0, 7)
        ioc_io(SSD1306_IOC_SET_DATA_MODE)
        _fd.write(bytes(_fb))
        ioc_u8(SSD1306_IOC_SET_CONTRAST, v)
        time.sleep(0.3)

    ioc_u8(SSD1306_IOC_SET_CONTRAST, 0x7F)
    fb_clear()
    fb_text_center("Contrast reset", 3)
    fb_text_center("to 127 (default)", 4)
    fb_send_and_wait(1.5, "Done. Contrast reset to 127.")

# ============================================================================
# DEMO 2:  Horizontal mirror
# ============================================================================

def demo_flip_h():
    print("\n  DEMO: H-Flip — horizontal mirror (0xA0 / 0xA1)")
    print("  LEFT/RIGHT labels appear swapped when mirror is toggled.")
    ioc_u8(SSD1306_IOC_INVERT, 0)

    fb_clear()
    show_header("  H-FLIP DEMO  ")
    fb_text("LEFT",  2, 2)
    fb_text("RIGHT", 2, 96)
    fb_text("<-- This side", 4, 0)
    fb_text("That side -->", 5, 14)
    fb_text_center("MIRROR: OFF (normal)", 7)
    ioc_u8(SSD1306_IOC_FLIP_H, 1)    # driver default = 0xA1
    fb_send_and_wait(2.0, "H-Flip=1 (0xA1): normal orientation for this module")

    ioc_u8(SSD1306_IOC_FLIP_H, 0)    # toggle to 0xA0 = mirror
    fb_clear()
    show_header("  H-FLIP DEMO  ")
    fb_text("LEFT",  2, 2)
    fb_text("RIGHT", 2, 96)
    fb_text("<-- This side", 4, 0)
    fb_text("That side -->", 5, 14)
    fb_text_center("MIRROR: ON (flipped)", 7)
    fb_send_and_wait(2.5, "H-Flip=0 (0xA0): LEFT/RIGHT labels now appear swapped")

    ioc_u8(SSD1306_IOC_FLIP_H, 1)
    fb_clear()
    fb_text_center("H-Flip restored.", 3)
    fb_send_and_wait(1.0, "Restored to driver default (flip_h=1)")

# ============================================================================
# DEMO 3:  Vertical flip
# ============================================================================

def demo_flip_v():
    print("\n  DEMO: V-Flip — vertical scan direction (0xC0 / 0xC8)")
    print("  TOP and BOTTOM labels swap when flip is toggled.")
    ioc_u8(SSD1306_IOC_INVERT, 0)

    ioc_u8(SSD1306_IOC_FLIP_V, 1)   # driver default = 0xC8
    fb_clear()
    show_header("  V-FLIP DEMO  ")
    fb_text_center("=== TOP OF SCREEN ===", 0)
    fb_hline(2, 0, 127, 0xFF)
    fb_text_center("Content area", 4)
    fb_text_center("=== BOTTOM ===", 7)
    fb_text_center("FLIP: OFF (normal)", 3)
    fb_send_and_wait(2.0, "V-Flip=1 (0xC8): normal orientation for this module")

    ioc_u8(SSD1306_IOC_FLIP_V, 0)   # toggle to 0xC0
    fb_clear()
    show_header("  V-FLIP DEMO  ")
    fb_text_center("=== TOP OF SCREEN ===", 0)
    fb_hline(2, 0, 127, 0xFF)
    fb_text_center("Content area", 4)
    fb_text_center("=== BOTTOM ===", 7)
    fb_text_center("FLIP: ON (flipped)", 3)
    fb_send_and_wait(2.5, "V-Flip=0 (0xC0): TOP/BOTTOM appear swapped")

    ioc_u8(SSD1306_IOC_FLIP_V, 1)
    fb_clear()
    fb_text_center("V-Flip restored.", 3)
    fb_send_and_wait(1.0, "Restored to driver default (flip_v=1)")

# ============================================================================
# DEMO 4:  180° rotation  (H-flip + V-flip combined)
# ============================================================================

def demo_rotate_180():
    print("\n  DEMO: 180° Rotation — combine H-flip + V-flip")
    print("  The display content rotates 180° without touching GDDRAM.")
    ioc_u8(SSD1306_IOC_INVERT, 0)

    def draw_orientation_screen(label: str):
        fb_clear()
        fb_text_center(">>> TOP >>>", 0)
        fb_rect_border(0, 7, 0, 127)
        fb_text("L",  3,  4)
        fb_text("R",  3, 118)
        fb_text_center(label, 4)
        fb_text_center("<<< BOTTOM <<<", 7)

    ioc_u8(SSD1306_IOC_FLIP_H, 1)
    ioc_u8(SSD1306_IOC_FLIP_V, 1)
    draw_orientation_screen("DEFAULT (normal)")
    fb_send_and_wait(2.0, "Default orientation (flip_h=1, flip_v=1). L on left.")

    ioc_u8(SSD1306_IOC_FLIP_H, 0)
    ioc_u8(SSD1306_IOC_FLIP_V, 0)
    draw_orientation_screen("ROTATED 180 deg")
    fb_send_and_wait(2.5, "Rotated 180 deg (flip_h=0, flip_v=0). L and R appear swapped.")

    ioc_u8(SSD1306_IOC_FLIP_H, 1)
    ioc_u8(SSD1306_IOC_FLIP_V, 1)
    fb_clear()
    fb_text_center("Rotation reset.", 3)
    fb_send_and_wait(1.0, "Restored to default.")

# ============================================================================
# DEMO 5:  Pixel inversion
# ============================================================================

def demo_invert():
    print("\n  DEMO: Pixel Inversion (0xA6 / 0xA7)")
    print("  Hardware inverts all pixels — no GDDRAM change needed.")
    ioc_u8(SSD1306_IOC_INVERT, 0)

    fb_clear()
    show_header("  INVERT DEMO  ")
    fb_text_center("White text,", 3)
    fb_text_center("black background.", 4)
    fb_text_center("INVERTED: NO", 6)
    fb_send_and_wait(2.0, "Normal: white pixels on black")

    ioc_u8(SSD1306_IOC_INVERT, 1)
    fb_clear()
    show_header("  INVERT DEMO  ")
    fb_text_center("Black text,", 3)
    fb_text_center("white background.", 4)
    fb_text_center("INVERTED: YES", 6)
    fb_send_and_wait(2.5, "Inverted: display shows black-on-white")

    ioc_u8(SSD1306_IOC_INVERT, 0)
    fb_clear()
    fb_text_center("Normal restored.", 3)
    fb_send_and_wait(1.0, "Restored to normal")

# ============================================================================
# DEMO 6:  Horizontal scroll
# ============================================================================

def demo_scroll_h():
    print("\n  DEMO: Hardware Horizontal Scroll (0x26 / 0x27)")
    print("  The SSD1306 scrolls autonomously. CPU does nothing after setup.")
    ioc_u8(SSD1306_IOC_INVERT, 0)

    # RIGHT scroll — all pages
    fb_clear()
    show_header(" H-SCROLL DEMO ")
    fb_text_center(">>> SCROLLING RIGHT >>>", 3)
    fb_text_center("All pages, fast speed", 5)
    fb_text_center("CPU is idle!", 7)
    fb_send()
    print("        Scroll RIGHT — all pages, speed 0x07 (fastest)")
    ioc_scroll(SSD1306_IOC_SCROLL_H,
               direction=0, start_page=0, end_page=7, speed=0x07)
    time.sleep(3)

    ioc_io(SSD1306_IOC_SCROLL_STOP)
    time.sleep(0.1)

    # LEFT scroll — bottom half only
    fb_clear()
    show_header(" H-SCROLL DEMO ")
    fb_text_center("TOP rows: FIXED", 2)
    fb_hline(3, 0, 127, 0xFF)               # divider
    fb_text_center("<<< SCROLLING LEFT <<<", 5)
    fb_text_center("pages 4-7, slow speed", 7)
    fb_send()
    print("        Scroll LEFT  — pages 4-7 only, speed 0x02 (slow)")
    ioc_scroll(SSD1306_IOC_SCROLL_H,
               direction=1, start_page=4, end_page=7, speed=0x02)
    time.sleep(4)

    ioc_io(SSD1306_IOC_SCROLL_STOP)
    fb_clear()
    fb_text_center("Scroll stopped.", 3)
    fb_send_and_wait(1.0, "Stopped")

# ============================================================================
# DEMO 7:  Diagonal scroll
# ============================================================================

def demo_scroll_diag():
    print("\n  DEMO: Diagonal Scroll (vertical + horizontal, 0x29 / 0x2A)")
    print("  Content scrolls diagonally — both axes at once.")
    ioc_u8(SSD1306_IOC_INVERT, 0)

    fb_clear()
    show_header("DIAGONAL SCROLL")
    for row in range(2, 8):
        fb_text(f"Line {row-1}: Pi 5 I2C OLED Driver v2.0", row, 0)
    fb_send()
    print("        Diagonal scroll RIGHT, 1 row/step, medium speed 0x03")
    ioc_scroll(SSD1306_IOC_SCROLL_DIAG,
               direction=0, start_page=0, end_page=7,
               speed=0x03, vert_offset=1)
    time.sleep(4)

    ioc_io(SSD1306_IOC_SCROLL_STOP)
    time.sleep(0.1)

    fb_clear()
    show_header("DIAGONAL SCROLL")
    for row in range(2, 8):
        fb_text(f">>> Row {row-1}  diagonal left  <<<", row, 0)
    fb_send()
    print("        Diagonal scroll LEFT, 2 rows/step, fast speed 0x05")
    ioc_scroll(SSD1306_IOC_SCROLL_DIAG,
               direction=1, start_page=0, end_page=7,
               speed=0x05, vert_offset=2)
    time.sleep(3)

    ioc_io(SSD1306_IOC_SCROLL_STOP)
    fb_clear()
    fb_text_center("Diagonal scroll", 3)
    fb_text_center("stopped.", 4)
    fb_send_and_wait(1.0, "Stopped")

# ============================================================================
# DEMO 8:  GDDRAM Addressing Modes
# ============================================================================

def demo_addr_mode():
    """
    GDDRAM Addressing Modes demo — two clean, numbered steps.

    Step 1 — Horizontal mode (0x20 0x00):
      Pointer auto-advances left→right across columns, then wraps to the
      next page automatically.  One 1024-byte write fills the whole screen.
      We show this by writing a page-striped pattern: each of the 8 pages
      gets a distinct fill byte, so you can see all 8 rows light up in one shot.

    Step 2 — Page mode (0x20 0x02):
      The pointer only advances within the current page row.  To move to a
      different page you must send a new page-address command.
      We show this by leaving a labelled screen on display while we write
      ONLY to pages 3 and 4 — all other pages are untouched.
    """
    print("\n  DEMO: GDDRAM Addressing Modes (0x20 command)")
    print("  Controls how the internal write pointer auto-advances.")
    ioc_u8(SSD1306_IOC_INVERT, 0)

    # ----------------------------------------------------------------
    # Announce Step 1 on a completely clean screen BEFORE running it
    # ----------------------------------------------------------------
    fb_clear()
    show_header(" ADDR MODE: 1/2 ")
    fb_text_center("HORIZONTAL MODE", 2)
    fb_text_center("Single 1024-byte write", 3)
    fb_text_center("fills ALL 8 rows at once.", 4)
    fb_text_center("Watch screen fill...", 6)
    fb_send_and_wait(2.5, "Step 1: Horizontal mode — about to write 1024 bytes in one shot")

    # Now demonstrate: write 8 different fill bytes, one per page, all in
    # one continuous 1024-byte stream — pointer wraps automatically.
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  0, 127)
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE, 0, 7)
    # Build pattern: each page has a distinct shade
    page_fills = [0xFF, 0xAA, 0x55, 0xFF, 0x55, 0xAA, 0xFF, 0x18]
    pattern = bytearray()
    for fill in page_fills:
        pattern += bytes([fill] * WIDTH)
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes(pattern))
    print("        8 rows written: each page has a different fill byte (all in 1 stream)")
    time.sleep(3.0)

    # ----------------------------------------------------------------
    # Announce Step 2  (clean screen first — blank slate)
    # ----------------------------------------------------------------
    fb_clear()
    show_header(" ADDR MODE: 2/2 ")
    fb_text_center("PAGE MODE", 2)
    fb_text_center("Write pointer stays", 3)
    fb_text_center("inside ONE row only.", 4)
    fb_text_center("Only rows 4+5 will change.", 6)
    # Restore horizontal so fb_send works to draw this announcement
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  0, 127)
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE, 0, 7)
    fb_send_and_wait(2.5, "Step 2: Page mode — only pages 3 and 4 will be updated")

    # Switch to page mode.  The announcement text is now frozen on screen.
    # Write ONLY to pages 3 and 4 — the rest of the screen stays as-is.
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 2)

    i2c_page_mode_goto(3, 0)                          # set cursor: page 3, col 0
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes([0xFF] * WIDTH))                  # fill row 4 solid white

    i2c_page_mode_goto(4, 0)                          # set cursor: page 4, col 0
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes([0xAA] * WIDTH))                  # fill row 5 dashed

    print("        Page mode write done — rows 4 and 5 changed, all others preserved.")
    time.sleep(3.5)

    # Restore to horizontal mode
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  0, 127)
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE, 0, 7)
    fb_clear()
    fb_text_center("Back to Horizontal", 3)

# ============================================================================
# DEMO 9:  Address windowing
# ============================================================================

def demo_windowing():
    """
    Address Windowing demo — 3 clear steps:

    Step 1: Fill the ENTIRE screen with a stripe pattern (full 1024-byte write).
            Every pixel is lit — this is the starting state.

    Step 2: Set a window (cols 24-103, pages 2-5) and write 0x00 (black) into it.
            Only that rectangle goes dark — the rest of the lit screen stays.
            This proves the write pointer is constrained inside the window.

    Step 3: Write a second, smaller window (cols 40-87, pages 3-4) with 0xFF
            (white) to 'draw' inside the black rectangle — like a sub-window.
            Then label it with dark-on-white text.
    """
    print("\n  DEMO: Column & Page Windowing (0x21 / 0x22)")
    print("  Constrains writes to a sub-rectangle. Pixels outside window are untouched.")
    ioc_u8(SSD1306_IOC_INVERT, 0)

    # ----------------------------------------------------------------
    # Step 1: Announce, then flood the whole screen with stripes
    # ----------------------------------------------------------------
    fb_clear()
    show_header(" WINDOW DEMO 1/3")
    fb_text_center("Step 1: Filling", 3)
    fb_text_center("ENTIRE screen with", 4)
    fb_text_center("stripes (full write)", 5)
    fb_send_and_wait(2.0, "Step 1 announcement shown")

    # Flood all 1024 bytes — alternating stripes per page
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  0, 127)
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE, 0, 7)
    stripe = bytearray()
    for p in range(8):
        fill = 0xFF if p % 2 == 0 else 0xAA
        stripe += bytes([fill] * WIDTH)
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes(stripe))
    print("        Full screen flooded with stripes — 1024 bytes sent")
    time.sleep(2.5)

    # ----------------------------------------------------------------
    # Step 2: Announce on top of stripes, then erase only the window
    # ----------------------------------------------------------------
    # Write a black announcement bar at the very top (page 0 only, page mode)
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 2)
    i2c_page_mode_goto(0, 0)
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes([0x00] * WIDTH))   # clear page 0
    i2c_page_mode_goto(1, 0)
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes([0x00] * WIDTH))   # clear page 1
    # Write announcement text into the now-black pages 0-1
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  0, 127)
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE, 0, 1)
    ann_buf = bytearray(WIDTH * 2)
    def _ann(text, page, col=0):
        for ch in text:
            idx = ord(ch) - 0x20
            if 0 <= idx < len(FONT):
                for cx, b in enumerate(FONT[idx]):
                    if col + cx < WIDTH:
                        ann_buf[(page) * WIDTH + col + cx] = b
            col += 6
    _ann("Step 2: Erasing window", 0, 0)
    _ann("cols24-103 pages2-5",   1, 4)
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes(ann_buf))
    time.sleep(2.5)
    print("        Step 2: erasing centre window only")

    # Now erase ONLY the window region — pixels outside stay striped
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  24, 103)   # 80 columns
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE,  2,   5)   # 4 pages = 32 rows
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes([0x00] * (80 * 4)))               # black rectangle
    print("        Window (80x32) erased — stripes outside window untouched")
    time.sleep(3.0)

    # ----------------------------------------------------------------
    # Step 3: Draw a white sub-window with dark text inside the black hole
    # ----------------------------------------------------------------
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 2)
    i2c_page_mode_goto(0, 0)
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes([0x00] * WIDTH))
    i2c_page_mode_goto(1, 0)
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes([0x00] * WIDTH))
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  0, 127)
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE, 0, 1)
    ann_buf2 = bytearray(WIDTH * 2)
    def _ann2(text, page, col=0):
        for ch in text:
            idx = ord(ch) - 0x20
            if 0 <= idx < len(FONT):
                for cx, b in enumerate(FONT[idx]):
                    if col + cx < WIDTH:
                        ann_buf2[page * WIDTH + col + cx] = b
            col += 6
    _ann2("Step 3: Sub-window", 0, 4)
    _ann2("inside the black box", 1, 1)
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes(ann_buf2))

    # Fill sub-window solid white
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  36, 91)    # 56 columns
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE,  3,  4)    # 2 pages = 16 rows
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes([0xFF] * (56 * 2)))

    # Write dark-on-white label inside the white sub-window
    label = bytearray([0xFF] * (56 * 2))
    def _lbl(text, row_in_subwin, col_offset=0):
        col = col_offset
        for ch in text:
            idx = ord(ch) - 0x20
            if 0 <= idx < len(FONT):
                for cx, b in enumerate(FONT[idx]):
                    li = row_in_subwin * 56 + col + cx
                    if 0 <= col + cx < 56 and 0 <= li < len(label):
                        label[li] ^= b
            col += 6
    _lbl("WINDOW", 0, 5)
    _lbl("56x16px", 1, 4)
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  36, 91)
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE,  3,  4)
    ioc_io(SSD1306_IOC_SET_DATA_MODE)
    _fd.write(bytes(label))
    print("        Sub-window (56x16) drawn inside the black region")
    time.sleep(3.5)

    # Clean up
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  0, 127)
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE, 0, 7)
    fb_clear()
    fb_text_center("Window demo done.", 3)
    fb_text_center("Full screen restored.", 4)
    fb_send_and_wait(1.0, "Done. Full window restored.")

# ============================================================================
# Interactive menu
# ============================================================================

DEMOS = [
    ("I2C Bus Speed (SCL frequency)",          demo_i2c_speed),
    ("Contrast / Brightness",                   demo_contrast),
    ("Horizontal Mirror (H-Flip)",              demo_flip_h),
    ("Vertical Flip (V-Flip)",                  demo_flip_v),
    ("180 Degree Rotation",                     demo_rotate_180),
    ("Pixel Inversion",                         demo_invert),
    ("Horizontal Scroll",                       demo_scroll_h),
    ("Diagonal Scroll",                         demo_scroll_diag),
    ("GDDRAM Addressing Modes",                 demo_addr_mode),
    ("Address Windowing (sub-rectangle)",       demo_windowing),
]

def reset_display():
    """Restore display to driver init defaults (matches ssd1306_hardware_init_sequence)."""
    ioc_io(SSD1306_IOC_SCROLL_STOP)
    ioc_u8(SSD1306_IOC_FLIP_H,        1)   # 0xA1 — segment remapped (matches driver)
    ioc_u8(SSD1306_IOC_FLIP_V,        1)   # 0xC8 — COM scan remapped (matches driver)
    ioc_u8(SSD1306_IOC_INVERT,        0)   # 0xA6 — normal (not inverted)
    ioc_u8(SSD1306_IOC_SET_CONTRAST,  0x7F)
    ioc_u8(SSD1306_IOC_SET_ADDR_MODE, 0)
    ioc_range(SSD1306_IOC_SET_COL_RANGE,  0, 127)
    ioc_range(SSD1306_IOC_SET_PAGE_RANGE, 0, 7)
    set_i2c_speed(2500)                     # I2C-specific: restore 100 kHz default
    fb_clear()
    fb_send()

def print_menu():
    print()
    print("  ╔══════════════════════════════════════════════╗")
    print("  ║   SSD1306 I2C Feature Demo  (v2.0)           ║")
    print("  ║   Device: /dev/i2c_ssd1306_bitbang           ║")
    print("  ╠══════════════════════════════════════════════╣")
    for i, (name, _) in enumerate(DEMOS, 1):
        print(f"  ║  {i:2d}. {name:<41} ║")
    print("  ╠══════════════════════════════════════════════╣")
    print("  ║   A. Run ALL demos in sequence               ║")
    print("  ║   R. Hardware Reset (RST pin)                ║")
    print("  ║   Q. Quit                                    ║")
    print("  ╚══════════════════════════════════════════════╝")

def show_demo_title_on_display(name: str):
    fb_clear()
    fb_rect_border(1, 6, 2, 125)
    fb_text_center("DEMO:", 2)
    # Word-wrap the demo name across pages 3–5
    words = name.split()
    line, row = "", 4
    for w in words:
        test = (line + " " + w).strip()
        if len(test) * 6 <= 118:
            line = test
        else:
            fb_text_center(line, row)
            row += 1
            line = w
    if line:
        fb_text_center(line, row)
    fb_send()
    time.sleep(1.0)

def main():
    open_device()
    print(f"\n  Opened {DEVICE_PATH}")
    ioc_io(SSD1306_IOC_WAKEUP)

    # Splash screen
    fb_clear()
    fb_text_center("Pi 5 I2C OLED", 2)
    fb_text_center("Feature Demo v2.0", 3)
    fb_hline(5, 0, 127, 0x01)
    fb_text_center("GPIO 16/17/27", 6)
    fb_send()
    time.sleep(1.5)

    try:
        while True:
            reset_display()
            print_menu()

            choice = input("\n  Enter choice: ").strip().upper()

            if choice == 'Q':
                print("\n  Goodbye! Display cleared.\n")
                break
            elif choice == 'R':
                print("  Performing software reset (re-sending full init sequence)...")
                print("  Note: 4-pin I2C OLED has no RST pin — HW_RESET does a software re-init.")
                ioc_io(SSD1306_IOC_HW_RESET)
                print("  Software reset complete.")
            elif choice == 'A':
                for name, fn in DEMOS:
                    show_demo_title_on_display(name)
                    fn()
                    reset_display()
                    time.sleep(0.3)
            else:
                try:
                    idx = int(choice) - 1
                    if 0 <= idx < len(DEMOS):
                        name, fn = DEMOS[idx]
                        show_demo_title_on_display(name)
                        fn()
                    else:
                        print("  Invalid choice.")
                except ValueError:
                    print("  Invalid choice.")

            input("\n  Press Enter to return to menu...")

    except KeyboardInterrupt:
        print("\n  Interrupted.")
    except OSError as e:
        print(f"\n  Device error: {e}")
        print("  Check: dmesg | tail -20")
    finally:
        try:
            ioc_io(SSD1306_IOC_SCROLL_STOP)
            reset_display()
        except Exception:
            pass
        close_device()

if __name__ == "__main__":
    main()
