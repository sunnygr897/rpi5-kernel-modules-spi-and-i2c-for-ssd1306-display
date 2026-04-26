# ==============================================================================
# Makefile for Raspberry Pi 5 I2C Bit-bang SSD1306 OLED Driver
# ==============================================================================

# Defines the object file to be built. The kernel build system will look
# for a matching .c file (i2c_bitbang_kernel_driver.c) and compile it into a .ko
obj-m += i2c_bitbang_kernel_driver.o

# When compiled natively ON the Raspberry Pi:
# KDIR points to the kernel headers for the currently running kernel.
KDIR = /lib/modules/$(shell uname -r)/build
PWD  = $(shell pwd)

all:
	# Invoke the kernel build system to compile the module
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	# Clean up build artifacts (.o, .ko, .mod, etc.)
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# ==============================================================================
# CROSS-COMPILATION INSTRUCTIONS:
# If you are compiling this on a separate Linux/Windows x86 machine targeted
# for the Raspberry Pi (AArch64), you must pass the architecture and
# cross-compiler variables. For example:
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/pi/linux
# ==============================================================================
