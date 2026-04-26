# ==============================================================================
# Makefile for Raspberry Pi 5 SPI Bit-bang Driver
# ==============================================================================
#
# WHY both obj-m lines are here:
#   The kernel build system is invoked as:
#     make -C $(KDIR) M=$(PWD) modules
#   It reads THIS file (Makefile) from $(PWD) to discover what to build.
#   Any separate Makefile_* is only a frontend — the kbuild system never
#   reads it, so all obj-m declarations MUST live here.
# ==============================================================================

# SPI SSD1306 bit-bang driver (main production module)
obj-m += spi_bitbang_kernel_driver.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# ---------- build targets -----------------------------------------------------

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	# Clean up build artifacts (.o, .ko, .mod, etc.)
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# ==============================================================================
# CROSS-COMPILATION INSTRUCTIONS:
# If you are compiling this on a separate Linux/Windows x86 machine targeted 
# for the Raspberry Pi (AArch64), you must pass the architecture and 
# cross-compiler variables. For example:
# make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/pi/linux
# ==============================================================================
