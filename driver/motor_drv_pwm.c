/*
 * Stepper Motor PWM Subsystem Driver — A4988 / DRV8825 / TMC2208
 *
 * Applicable chips: A4988, DRV8825, TMC2208, TB6600 and other STEP/DIR interface
 *                   stepper motor driver chips.
 *
 * Hardware wiring (RK3568 → A4988):
 *   PWM  → STEP  (pulse signal, one pulse = one step)
 *   GPIO → DIR   (direction, HIGH=CW, LOW=CCW)
 *   GPIO → EN    (enable, optional, LOW=enabled)
 *
 * Device tree:
 *   stepper {
 *       compatible = "retail,stepper-pwm";
 *       pwms = <&pwm2 0 50000>;        // PWM2_CH0, period=50us→20kHz
 *       enable-gpios = <&gpio3 12 GPIO_ACTIVE_HIGH>;  // EN
 *       dir-gpios  = <&gpio3 13 GPIO_ACTIVE_HIGH>;  // DIR
 *       default-speed-hz = <1000>;     // default 1000 steps/sec
 *       max-speed-hz = <5000>;         // max 5000 steps/sec
 *   };
 *
 * Character device: /dev/stepper_pwm
 *   write(int steps)  → rotate the specified number of steps
 *   ioctl             → set speed/direction
 *
 * ==================== Interview Knowledge Points ====================
 *
 * 1. PWM subsystem
 *
 *    The Linux PWM subsystem provides a unified PWM control interface.
 *
 *    Core API:
 *      pwm_get()        — get a PWM device from the device tree "pwms" property
 *      pwm_config()     — set period and duty_cycle
 *      pwm_enable()     — enable PWM output
 *      pwm_disable()    — disable PWM output
 *      pwm_put()        — release the PWM device
 *
 *    Device tree "pwms" property format:
 *      pwms = <&pwmX channel period_ns>;
 *      e.g.: pwms = <&pwm2 0 50000>;  // PWM2, channel 0, 50us period
 *
 *    PWM controlling A4988 stepper motor:
 *      - STEP pin connected to PWM output
 *      - one PWM pulse = motor moves one step
 *      - frequency = step rate (Hz)
 *      - 50% duty cycle is sufficient (A4988 triggers on rising edge)
 *
 * 2. A4988 driver chip
 *
 *    A4988 is Allegro's DMOS microstepping motor driver chip. Features:
 *      - supports 1/1, 1/2, 1/4, 1/8, 1/16 microstepping
 *      - up to 2A output current (needs cooling)
 *      - STEP/DIR interface, simple to use
 *      - has thermal/overcurrent protection
 *
 *    Pins:
 *      STEP    — pulse input, triggers on rising edge
 *      DIR     — direction control, HIGH=CW
 *      MS1/MS2/MS3 — microstep resolution selection
 *      ENABLE  — enable, LOW=enabled, HIGH=disabled
 *      SLEEP   — sleep, LOW=sleep
 *      RESET   — reset, LOW=reset
 *
 * 3. Comparison with ULN2003 (motor_drv.c)
 *
 *    ULN2003 (28BYJ-48):
 *      - needs 4 GPIOs switched in phase sequence
 *      - must use hrtimer to output beat by beat
 *      - accuracy affected by timer response latency
 *      - suitable for low-cost, low-precision scenarios
 *
 *    A4988 (PWM):
 *      - only needs 1 PWM + 1 GPIO (DIR)
 *      - hardware auto-generates pulses, high precision
 *      - supports microstepping, smoother
 *      - suitable for high-precision, high-speed scenarios
 *
 * 4. PWM step frequency calculation
 *
 *    period_ns = 1e9 / frequency_hz (nanoseconds)
 *    e.g.:
 *      1000 Hz → period = 1,000,000 ns = 1ms
 *      5000 Hz → period = 200,000 ns = 200us
 *
 *    duty cycle = 50% → duty_cycle = period_ns / 2
 */

#define pr_fmt(fmt) "stepper_pwm: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/gpio/consumer.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/sysfs.h>
#include <linux/of.h>

#define STEPPER_DEV_NAME    "stepper_pwm"
#define STEPPER_MAX_SPEED   10000   /* Max 10kHz step frequency */
#define STEPPER_MIN_SPEED   10      /* Min 10Hz */

/* Default PWM period (ns) — 1000 Hz */
#define STEPPER_DEFAULT_PERIOD_NS  1000000

struct stepper_data {
    struct pwm_device   *pwm;
    struct gpio_desc    *dir_gpio;
    struct gpio_desc    *enable_gpio;

    dev_t       devid;
    struct cdev cdev;
    struct class *class;
    struct device *cls_dev;

    struct mutex        lock;
    bool                running;

    int                 direction;      /* 0=CW, 1=CCW */
    int                 speed_hz;       /* current step frequency (Hz) */
    int                 period_ns;      /* PWM period (ns) */

    /* Stepping state */
    int                 target_steps;
    int                 current_step;
    int                 position;       /* accumulated steps (signed) */

    struct hrtimer      timer;
};

/* ======================== hrtimer step completion callback ======================== */

/*
 * Timer expiry = steps completed, stop PWM
 */
static enum hrtimer_restart stepper_done_cb(struct hrtimer *t)
{
    struct stepper_data *dev = container_of(t, struct stepper_data, timer);

    pwm_disable(dev->pwm);
    dev->running = false;
    pr_debug("Stepper completed %d steps (total pos: %d)\n",
             dev->current_step, dev->position);
    return HRTIMER_NORESTART;
}

/*
 * Start stepping
 *
 * Principle:
 *   1. Set the DIR GPIO
 *   2. Configure PWM period and duty cycle (50%)
 *   3. Enable PWM → hardware starts generating pulses
 *   4. Start hrtimer to wait for step completion
 *
 * Duration calculation:
 *   duration_ns = steps * 1e9 / speed_hz
 *   e.g.: 1000 steps @ 1000Hz → 1,000,000,000 ns = 1 second
 */
static int stepper_start(struct stepper_data *dev, int steps)
{
    u64 duration_ns;

    if (dev->speed_hz <= 0)
        return -EINVAL;

    /* Set direction */
    gpiod_set_value(dev->dir_gpio, dev->direction);

    /* Configure PWM: 50% duty cycle */
    pwm_config(dev->pwm, dev->period_ns / 2, dev->period_ns);

    /* Enable PWM */
    pwm_enable(dev->pwm);

    dev->target_steps = steps;
    dev->current_step = 0;
    dev->running = true;

    /* Compute duration and start the timer */
    duration_ns = (u64)steps * 1000000000ULL / dev->speed_hz;
    hrtimer_start(&dev->timer, ns_to_ktime(duration_ns), HRTIMER_MODE_REL);

    return 0;
}

/* ======================== Character device operations ======================== */

static struct stepper_data *stepper_from_file(struct file *filp)
{
    return (struct stepper_data *)filp->private_data;
}

static int stepper_open(struct inode *inode, struct file *filp)
{
    struct stepper_data *dev = container_of(inode->i_cdev,
                                             struct stepper_data, cdev);
    filp->private_data = dev;

    /* Enable the driver chip */
    if (dev->enable_gpio)
        gpiod_set_value(dev->enable_gpio, 0);  /* LOW = enabled */

    return 0;
}

static int stepper_release(struct inode *inode, struct file *filp)
{
    struct stepper_data *dev = stepper_from_file(filp);

    if (dev->running) {
        hrtimer_cancel(&dev->timer);
        pwm_disable(dev->pwm);
        dev->running = false;
    }

    /* Disable the driver chip */
    if (dev->enable_gpio)
        gpiod_set_value(dev->enable_gpio, 1);

    return 0;
}

/*
 * write(int steps) — rotate the specified number of steps
 *
 * Positive = CW, negative = CCW
 */
static ssize_t stepper_write(struct file *filp, const char __user *buf,
                             size_t count, loff_t *off)
{
    struct stepper_data *dev = stepper_from_file(filp);
    int steps;

    if (!dev) return -ENODEV;
    if (count < sizeof(int))
        return -EINVAL;

    if (copy_from_user(&steps, buf, sizeof(int)))
        return -EFAULT;

    mutex_lock(&dev->lock);

    /* Stop any running stepping */
    if (dev->running) {
        hrtimer_cancel(&dev->timer);
        pwm_disable(dev->pwm);
        dev->running = false;
    }

    if (steps == 0) {
        mutex_unlock(&dev->lock);
        return sizeof(int);
    }

    if (steps < 0) {
        dev->direction = 1;            /* CCW */
        steps = -steps;
    } else {
        dev->direction = 0;            /* CW */
    }

    if (stepper_start(dev, steps) == 0) {
        /* Update accumulated position */
        if (dev->direction == 0)
            dev->position += steps;
        else
            dev->position -= steps;
    }

    mutex_unlock(&dev->lock);

    pr_debug("Stepper: %d steps @ %d Hz, dir=%s\n",
             steps, dev->speed_hz,
             dev->direction ? "CCW" : "CW");

    return sizeof(int);
}

static ssize_t stepper_read(struct file *filp, char __user *buf,
                            size_t count, loff_t *off)
{
    struct stepper_data *dev = stepper_from_file(filp);
    int status;

    if (!dev) return -ENODEV;
    if (count < sizeof(int))
        return -EINVAL;

    status = dev->running ? 1 : 0;

    if (copy_to_user(buf, &status, sizeof(status)))
        return -EFAULT;

    return sizeof(status);
}

/* ======================== ioctl ======================== */

#define STEPPER_IOC_MAGIC      'S'
#define STEPPER_IOC_SET_SPEED  _IOW(STEPPER_IOC_MAGIC, 1, int)
#define STEPPER_IOC_GET_SPEED  _IOR(STEPPER_IOC_MAGIC, 2, int)
#define STEPPER_IOC_SET_DIR    _IOW(STEPPER_IOC_MAGIC, 3, int)
#define STEPPER_IOC_GET_DIR    _IOR(STEPPER_IOC_MAGIC, 4, int)
#define STEPPER_IOC_STOP       _IO(STEPPER_IOC_MAGIC, 5)
#define STEPPER_IOC_GET_POS    _IOR(STEPPER_IOC_MAGIC, 6, int)
#define STEPPER_IOC_RESET_POS  _IO(STEPPER_IOC_MAGIC, 7)

static long stepper_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct stepper_data *dev = stepper_from_file(filp);
    int val;

    if (!dev) return -ENODEV;

    switch (cmd) {
    case STEPPER_IOC_SET_SPEED:
        if (copy_from_user(&val, (int __user *)arg, sizeof(val)))
            return -EFAULT;
        if (val < STEPPER_MIN_SPEED || val > STEPPER_MAX_SPEED)
            return -EINVAL;
        mutex_lock(&dev->lock);
        dev->speed_hz = val;
        /*
         * Compute the PWM period from the speed:
         *   period_ns = 1e9 / speed_hz
         *   But the PWM period can't be too small; ensure at least 50% duty
         *   has sufficient pulse width
         */
        dev->period_ns = 1000000000 / val;
        if (dev->period_ns < 1000)   /* Min 1us period (1MHz) */
            dev->period_ns = 1000;
        mutex_unlock(&dev->lock);
        return 0;

    case STEPPER_IOC_GET_SPEED:
        val = dev->speed_hz;
        return put_user(val, (int __user *)arg);

    case STEPPER_IOC_SET_DIR:
        if (copy_from_user(&val, (int __user *)arg, sizeof(val)))
            return -EFAULT;
        if (val != 0 && val != 1)
            return -EINVAL;
        mutex_lock(&dev->lock);
        dev->direction = val;
        mutex_unlock(&dev->lock);
        return 0;

    case STEPPER_IOC_GET_DIR:
        val = dev->direction;
        return put_user(val, (int __user *)arg);

    case STEPPER_IOC_STOP:
        mutex_lock(&dev->lock);
        if (dev->running) {
            hrtimer_cancel(&dev->timer);
            pwm_disable(dev->pwm);
            dev->running = false;
        }
        mutex_unlock(&dev->lock);
        return 0;

    case STEPPER_IOC_GET_POS:
        val = dev->position;
        return put_user(val, (int __user *)arg);

    case STEPPER_IOC_RESET_POS:
        dev->position = 0;
        return 0;

    default:
        return -ENOTTY;
    }
}

static struct file_operations stepper_fops = {
    .owner          = THIS_MODULE,
    .open           = stepper_open,
    .release        = stepper_release,
    .read           = stepper_read,
    .write          = stepper_write,
    .unlocked_ioctl = stepper_ioctl,
};

/* ======================== sysfs ======================== */

static inline struct stepper_data *to_stepper(struct device *dev)
{
    return dev_get_drvdata(dev);
}

static ssize_t speed_show(struct device *dev,
                          struct device_attribute *attr, char *buf)
{
    struct stepper_data *s = to_stepper(dev);
    return sprintf(buf, "%d\n", s->speed_hz);
}

static ssize_t speed_store(struct device *dev,
                           struct device_attribute *attr,
                           const char *buf, size_t count)
{
    struct stepper_data *s = to_stepper(dev);
    int val;

    if (kstrtoint(buf, 10, &val) != 0)
        return -EINVAL;
    if (val < STEPPER_MIN_SPEED || val > STEPPER_MAX_SPEED)
        return -EINVAL;

    mutex_lock(&s->lock);
    s->speed_hz = val;
    s->period_ns = 1000000000 / val;
    if (s->period_ns < 1000)
        s->period_ns = 1000;
    mutex_unlock(&s->lock);

    return count;
}
static DEVICE_ATTR_RW(speed);

static ssize_t direction_show(struct device *dev,
                              struct device_attribute *attr, char *buf)
{
    struct stepper_data *s = to_stepper(dev);
    return sprintf(buf, "%s\n", s->direction ? "CCW" : "CW");
}
static DEVICE_ATTR_RO(direction);

static ssize_t position_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    struct stepper_data *s = to_stepper(dev);
    return sprintf(buf, "%d\n", s->position);
}
static DEVICE_ATTR_RO(position);

static struct attribute *stepper_sysfs_attrs[] = {
    &dev_attr_speed.attr,
    &dev_attr_direction.attr,
    &dev_attr_position.attr,
    NULL,
};
ATTRIBUTE_GROUPS(stepper_sysfs);

/* ======================== platform_driver ======================== */

static int stepper_pwm_probe(struct platform_device *pdev)
{
    int ret;
    struct stepper_data *dev;
    struct device *parent = &pdev->dev;
    u32 default_speed = 1000;

    dev = devm_kzalloc(parent, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    mutex_init(&dev->lock);
    dev->running = false;
    dev->direction = 0;
    dev->position = 0;

    /* Parse device tree properties */
    of_property_read_u32(parent->of_node, "default-speed-hz", &default_speed);
    dev->speed_hz = (int)default_speed;
    dev->period_ns = 1000000000 / dev->speed_hz;

    /* Get the PWM device */
    dev->pwm = devm_pwm_get(parent, NULL);
    if (IS_ERR(dev->pwm)) {
        ret = PTR_ERR(dev->pwm);
        if (ret != -EPROBE_DEFER)
            dev_err(parent, "Failed to get PWM: %d\n", ret);
        return ret;
    }

    /* Get the DIR GPIO */
    dev->dir_gpio = devm_gpiod_get(parent, "dir", GPIOD_OUT_LOW);
    if (IS_ERR(dev->dir_gpio)) {
        dev_err(parent, "Failed to get DIR GPIO\n");
        return PTR_ERR(dev->dir_gpio);
    }

    /* Get the ENABLE GPIO (optional) */
    dev->enable_gpio = devm_gpiod_get_optional(parent, "step", GPIOD_OUT_HIGH);
    if (IS_ERR(dev->enable_gpio))
        dev->enable_gpio = NULL;

    /* Initialize timer */
    hrtimer_init(&dev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    dev->timer.function = stepper_done_cb;

    /* Register char device */
    ret = alloc_chrdev_region(&dev->devid, 0, 1, STEPPER_DEV_NAME);
    if (ret) return ret;

    cdev_init(&dev->cdev, &stepper_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devid, 1);
    if (ret) {
        unregister_chrdev_region(dev->devid, 1);
        return ret;
    }

    dev->class = class_create(THIS_MODULE, STEPPER_DEV_NAME);
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        goto err_cdev;
    }

    dev->cls_dev = device_create_with_groups(dev->class, parent,
                              dev->devid, dev,
                              stepper_sysfs_groups, STEPPER_DEV_NAME);
    if (IS_ERR(dev->cls_dev)) {
        ret = PTR_ERR(dev->cls_dev);
        goto err_class;
    }

    platform_set_drvdata(pdev, dev);

    dev_info(parent, "PWM Stepper driver loaded (speed=%dHz, period=%dns)\n",
             dev->speed_hz, dev->period_ns);
    return 0;

err_class:
    class_destroy(dev->class);
err_cdev:
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devid, 1);
    return ret;
}

static int stepper_pwm_remove(struct platform_device *pdev)
{
    struct stepper_data *dev = platform_get_drvdata(pdev);

    if (!dev) return 0;

    hrtimer_cancel(&dev->timer);
    pwm_disable(dev->pwm);

    device_destroy(dev->class, dev->devid);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devid, 1);

    dev_info(&pdev->dev, "PWM Stepper driver removed\n");
    return 0;
}

static const struct of_device_id stepper_pwm_match[] = {
    { .compatible = "retail,stepper-pwm" },
    {}
};
MODULE_DEVICE_TABLE(of, stepper_pwm_match);

static struct platform_driver stepper_pwm_driver = {
    .probe  = stepper_pwm_probe,
    .remove = stepper_pwm_remove,
    .driver = {
        .name           = "stepper_pwm",
        .owner          = THIS_MODULE,
        .of_match_table = stepper_pwm_match,
    },
};

module_platform_driver(stepper_pwm_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RetailSystem");
MODULE_DESCRIPTION("A4988/DRV8825 stepper motor driver (PWM subsystem + hrtimer)");
