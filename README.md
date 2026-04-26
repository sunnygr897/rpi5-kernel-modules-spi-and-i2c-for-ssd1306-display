# rpi5-kernel-modules-spi-and-i2c-for-ssd1306-display
This contains implementation of kernel module driver which bitbangs the GPIO's to support SSD1306 display on Raspberry Pi 5.

## Raspberry Pi 5 Setup

Perform all the steps below **directly on the Raspberry Pi 5** (native compilation).  
Cross-compilation is supported — see the comments at the bottom of each `Makefile`.

### 1. Install OS & Kernel Headers

Flash **Raspberry Pi OS Lite (64-bit)** or the full desktop image using [Raspberry Pi Imager](https://www.raspberrypi.com/software/).

After first boot, update the system and install the running kernel's headers:

```bash
sudo apt update && sudo apt full-upgrade -y
sudo apt install -y raspberrypi-kernel-headers
```

Verify headers are present for your running kernel:

```bash
ls /lib/modules/$(uname -r)/build
# Should list: Makefile  Module.symvers  arch  include  scripts ...
```

> **If the `build` symlink is missing** after an upgrade, run:
> ```bash
> sudo apt install --reinstall raspberrypi-kernel-headers
> sudo reboot
> ```

### 2. Install Build Tools

```bash
sudo apt install -y build-essential git bc flex bison libssl-dev libelf-dev
```

### 3. Install Device Tree Compiler

```bash
sudo apt install -y device-tree-compiler
```

Confirm the version supports overlays (`-@` flag):

```bash
dtc --version
# dtc: Version DTS v1.6.x  (need ≥ 1.4.4)
```

### 4. Clone the Repository

```bash
git clone https://github.com/sunnygr897/rpi5-kernel-modules-spi-and-i2c-for-ssd1306-display.git
cd <your-repo-name>
```

## SPI Driver — pi5\_spi\_driver

### SPI Wiring

Use BCM (GPIO) numbering — **not** the physical pin numbers.

| SSD1306 Pin | Signal | GPIO (BCM) | 40-pin Header |
|-------------|--------|-----------|---------------|
| GND         | Ground | GND       | Pin 6         |
| VCC         | 3.3 V  | 3V3       | Pin 1         |
| D0 / CLK    | SCLK   | **GPIO 17** | Pin 11      |
| D1 / MOSI   | MOSI   | **GPIO 27** | Pin 13      |
| RES         | RST    | **GPIO 6**  | Pin 31      |
| DC          | D/C    | **GPIO 5**  | Pin 29      |
| CS          | CS     | **GPIO 22** | Pin 15      |

> **Tip:** The 7-pin SSD1306 module uses 4-wire SPI (MISO is unused since the display is write-only).

### SPI: Build the Kernel Module

```bash
cd pi5_spi_driver
make
```

A successful build produces:

```
spi_bitbang_kernel_driver.ko   ← main OLED driver
```

To clean build artifacts:

```bash
make clean
```

### SPI: Compile & Install the Device Tree Overlay

```bash
# Compile .dts → .dtbo
dtc -@ -I dts -O dtb -o ssd1306-bitbang.dtbo ssd1306-bitbang.dts

# Install to the firmware overlays directory
sudo cp ssd1306-bitbang.dtbo /boot/firmware/overlays/
```

### SPI: Enable the Overlay

Open `/boot/firmware/config.txt` with your editor:

```bash
sudo nano /boot/firmware/config.txt
```

Add **exactly one** of the following lines at the end of the file:

```ini
dtoverlay=ssd1306-bitbang
```

Save and reboot:

```bash
sudo reboot
```

After reboot, verify the GPIO pins are claimed by the overlay:

```bash
gpioinfo | grep ssd1306
# Expected output (6 lines):
#   line  5: "ssd1306-dc"   output active-high [used]
#   line  6: "ssd1306-rst"  output active-low  [used]
#   line 16: "ssd1306-miso-lb" output active-low [used]
#   line 17: "ssd1306-sclk" output active-low  [used]
#   line 22: "ssd1306-cs"   output active-high [used]
#   line 27: "ssd1306-mosi" output active-low  [used]
```

### SPI: Load the Driver

```bash
cd pi5_spi_driver

# Insert the kernel module
sudo insmod spi_bitbang_kernel_driver.ko

# Verify it loaded and the device node appeared
ls -l /dev/spi_ssd1306_bitbang

# Confirm the OLED initialised (look for the init sequence log)
sudo dmesg | tail -20
# Expect lines like:
#   spi_ssd1306: Initializing SSD1306 Bit-Banged SPI Driver v2.1...
#   spi_ssd1306: Applying SSD1306 hardware reset...
#   spi_ssd1306: Initialization sequence written to display
#   spi_ssd1306: Driver loaded and ready at /dev/spi_ssd1306_bitbang
```

Grant your user access to the device node (or run test scripts with `sudo`):

```bash
sudo chmod 666 /dev/spi_ssd1306_bitbang
```

### SPI: Run the Test Scripts

**Full feature demo** — interactive menu covering every IOCTL (contrast, flip, invert, scroll, addressing modes, windowing):

```bash
python3 test_features.py
```

### SPI: Unload the Driver

```bash
sudo rmmod spi_bitbang_kernel_driver
sudo dmesg | tail -5
# Expect: spi_ssd1306: Driver removed cleanly.
```

## I2C Driver — pi5\_i2c\_oled

### I2C Wiring

| SSD1306 Pin | Signal | GPIO (BCM) | 40-pin Header |
|-------------|--------|-----------|---------------|
| GND         | Ground | GND       | Pin 6         |
| VCC         | 3.3 V  | 3V3       | Pin 1         |
| SCL         | SCL    | **GPIO 23** | Pin 16      |
| SDA         | SDA    | **GPIO 24** | Pin 18      |

**Pull-up resistors are required:**  
Connect a **4.7 kΩ resistor** from SCL → 3.3 V and another from SDA → 3.3 V.  
Most 4-pin OLED breakout boards already include these on-board — check your module's silkscreen for `R1`/`R2` populated pads before adding external ones.

> The driver emulates open-drain I2C by toggling output-enable on the RIO register:  
> **LOW** = drive pin to GND (output mode), **HIGH** = release pin to pull-up (input / high-Z).

### I2C: Build the Kernel Module

```bash
cd pi5_i2c_oled
make
```

A successful build produces:

```
i2c_bitbang_kernel_driver.ko
```

To clean:

```bash
make clean
```

### I2C: Compile & Install the Device Tree Overlay

```bash
# Compile .dts → .dtbo
dtc -@ -I dts -O dtb -o ssd1306-i2c-bitbang.dtbo ssd1306-i2c-bitbang.dts

# Install
sudo cp ssd1306-i2c-bitbang.dtbo /boot/firmware/overlays/
```

### I2C: Enable the Overlay

```bash
sudo nano /boot/firmware/config.txt
```

Add at the end:

```ini
dtoverlay=ssd1306-i2c-bitbang
```

Save and reboot:

```bash
sudo reboot
```

After reboot, verify the GPIO pins are claimed:

```bash
gpioinfo | grep ssd1306
# Expected output (2 lines):
#   line 23: "ssd1306-scl"  output active-high [used]
#   line 24: "ssd1306-sda"  output active-high [used]
```

### I2C: Load the Driver

```bash
cd pi5_i2c_oled

sudo insmod i2c_bitbang_kernel_driver.ko

ls -l /dev/i2c_ssd1306_bitbang

sudo dmesg | tail -20
# Expect:
#   i2c_ssd1306: Initializing SSD1306 Bit-Banged I2C Driver...
#   i2c_ssd1306: Initialization sequence written to display
#   i2c_ssd1306: Driver loaded and ready at /dev/i2c_ssd1306_bitbang
```

Grant access:

```bash
sudo chmod 666 /dev/i2c_ssd1306_bitbang
```

### I2C: Run the Test Scripts
**Full feature demo** (contrast, flip, invert, scroll, addressing modes, windowing):

```bash
python3 test_features.py
```

### I2C: Unload the Driver

```bash
sudo rmmod i2c_bitbang_kernel_driver
sudo dmesg | tail -5
# Expect: i2c_ssd1306: Driver removed cleanly.
```

## Auto-Load on Boot (Optional)

To have the driver load automatically every boot without editing `config.txt` further:

```bash
# SPI driver
echo "spi_bitbang_kernel_driver" | sudo tee /etc/modules-load.d/ssd1306-spi.conf

# I2C driver
echo "i2c_bitbang_kernel_driver" | sudo tee /etc/modules-load.d/ssd1306-i2c.conf
```

The module must be installed into the kernel tree for `modprobe` to find it.  
The simplest approach is to copy the `.ko` file to the extra modules directory:

```bash
# Example for SPI driver
sudo mkdir -p /lib/modules/$(uname -r)/extra
sudo cp pi5_spi_driver/spi_bitbang_kernel_driver.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a

# Example for I2C driver
sudo cp pi5_i2c_oled/i2c_bitbang_kernel_driver.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
```

## License

These drivers are released under the **GNU General Public License v2.0** (GPL-2.0), consistent with the Linux kernel licensing requirements for loadable kernel modules.

```
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
```


