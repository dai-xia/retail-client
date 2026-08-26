/*
 * LED PWM Driver — Character Device + PWM subsystem
 *
 * Platform: RK3568 (Topeet development board)
 * Bus: PWM (PWM1_CH0)
 *
 * Features:
 *   - Character device /dev/led_pwm (write 0~255 to control brightness)
 *   - Uses the Linux PWM subsystem for true analog dimming
 *   - Brightness 0 = PWM fully off
 *   - Brightness 255 = PWM 100% duty cycle
 *
 * ==================== Interview Knowledge: PWM Subsystem ====================
 *
 * 1. What is PWM
 *    Pulse Width Modulation.
 *    A square wave of fixed frequency; the proportion of high-level time to the
 *    total period = duty cycle. For an LED, 100% duty cycle = brightest, 50% =
 *    half bright, 0% = off. The human eye's persistence of vision averages the
 *    fast flickering into "brightness".
 *
 * 2. PWM subsystem architecture
 *
 *    Userspace
 *      │
 *      │ write(/dev/led_pwm, &brightness)
 *      ▼
 *    ┌─────────────┐
 *    │  Char device │  ← led_fops.write → pwm_config / pwm_enable
 *    └──────┬──────┘
 *           │
 *    ┌──────▼──────┐
 *    │  PWM core   │  ← drivers/pwm/core.c
 *    │  pwm_config()│     manages the global namespace of PWM channels
 *    │  pwm_enable()│     provides APIs such as pwm_get / pwm_put
 *    │  pwm_disable()│
 *    └──────┬──────┘
 *           │
 *    ┌──────▼──────┐
 *    │  PWM ctrl   │  ← drivers/pwm/pwm-rockchip.c
 *    │  (Rockchip) │     operates RK3568 PWM hardware registers
 *    └──────┬──────┘
 *           │
 *    ┌──────▼──────┐
 *    │  GPIO pin   │  ← physical IO outputs the PWM waveform
 *    └─────────────┘
 *
 * 3. PWM core API
 *
 *    #include <linux/pwm.h>
 *
 *    // Get the PWM device (in probe stage)
 *    struct pwm_device *pwm = devm_pwm_get(dev, NULL);
 *    // NULL = con_id, corresponds to the default pwms property in the device tree
 *
 *    // Configure duty cycle and period (in nanoseconds)
 *    int pwm_config(struct pwm_device *pwm, int duty_ns, int period_ns);
 *
 *    // Enable/disable PWM output
 *    pwm_enable(pwm);
 *    pwm_disable(pwm);
 *
 *    // Set polarity (active-high/active-low)
 *    int pwm_set_polarity(struct pwm_device *pwm, enum pwm_polarity polarity);
 *
 * 4. Device tree configuration
 *
 *    led_pwm {
 *        compatible = "retail,led_pwm";
 *        pwms = <&pwm1 0 1000000>;   // PWM1, channel 0, period=1ms=1000000ns
 *        // frequency = 1/period = 1kHz
 *    };
 *
 *    RK3568 PWM controller (docs: Rockchip PWM has 4 channels, each 1 channel):
 *      pwm0:  PWM0  (GPIO0_B7)
 *      pwm1:  PWM1  (GPIO4_C6)
 *      pwm2:  PWM2  (GPIO4_C2)
 *      pwm3:  PWM3  (GPIO0_C2)
 *      ...
 *      pwm15: PWM15 (GPIO3_B1)
 *
 * 5. Duty cycle calculation
 *
 *    User writes brightness (0~255):
 *      duty_ns = brightness * period_ns / 255
 *
 *    For example period=1000000ns (1kHz), brightness=128:
 *      duty_ns = 128 * 1000000 / 255 ≈ 501960ns → 50.2% duty cycle
 *
 * 6. PWM vs GPIO for LED control
 *
 *    GPIO approach:
 *      Only on (1) or off (0)
 *      Suitable for: status indicators
 *      Drawback: cannot adjust brightness
 *
 *    PWM approach:
 *      Continuous 0~255 brightness levels
 *      Suitable for: backlight / fill light
 *      Advantages: precise control of light intensity, adapts to ambient light
 *
 * 7. Application scenarios
 *
 *    Face recognition fill light:
 *      Ambient light sensor (BH1750) reads lux → calculates needed fill brightness from lux →
 *      write(fd, &brightness, 4) → PWM auto-outputs the corresponding duty cycle
 *
 *      Dark (lux<100): brightness=255 → 100% duty → brightest
 *      Dim (lux<500): brightness=180 → 71% duty → medium
 *      Normal (lux<1000): brightness=120 → 47% duty → low
 *      Bright (lux≥1000): brightness=50  → 20% duty → dim
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

/*
 * write() — set LED brightness
 *
 * Write int value: 0~255
 *   0   → PWM disable (LED off)
 *   1~255 → pwm_config(duty, period) + pwm_enable
 *
 * Duty cycle calculation:
 *   duty_ns = brightness * period_ns / 255
 */
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
        /*
         * duty_ns = (brightness / 255) * period_ns
         * Multiply first, then divide, to avoid floating point
         */
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

/* ======================== sysfs ======================== */

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

/* ======================== platform_driver ======================== */

static int led_pwm_probe(struct platform_device *pdev)
{
    int ret;
    struct device *dev = &pdev->dev;

    led_pwm_dev = devm_kzalloc(dev, sizeof(*led_pwm_dev), GFP_KERNEL);
    if (!led_pwm_dev)
        return -ENOMEM;

    mutex_init(&led_pwm_dev->lock);
    led_pwm_dev->period_ns = LED_PWM_PERIOD_NS;

    /*
     * devm_pwm_get — get the PWM device
     *
     * Corresponds to the device tree:
     *   pwms = <&pwm1 0 1000000>;
     *            ↑   ↑  ↑
     *            │   │  └── period (ns), can be overridden by the driver
     *            │   └──── channel (channel number within the PWM controller)
     *            └──────── PWM controller phandle
     */
    led_pwm_dev->pwm = devm_pwm_get(dev, NULL);
    if (IS_ERR(led_pwm_dev->pwm)) {
        ret = PTR_ERR(led_pwm_dev->pwm);
        if (ret != -EPROBE_DEFER)
            dev_err(dev, "Failed to get PWM: %d\n", ret);
        return ret;
    }

    /*
     * Set the initial period (this overrides the device tree default)
     *
     * pwm_config parameters:
     *   duty_ns  = 0 (initial duty 0, LED off)
     *   period_ns = LED_PWM_PERIOD_NS
     */
    pwm_config(led_pwm_dev->pwm, 0, led_pwm_dev->period_ns);

    led_pwm_dev->brightness = 0;

    /* Character device */
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
