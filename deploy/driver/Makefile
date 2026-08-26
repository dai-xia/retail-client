#
# In-tree kernel driver Makefile for the retail subsystem.
# Place under drivers/retail/ and trigger via
# obj-$(CONFIG_RETAIL_DRIVERS) += retail/ in drivers/Makefile.
#

obj-$(CONFIG_RETAIL_RC522)  += rc522_drv.o
obj-$(CONFIG_RETAIL_BH1750) += bh1750_drv.o
obj-$(CONFIG_RETAIL_LED)    += led_drv.o
obj-$(CONFIG_RETAIL_MOTOR)  += motor_drv.o

ccflags-y := -O2 -Wall
