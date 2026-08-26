/*
 * LED GPIO Driver — Character Device + sysfs
 *
 * Platform: RK3568 (Topeet development board)
 * Bus: GPIO (GPIO1_D0)
 *
 * Features:
 *   - Character device /dev/led (read returns current brightness, write sets brightness)
 *   - sysfs /sys/class/led_dev/led_dev/brightness
 *
 * Interview knowledge points:
 *
 * 1. GPIO subsystem vs direct register operation
 *    - Old approach: ioremap maps GPIO controller address, write registers directly
 *    - Modern approach: use gpiod API (devm_gpiod_get)
 *    - Advantages: no SoC dependency, device tree configures pins, kernel manages pin conflicts
 *
 * 2. Why not use /sys/class/leds?
 *    - /sys/class/leds is the LED subsystem, suitable for simple blinking
 *    - A custom character device can provide a more flexible interface (ioctl to set PWM duty, etc.)
 *    - Understanding both approaches is a plus in interviews
 *
 * 3. PWM vs GPIO to control LED
 *    - GPIO: only on/off (digital), suitable for status indicators
 *    - PWM: can adjust brightness (analog), suitable for backlight control
 *    - This driver uses GPIO mode, simple and direct
 *
 * Hardware wiring:
 *   LED anode → current-limiting resistor (220Ω) → GPIO1_D0
 *   LED cathode → GND
 *
 * GPIO number calculation:
 *   GPIO1_D0 = GPIO1 bank D pin 0
 *   GPIO1 = 32 (GPIO0 has A0-D7, 32 pins total)
 *   D0 = 24 (A0-A7=0-7, B0-B7=8-15, C0-C7=16-23)
 *   GPIO1_D0 = 32 + 24 = 56
 *
 *   Verify: cat /sys/kernel/debug/gpio | grep gpio-56
 */

#define pr_fmt(fmt) "led: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#define LED_DEV_NAME "led_dev"

struct led_data {
    struct gpio_desc *gpio;

    dev_t       devid;
    struct cdev cdev;
    struct class *class;

    struct mutex lock;
    int brightness;   /* 0=off, 1=on */
};

static struct led_data *led_dev;

/*
 * read() — read current LED state
 *
 * Reads back the value written by the last write, not the actual hardware pin level.
 * This is common practice — the driver maintains software state to avoid accessing
 * hardware on every read.
 */
static ssize_t led_read(struct file *filp, char __user *buf,
                        size_t count, loff_t *off)
{
    int val;

    if (count < sizeof(int))
        return -EINVAL;

    mutex_lock(&led_dev->lock);
    val = led_dev->brightness;
    mutex_unlock(&led_dev->lock);

    if (copy_to_user(buf, &val, sizeof(val)))
        return -EFAULT;

    return sizeof(val);
}

/*
 * write() — set LED brightness
 *
 * Write an int value:
 *   0 = off
 *   non-zero = on
 *
 * gpiod_set_value(desc, value):
 *   value = 0 → output low (LED off)
 *   value = 1 → output high (LED on, if LED anode is connected to GPIO)
 */
static ssize_t led_write(struct file *filp, const char __user *buf,
                         size_t count, loff_t *off)
{
    int val;

    if (count < sizeof(int))
        return -EINVAL;

    if (copy_from_user(&val, buf, sizeof(val)))
        return -EFAULT;

    mutex_lock(&led_dev->lock);
    led_dev->brightness = val ? 1 : 0;
    gpiod_set_value(led_dev->gpio, led_dev->brightness);
    mutex_unlock(&led_dev->lock);

    return sizeof(val);
}

static struct file_operations led_fops = {
    .owner = THIS_MODULE,
    .read  = led_read,
    .write = led_write,
};

/* ======================== sysfs ======================== */

static ssize_t brightness_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", led_dev->brightness);
}

static ssize_t brightness_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    int val;
    if (kstrtoint(buf, 10, &val) != 0)
        return -EINVAL;

    mutex_lock(&led_dev->lock);
    led_dev->brightness = val ? 1 : 0;
    gpiod_set_value(led_dev->gpio, led_dev->brightness);
    mutex_unlock(&led_dev->lock);

    return count;
}
static DEVICE_ATTR_RW(brightness);

static struct attribute *led_sysfs_attrs[] = {
    &dev_attr_brightness.attr,
    NULL,
};
ATTRIBUTE_GROUPS(led_sysfs);

/* ======================== platform_driver ======================== */

/*
 * platform_driver — platform device driver model
 *
 * Why use platform_driver instead of creating a device directly?
 *   - LED is a GPIO device, not on the I2C/SPI bus
 *   - Using platform_driver conforms to the Linux device model
 *   - The device tree has a matching compatible = "retail,led" node
 *
 * platform_driver matching process:
 *   1. The kernel parses the device tree and creates a platform_device
 *   2. Compares the platform_device's compatible with the platform_driver's of_match_table
 *   3. On match → calls probe
 */
static int led_probe(struct platform_device *pdev)
{
    int ret;
    struct device *dev = &pdev->dev;

    led_dev = devm_kzalloc(dev, sizeof(*led_dev), GFP_KERNEL);
    if (!led_dev)
        return -ENOMEM;

    mutex_init(&led_dev->lock);

    /*
     * devm_gpiod_get — get a GPIO
     *
     * Parameters:
     *   dev:  device pointer
     *   NULL: con_id is NULL, use the device tree default GPIO (the first gpios property)
     *   GPIOD_OUT_LOW: initialize as output low
     *
     * If the device tree defines multiple gpios, you can distinguish them with con_id:
     *   devm_gpiod_get(dev, "led", GPIOD_OUT_LOW)
     *   corresponds to led-gpios = <&gpio1 RK_PD0 ...>; in the device tree
     */
    led_dev->gpio = devm_gpiod_get(dev, NULL, GPIOD_OUT_LOW);
    if (IS_ERR(led_dev->gpio)) {
        dev_err(dev, "Failed to get LED GPIO\n");
        return PTR_ERR(led_dev->gpio);
    }

    led_dev->brightness = 0;

    ret = alloc_chrdev_region(&led_dev->devid, 0, 1, LED_DEV_NAME);
    if (ret) return ret;

    cdev_init(&led_dev->cdev, &led_fops);
    ret = cdev_add(&led_dev->cdev, led_dev->devid, 1);
    if (ret) {
        unregister_chrdev_region(led_dev->devid, 1);
        return ret;
    }

    led_dev->class = class_create(THIS_MODULE, LED_DEV_NAME);
    if (IS_ERR(led_dev->class)) {
        cdev_del(&led_dev->cdev);
        unregister_chrdev_region(led_dev->devid, 1);
        return PTR_ERR(led_dev->class);
    }

    device_create_with_groups(led_dev->class, dev,
                              led_dev->devid, led_dev,
                              led_sysfs_groups, LED_DEV_NAME);

    dev_info(dev, "LED driver loaded\n");
    return 0;
}

static int led_remove(struct platform_device *pdev)
{
    gpiod_set_value(led_dev->gpio, 0);

    device_destroy(led_dev->class, led_dev->devid);
    class_destroy(led_dev->class);
    cdev_del(&led_dev->cdev);
    unregister_chrdev_region(led_dev->devid, 1);

    dev_info(&pdev->dev, "LED driver removed\n");
    return 0;
}

static const struct of_device_id led_match[] = {
    { .compatible = "retail,led" },
    {}
};

static struct platform_driver led_driver = {
    .probe  = led_probe,
    .remove = led_remove,
    .driver = {
        .name           = "led",
        .of_match_table = led_match,
    },
};

module_platform_driver(led_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RetailSystem");
MODULE_DESCRIPTION("LED GPIO Driver");
