/*
 * LED PWM Driver — character device + PWM subsystem
 */

#define pr_fmt(fmt) "led_pwm: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#define LED_PWM_DEV_NAME  "led_pwm"
#define LED_MAX_BRIGHTNESS 255
#define LED_PWM_PERIOD_NS  1000000    /* 1ms → 1kHz */

struct led_pwm_data {
    struct pwm_device *pwm;
    int period_ns;

    dev_t       devid;
    struct cdev cdev;
    struct class *class;

    struct mutex lock;
    int brightness;   /* 0 ~ 255 */
};

static struct led_pwm_data *led_pwm_dev;

/* write(): 0 = off, 1~255 = brightness */
static ssize_t led_pwm_write(struct file *filp, const char __user *buf,
                             size_t count, loff_t *off)
{
    int brightness;
    int duty_ns;

    if (count < sizeof(int))
        return -EINVAL;

    if (copy_from_user(&brightness, buf, sizeof(int)))
        return -EFAULT;

    if (brightness < 0)
        brightness = 0;
    if (brightness > LED_MAX_BRIGHTNESS)
        brightness = LED_MAX_BRIGHTNESS;

    mutex_lock(&led_pwm_dev->lock);

    led_pwm_dev->brightness = brightness;

    if (brightness == 0) {
        pwm_disable(led_pwm_dev->pwm);
    } else {
        /* multiply before divide to keep integer precision */
        duty_ns = brightness * led_pwm_dev->period_ns / LED_MAX_BRIGHTNESS;
        pwm_config(led_pwm_dev->pwm, duty_ns, led_pwm_dev->period_ns);
        pwm_enable(led_pwm_dev->pwm);
    }

    mutex_unlock(&led_pwm_dev->lock);
    return sizeof(int);
}

static ssize_t led_pwm_read(struct file *filp, char __user *buf,
                            size_t count, loff_t *off)
{
    int val;

    if (count < sizeof(int))
        return -EINVAL;

    mutex_lock(&led_pwm_dev->lock);
    val = led_pwm_dev->brightness;
    mutex_unlock(&led_pwm_dev->lock);

    if (copy_to_user(buf, &val, sizeof(val)))
        return -EFAULT;

    return sizeof(val);
}

static struct file_operations led_pwm_fops = {
    .owner = THIS_MODULE,
    .read  = led_pwm_read,
    .write = led_pwm_write,
};

static ssize_t brightness_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", led_pwm_dev->brightness);
}

static ssize_t brightness_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    int val, duty_ns;
    if (kstrtoint(buf, 10, &val) != 0)
        return -EINVAL;
    if (val < 0) val = 0;
    if (val > LED_MAX_BRIGHTNESS) val = LED_MAX_BRIGHTNESS;

    mutex_lock(&led_pwm_dev->lock);
    led_pwm_dev->brightness = val;
    if (val == 0) {
        pwm_disable(led_pwm_dev->pwm);
    } else {
        duty_ns = val * led_pwm_dev->period_ns / LED_MAX_BRIGHTNESS;
        pwm_config(led_pwm_dev->pwm, duty_ns, led_pwm_dev->period_ns);
        pwm_enable(led_pwm_dev->pwm);
    }
    mutex_unlock(&led_pwm_dev->lock);
    return count;
}
static DEVICE_ATTR_RW(brightness);

static struct attribute *led_pwm_sysfs_attrs[] = {
    &dev_attr_brightness.attr,
    NULL,
};
ATTRIBUTE_GROUPS(led_pwm_sysfs);

static int led_pwm_probe(struct platform_device *pdev)
{
    int ret;
    struct device *dev = &pdev->dev;

    led_pwm_dev = devm_kzalloc(dev, sizeof(*led_pwm_dev), GFP_KERNEL);
    if (!led_pwm_dev)
        return -ENOMEM;

    mutex_init(&led_pwm_dev->lock);
    led_pwm_dev->period_ns = LED_PWM_PERIOD_NS;

    led_pwm_dev->pwm = devm_pwm_get(dev, NULL);
    if (IS_ERR(led_pwm_dev->pwm)) {
        ret = PTR_ERR(led_pwm_dev->pwm);
        if (ret != -EPROBE_DEFER)
            dev_err(dev, "Failed to get PWM: %d\n", ret);
        return ret;
    }

    /* Initial duty 0 (LED off) */
    pwm_config(led_pwm_dev->pwm, 0, led_pwm_dev->period_ns);

    led_pwm_dev->brightness = 0;

    ret = alloc_chrdev_region(&led_pwm_dev->devid, 0, 1, LED_PWM_DEV_NAME);
    if (ret) return ret;

    cdev_init(&led_pwm_dev->cdev, &led_pwm_fops);
    ret = cdev_add(&led_pwm_dev->cdev, led_pwm_dev->devid, 1);
    if (ret) {
        unregister_chrdev_region(led_pwm_dev->devid, 1);
        return ret;
    }

    led_pwm_dev->class = class_create(THIS_MODULE, LED_PWM_DEV_NAME);
    if (IS_ERR(led_pwm_dev->class)) {
        cdev_del(&led_pwm_dev->cdev);
        unregister_chrdev_region(led_pwm_dev->devid, 1);
        return PTR_ERR(led_pwm_dev->class);
    }

    device_create_with_groups(led_pwm_dev->class, dev,
                              led_pwm_dev->devid, led_pwm_dev,
                              led_pwm_sysfs_groups, LED_PWM_DEV_NAME);

    dev_info(dev, "LED PWM driver loaded (period=%dus, max_brightness=%d)\n",
             led_pwm_dev->period_ns / 1000, LED_MAX_BRIGHTNESS);
    return 0;
}

static int led_pwm_remove(struct platform_device *pdev)
{
    pwm_disable(led_pwm_dev->pwm);

    device_destroy(led_pwm_dev->class, led_pwm_dev->devid);
    class_destroy(led_pwm_dev->class);
    cdev_del(&led_pwm_dev->cdev);
    unregister_chrdev_region(led_pwm_dev->devid, 1);

    dev_info(&pdev->dev, "LED PWM driver removed\n");
    return 0;
}

static const struct of_device_id led_pwm_match[] = {
    { .compatible = "retail,led_pwm" },
    {}
};

static struct platform_driver led_pwm_driver = {
    .probe  = led_pwm_probe,
    .remove = led_pwm_remove,
    .driver = {
        .name           = "led_pwm",
        .of_match_table = led_pwm_match,
    },
};

module_platform_driver(led_pwm_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LED PWM driver using Linux PWM subsystem");
