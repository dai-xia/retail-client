# Driver Makefile - RK3568 cross-compilation
#
# Usage:
#   make                          # Build all driver modules
#   make clean                    # Clean build outputs
#   make modules                  # Build only .ko kernel modules
#
# Toolchain: install the aarch64-linux-gnu- cross-compiler:
#   sudo apt install gcc-aarch64-linux-gnu
#
# KERNEL_DIR must point to your RK3568 kernel source tree. Override it on
# the command line, e.g.:
#   make KERNEL_DIR=/path/to/rk3568/kernel

CROSS_COMPILE ?= aarch64-linux-gnu-
KERNEL_DIR    ?= $(error KERNEL_DIR is not set. \
    Point it to your RK3568 kernel source tree, e.g. \
    `make KERNEL_DIR=/path/to/rk3568/kernel`)
PWD           := $(shell pwd)
CC            := $(CROSS_COMPILE)gcc

# ======================== Kernel modules ========================
# obj-m: kernel modules to build
obj-m += rc522_drv.o
obj-m += bh1750_drv.o
obj-m += led_drv.o
obj-m += motor_drv.o
obj-m += motor_drv_pwm.o
obj-m += dma_spi_drv.o

all: modules

modules:
	make -C $(KERNEL_DIR) M=$(PWD) modules ARCH=arm64 CROSS_COMPILE=$(CROSS_COMPILE)

clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean ARCH=arm64 CROSS_COMPILE=$(CROSS_COMPILE)

.PHONY: all modules clean
