/*
 * LED GPIO Driver — character device + sysfs
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

/* read() returns the last written brightness, not the actual pin level */
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

/* write(): 0 = off, non-zero = on */
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

static int led_probe(struct platform_device *pdev)
{
    int ret;
    struct device *dev = &pdev->dev;

    led_dev = devm_kzalloc(dev, sizeof(*led_dev), GFP_KERNEL);
    if (!led_dev)
        return -ENOMEM;

    mutex_init(&led_dev->lock);

    /* Get default device-tree GPIO */
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
