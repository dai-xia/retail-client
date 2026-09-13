/*
 * 28BYJ-48 Stepper Motor ULN2003 Driver — platform_driver + hrtimer
 */

#define pr_fmt(fmt) "motor: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
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

#define MOTOR_DEV_NAME      "motor_dev"
#define MOTOR_NUM_GPIOS     4
#define MOTOR_PHASES_HALF   8   /* Half-step: 8 beats */
#define MOTOR_PHASES_FULL   4   /* Full-step: 4 beats */
#define MOTOR_STEPS_PER_REV 4096 /* Steps per revolution in half-step mode (28BYJ-48 after reduction) */

/* Default step interval (microseconds) */
#define MOTOR_DEFAULT_INTERVAL_US  1200  /* ~15 RPM */

/* Safety limit: max steps per request */
#define MOTOR_MAX_STEPS     32768

/* Step direction */
#define MOTOR_DIR_CW        0   /* Clockwise */
#define MOTOR_DIR_CCW       1   /* Counter-clockwise */

/* Drive mode */
#define MOTOR_MODE_HALF     0   /* Half-step (8 beats, default) */
#define MOTOR_MODE_FULL     1   /* Full-step (4 beats) */

enum motor_status {
    MOTOR_IDLE = 0,
    MOTOR_RUNNING = 1,
};

/* Half-step phase table (8 beats); low 4 bits = IN1..IN4 */
static const u8 phase_half[MOTOR_PHASES_HALF] = {
    0x01,  /* A    */
    0x03,  /* AB   */
    0x02,  /* B    */
    0x06,  /* BC   */
    0x04,  /* C    */
    0x0C,  /* CD   */
    0x08,  /* D    */
    0x09,  /* DA   */
};

static const u8 phase_full[MOTOR_PHASES_FULL] = {
    0x03,  /* AB   */
    0x06,  /* BC   */
    0x0C,  /* CD   */
    0x09,  /* DA   */
};

struct motor_data {
    struct gpio_desc *gpios[MOTOR_NUM_GPIOS];

    dev_t       devid;
    struct cdev cdev;
    struct class *class;
    struct device *cls_dev;

    struct mutex        lock;
    enum motor_status   status;
    int                 direction;     /* 0=CW, 1=CCW */
    int                 mode;          /* 0=half-step, 1=full-step */
    int                 interval_us;   /* step interval (us) */

    /* Stepping state */
    int                 target_steps;  /* target step count */
    int                 current_step;  /* steps already taken */
    int                 phase_idx;     /* current beat index */
    int                 position;      /* accumulated steps (absolute value) */

    struct hrtimer      timer;
    struct work_struct  stop_work;     /* power off after stop */
};

static void motor_set_phase(struct motor_data *dev, u8 phase)
{
    int i;
    for (i = 0; i < MOTOR_NUM_GPIOS; i++)
        gpiod_set_value(dev->gpios[i], (phase >> i) & 1);
}

/* Power off all coils */
static void motor_release(struct motor_data *dev)
{
    motor_set_phase(dev, 0x00);
}

/* hrtimer callback — one step per trigger; hard IRQ context (no sleeping) */
static enum hrtimer_restart motor_step_cb(struct hrtimer *t)
{
    struct motor_data *dev = container_of(t, struct motor_data, timer);
    const u8 *phases;
    int num_phases, next_phase;

    if (dev->mode == MOTOR_MODE_FULL) {
        phases = phase_full;
        num_phases = MOTOR_PHASES_FULL;
    } else {
        phases = phase_half;
        num_phases = MOTOR_PHASES_HALF;
    }

    if (dev->direction == MOTOR_DIR_CW)
        next_phase = (dev->phase_idx + 1) % num_phases;
    else
        next_phase = (dev->phase_idx - 1 + num_phases) % num_phases;

    dev->phase_idx = next_phase;
    motor_set_phase(dev, phases[next_phase]);

    dev->current_step++;
    dev->position++;

    if (dev->current_step >= dev->target_steps) {
        dev->status = MOTOR_IDLE;
        schedule_work(&dev->stop_work);
        pr_debug("Motor completed %d steps (total: %d)\n",
                 dev->current_step, dev->position);
        return HRTIMER_NORESTART;
    }

    hrtimer_forward_now(t, ns_to_ktime((u64)dev->interval_us * 1000));
    return HRTIMER_RESTART;
}

static void motor_stop_work_fn(struct work_struct *work)
{
    struct motor_data *dev = container_of(work, struct motor_data, stop_work);
    motor_release(dev);
}

static struct motor_data *motor_from_file(struct file *filp)
{
    return (struct motor_data *)filp->private_data;
}

static int motor_open(struct inode *inode, struct file *filp)
{
    struct motor_data *dev = container_of(inode->i_cdev,
                                          struct motor_data, cdev);
    filp->private_data = dev;
    return 0;
}

/* write(int steps): positive=CW, negative=CCW, 0=stop */
static ssize_t motor_write(struct file *filp, const char __user *buf,
                           size_t count, loff_t *off)
{
    struct motor_data *dev = motor_from_file(filp);
    int steps;

    if (!dev) return -ENODEV;
    if (count < sizeof(int))
        return -EINVAL;

    if (copy_from_user(&steps, buf, sizeof(int)))
        return -EFAULT;

    mutex_lock(&dev->lock);

    if (dev->status == MOTOR_RUNNING) {
        hrtimer_cancel(&dev->timer);
        cancel_work_sync(&dev->stop_work);
        motor_release(dev);
        dev->status = MOTOR_IDLE;
    }

    if (steps == 0) {
        mutex_unlock(&dev->lock);
        return sizeof(int);
    }

    if (steps < 0) {
        dev->direction = MOTOR_DIR_CCW;
        steps = -steps;
    } else {
        dev->direction = MOTOR_DIR_CW;
    }

    if (steps > MOTOR_MAX_STEPS)
        steps = MOTOR_MAX_STEPS;

    dev->target_steps = steps;
    dev->current_step = 0;

    {
        const u8 *phases;
        int num_phases;
        if (dev->mode == MOTOR_MODE_FULL) {
            phases = phase_full;
            num_phases = MOTOR_PHASES_FULL;
        } else {
            phases = phase_half;
            num_phases = MOTOR_PHASES_HALF;
        }
        dev->phase_idx = dev->phase_idx % num_phases;
        motor_set_phase(dev, phases[dev->phase_idx]);
    }

    dev->status = MOTOR_RUNNING;

    hrtimer_start(&dev->timer,
                  ns_to_ktime((u64)dev->interval_us * 1000),
                  HRTIMER_MODE_REL);

    mutex_unlock(&dev->lock);

    pr_debug("Motor: %d steps, dir=%s, mode=%s, interval=%dus\n",
             steps,
             dev->direction == MOTOR_DIR_CW ? "CW" : "CCW",
             dev->mode == MOTOR_MODE_HALF ? "half" : "full",
             dev->interval_us);

    return sizeof(int);
}

/* read(): 0=idle, 1=running */
static ssize_t motor_read(struct file *filp, char __user *buf,
                          size_t count, loff_t *off)
{
    struct motor_data *dev = motor_from_file(filp);
    int status;

    if (!dev) return -ENODEV;
    if (count < sizeof(int))
        return -EINVAL;

    mutex_lock(&dev->lock);
    status = dev->status;
    mutex_unlock(&dev->lock);

    if (copy_to_user(buf, &status, sizeof(status)))
        return -EFAULT;

    return sizeof(status);
}

#define MOTOR_IOC_MAGIC     'M'
#define MOTOR_IOC_SET_DIR   _IOW(MOTOR_IOC_MAGIC, 1, int)
#define MOTOR_IOC_SET_SPEED _IOW(MOTOR_IOC_MAGIC, 2, int)
#define MOTOR_IOC_SET_MODE  _IOW(MOTOR_IOC_MAGIC, 3, int)
#define MOTOR_IOC_GET_DIR   _IOR(MOTOR_IOC_MAGIC, 4, int)
#define MOTOR_IOC_GET_SPEED _IOR(MOTOR_IOC_MAGIC, 5, int)
#define MOTOR_IOC_GET_MODE  _IOR(MOTOR_IOC_MAGIC, 6, int)
#define MOTOR_IOC_STOP      _IO(MOTOR_IOC_MAGIC, 7)
#define MOTOR_IOC_GET_POS   _IOR(MOTOR_IOC_MAGIC, 8, int)
#define MOTOR_IOC_RESET_POS _IO(MOTOR_IOC_MAGIC, 9)

static long motor_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct motor_data *dev = motor_from_file(filp);
    int val;

    if (!dev) return -ENODEV;

    switch (cmd) {
    case MOTOR_IOC_SET_DIR:
        if (copy_from_user(&val, (int __user *)arg, sizeof(val)))
            return -EFAULT;
        if (val != MOTOR_DIR_CW && val != MOTOR_DIR_CCW)
            return -EINVAL;
        mutex_lock(&dev->lock);
        dev->direction = val;
        mutex_unlock(&dev->lock);
        return 0;

    case MOTOR_IOC_SET_SPEED:
        if (copy_from_user(&val, (int __user *)arg, sizeof(val)))
            return -EFAULT;
        if (val < 500 || val > 10000)
            return -EINVAL;
        mutex_lock(&dev->lock);
        dev->interval_us = val;
        mutex_unlock(&dev->lock);
        return 0;

    case MOTOR_IOC_SET_MODE:
        if (copy_from_user(&val, (int __user *)arg, sizeof(val)))
            return -EFAULT;
        if (val != MOTOR_MODE_HALF && val != MOTOR_MODE_FULL)
            return -EINVAL;
        mutex_lock(&dev->lock);
        dev->mode = val;
        mutex_unlock(&dev->lock);
        return 0;

    case MOTOR_IOC_GET_DIR:
        mutex_lock(&dev->lock);
        val = dev->direction;
        mutex_unlock(&dev->lock);
        return put_user(val, (int __user *)arg);

    case MOTOR_IOC_GET_SPEED:
        mutex_lock(&dev->lock);
        val = dev->interval_us;
        mutex_unlock(&dev->lock);
        return put_user(val, (int __user *)arg);

    case MOTOR_IOC_GET_MODE:
        mutex_lock(&dev->lock);
        val = dev->mode;
        mutex_unlock(&dev->lock);
        return put_user(val, (int __user *)arg);

    case MOTOR_IOC_GET_POS:
        mutex_lock(&dev->lock);
        val = dev->position;
        mutex_unlock(&dev->lock);
        return put_user(val, (int __user *)arg);

    case MOTOR_IOC_RESET_POS:
        mutex_lock(&dev->lock);
        dev->position = 0;
        mutex_unlock(&dev->lock);
        return 0;

    case MOTOR_IOC_STOP:
        mutex_lock(&dev->lock);
        if (dev->status == MOTOR_RUNNING) {
            hrtimer_cancel(&dev->timer);
            cancel_work_sync(&dev->stop_work);
            motor_release(dev);
            dev->status = MOTOR_IDLE;
        }
        mutex_unlock(&dev->lock);
        return 0;

    default:
        return -ENOTTY;
    }
}

static struct file_operations motor_fops = {
    .owner          = THIS_MODULE,
    .open           = motor_open,
    .read           = motor_read,
    .write          = motor_write,
    .unlocked_ioctl = motor_ioctl,
};

static inline struct motor_data *to_motor(struct device *dev)
{
    return dev_get_drvdata(dev);
}

static ssize_t status_show(struct device *dev,
                           struct device_attribute *attr, char *buf)
{
    struct motor_data *m = to_motor(dev);
    return sprintf(buf, "%s\n",
                   m->status == MOTOR_RUNNING ? "running" : "idle");
}
static DEVICE_ATTR_RO(status);

static ssize_t direction_show(struct device *dev,
                              struct device_attribute *attr, char *buf)
{
    struct motor_data *m = to_motor(dev);
    return sprintf(buf, "%s\n",
                   m->direction == MOTOR_DIR_CW ? "CW" : "CCW");
}
static DEVICE_ATTR_RO(direction);

static ssize_t speed_show(struct device *dev,
                          struct device_attribute *attr, char *buf)
{
    struct motor_data *m = to_motor(dev);
    return sprintf(buf, "%d\n", m->interval_us);
}
static DEVICE_ATTR_RO(speed);

static ssize_t mode_show(struct device *dev,
                         struct device_attribute *attr, char *buf)
{
    struct motor_data *m = to_motor(dev);
    return sprintf(buf, "%s\n",
                   m->mode == MOTOR_MODE_HALF ? "half" : "full");
}
static DEVICE_ATTR_RO(mode);

static ssize_t position_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    struct motor_data *m = to_motor(dev);
    return sprintf(buf, "%d\n", m->position);
}
static DEVICE_ATTR_RO(position);

static struct attribute *motor_sysfs_attrs[] = {
    &dev_attr_status.attr,
    &dev_attr_direction.attr,
    &dev_attr_speed.attr,
    &dev_attr_mode.attr,
    &dev_attr_position.attr,
    NULL,
};
ATTRIBUTE_GROUPS(motor_sysfs);

static int motor_probe(struct platform_device *pdev)
{
    int ret, i;
    struct motor_data *dev;
    struct device *parent = &pdev->dev;
    u32 default_interval = MOTOR_DEFAULT_INTERVAL_US;
    u32 default_mode = MOTOR_MODE_HALF;

    dev = devm_kzalloc(parent, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    of_property_read_u32(parent->of_node, "default-interval-us",
                         &default_interval);

    of_property_read_u32(parent->of_node, "default-mode",
                         &default_mode);

    mutex_init(&dev->lock);
    dev->status = MOTOR_IDLE;
    dev->direction = MOTOR_DIR_CW;
    dev->mode = (int)default_mode;
    dev->interval_us = (int)default_interval;
    dev->phase_idx = 0;
    dev->position = 0;

    /* Get the 4 phase GPIOs */
    for (i = 0; i < MOTOR_NUM_GPIOS; i++) {
        dev->gpios[i] = devm_gpiod_get_index(parent, NULL, i, GPIOD_OUT_LOW);
        if (IS_ERR(dev->gpios[i])) {
            dev_err(parent, "Failed to get motor GPIO %d\n", i);
            return PTR_ERR(dev->gpios[i]);
        }
    }

    INIT_WORK(&dev->stop_work, motor_stop_work_fn);
    hrtimer_init(&dev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    dev->timer.function = motor_step_cb;

    ret = alloc_chrdev_region(&dev->devid, 0, 1, MOTOR_DEV_NAME);
    if (ret) return ret;

    cdev_init(&dev->cdev, &motor_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devid, 1);
    if (ret) {
        unregister_chrdev_region(dev->devid, 1);
        return ret;
    }

    dev->class = class_create(THIS_MODULE, MOTOR_DEV_NAME);
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        goto err_cdev;
    }

    dev->cls_dev = device_create_with_groups(dev->class, parent,
                              dev->devid, dev,
                              motor_sysfs_groups, MOTOR_DEV_NAME);
    if (IS_ERR(dev->cls_dev)) {
        ret = PTR_ERR(dev->cls_dev);
        goto err_class;
    }

    platform_set_drvdata(pdev, dev);

    dev_info(parent, "28BYJ-48 motor driver loaded (mode=%s, %dus/step, %d steps/rev)\n",
             dev->mode == MOTOR_MODE_HALF ? "half" : "full",
             dev->interval_us, MOTOR_STEPS_PER_REV);
    return 0;

err_class:
    class_destroy(dev->class);
err_cdev:
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devid, 1);
    return ret;
}

static int motor_remove(struct platform_device *pdev)
{
    struct motor_data *dev = platform_get_drvdata(pdev);

    if (!dev) return 0;

    hrtimer_cancel(&dev->timer);
    cancel_work_sync(&dev->stop_work);
    motor_release(dev);

    device_destroy(dev->class, dev->devid);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devid, 1);

    dev_info(&pdev->dev, "Motor driver removed\n");
    return 0;
}

static const struct of_device_id motor_match[] = {
    { .compatible = "retail,motor" },
    {}
};
MODULE_DEVICE_TABLE(of, motor_match);

static struct platform_driver motor_driver = {
    .probe  = motor_probe,
    .remove = motor_remove,
    .driver = {
        .name           = "motor",
        .owner          = THIS_MODULE,
        .of_match_table = motor_match,
    },
};

module_platform_driver(motor_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RetailSystem");
MODULE_DESCRIPTION("28BYJ-48 stepper motor driver (platform_driver + hrtimer)");
