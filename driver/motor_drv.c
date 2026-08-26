/*
 * 28BYJ-48 Stepper Motor ULN2003 Driver — platform_driver + hrtimer (Refactored)
 *
 * Motor model: 28BYJ-48 (5V four-phase five-wire geared stepper motor)
 * Driver board: ULN2003 Darlington array driver board
 *
 * Parameters:
 *   - Step angle: 5.625°/64 (after gear reduction)
 *   - Reduction ratio: 1:64
 *   - Steps per revolution: 4096 (half-step drive, 8 beats)
 *   - Steps per revolution: 2048 (full-step drive, 4 beats)
 *   - Recommended speed: ~15 RPM (half-step, 1ms interval)
 *
 * Hardware wiring (RK3568 GPIO → ULN2003):
 *   GPIO1_C7  → IN1 (phase A)
 *   GPIO1_D0  → IN2 (phase B)
 *   GPIO1_D1  → IN3 (phase C)
 *   GPIO1_D2  → IN4 (phase D)
 *   Motor red wire → VCC (5V)
 *   ULN2003 GND → GND
 *
 * Device tree:
 *   motor {
 *       compatible = "retail,motor";
 *       gpios = <&gpio1 23 GPIO_ACTIVE_HIGH>,   // IN1: GPIO1_C7
 *               <&gpio1 24 GPIO_ACTIVE_HIGH>,   // IN2: GPIO1_D0
 *               <&gpio1 25 GPIO_ACTIVE_HIGH>,   // IN3: GPIO1_D1
 *               <&gpio1 26 GPIO_ACTIVE_HIGH>;   // IN4: GPIO1_D2
 *       default-interval-us = <1200>;
 *       default-mode = <0>;                    // 0=half, 1=full
 *   };
 *
 * Character device: /dev/motor_dev
 *   write(int step_count) → motor rotates the specified number of steps then auto-stops (non-blocking)
 *   read()                → read motor status (0=idle, 1=running)
 *   ioctl()               → set direction/speed/mode
 *
 * sysfs: /sys/class/motor_dev/motor_dev/
 *   status    — running status
 *   direction — direction (0=CW, 1=CCW)
 *   speed     — current step interval (us)
 *   mode      — drive mode (0=half-step, 1=full-step)
 *   position  — accumulated step count (absolute value)
 *
 * ==================== Interview Knowledge Points ====================
 *
 * 1. platform_driver framework
 *
 *    platform_driver is the driver framework in the Linux device model for
 *    platform devices (non-enumerable-bus devices). Unlike I2C/SPI/USB bus
 *    drivers, platform devices are registered statically via the device tree
 *    (or platform code).
 *
 *    Core flow:
 *      module_platform_driver(motor_driver)
 *        → motor_probe(struct platform_device *pdev)
 *            → parse device tree → request resources → register char device → return 0
 *        → motor_remove(struct platform_device *pdev)
 *            → release resources → delete char device → return 0
 *
 *    Matching mechanism:
 *      of_match_table = { .compatible = "retail,motor" }
 *      At boot the kernel scans the device tree, finds nodes whose compatible
 *      matches, automatically creates a platform_device and calls the matching
 *      driver's probe function.
 *
 *    Differences from I2C/SPI drivers:
 *      - platform_driver: memory-mapped IO, GPIO, interrupts and other "on-chip" resources
 *      - i2c_driver: devices on the I2C bus (e.g. BH1750)
 *      - spi_driver: devices on the SPI bus (e.g. RC522)
 *
 * 2. devm_* resource management
 *
 *    devm_kzalloc / devm_gpiod_get_index / devm_request_irq and other functions
 *    with the "devm" prefix automatically release resources when the driver is
 *    unloaded; no need to manually call kfree/gpiod_put/free_irq in remove.
 *
 *    Resource release order: reverse of request order (LIFO).
 *
 * 3. platform_set_drvdata / platform_get_drvdata
 *
 *    Associates the driver's private data pointer with the platform_device.
 *    In probe, call platform_set_drvdata(pdev, data); in remove and open,
 *    retrieve it via platform_get_drvdata.
 *
 *    ★ Refactoring key point: eliminate the global variable motor_dev; instead,
 *       obtain device data via container_of or platform_get_drvdata in
 *       file_operations.
 *
 * 4. hrtimer non-blocking stepping
 *
 *    Each hrtimer cycle outputs one beat; in the expiry callback:
 *      - switch to the next beat → set GPIO
 *      - if steps not done → restart hrtimer
 *      - if steps done → stop, release GPIO
 *
 *    Compared to the PWM approach:
 *      - ULN2003 needs 4 GPIOs switched in sequence, so hrtimer is required
 *      - A4988/DRV8825 only have STEP/DIR pins, use the PWM subsystem
 *        (see motor_drv_pwm.c)
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

/*
 * Half-step drive phase sequence table (8 beats)
 * The low 4 bits of each byte correspond to IN1/IN2/IN3/IN4 respectively
 */
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

/*
 * Full-step drive phase sequence table (4 beats)
 */
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

/* ======================== GPIO phase output ======================== */

static void motor_set_phase(struct motor_data *dev, u8 phase)
{
    int i;
    for (i = 0; i < MOTOR_NUM_GPIOS; i++)
        gpiod_set_value(dev->gpios[i], (phase >> i) & 1);
}

/* Power off: all GPIOs low, reduces power consumption and heat */
static void motor_release(struct motor_data *dev)
{
    motor_set_phase(dev, 0x00);
}

/* ======================== hrtimer step callback ======================== */

/*
 * hrtimer callback — one step per trigger
 *
 * Executes in hard interrupt context:
 *   - cannot call mutex_lock
 *   - only simple hardware operations
 *   - hrtimer_forward_now + HRTIMER_RESTART implements periodic timing
 */
static enum hrtimer_restart motor_step_cb(struct hrtimer *t)
{
    struct motor_data *dev = container_of(t, struct motor_data, timer);
    const u8 *phases;
    int num_phases, next_phase;

    /* Select the phase table */
    if (dev->mode == MOTOR_MODE_FULL) {
        phases = phase_full;
        num_phases = MOTOR_PHASES_FULL;
    } else {
        phases = phase_half;
        num_phases = MOTOR_PHASES_HALF;
    }

    /* Compute the next beat */
    if (dev->direction == MOTOR_DIR_CW)
        next_phase = (dev->phase_idx + 1) % num_phases;
    else
        next_phase = (dev->phase_idx - 1 + num_phases) % num_phases;

    dev->phase_idx = next_phase;
    motor_set_phase(dev, phases[next_phase]);

    dev->current_step++;
    dev->position++;

    /* Check if done */
    if (dev->current_step >= dev->target_steps) {
        dev->status = MOTOR_IDLE;
        schedule_work(&dev->stop_work);
        pr_debug("Motor completed %d steps (total: %d)\n",
                 dev->current_step, dev->position);
        return HRTIMER_NORESTART;
    }

    /* Continue: restart the timer */
    hrtimer_forward_now(t, ns_to_ktime((u64)dev->interval_us * 1000));
    return HRTIMER_RESTART;
}

/*
 * stop_work callback — power off in process context
 */
static void motor_stop_work_fn(struct work_struct *work)
{
    struct motor_data *dev = container_of(work, struct motor_data, stop_work);
    motor_release(dev);
}

/* ======================== Character device operations ======================== */

/*
 * Get motor_data from cdev
 *
 * ★ Refactoring key point: no global variable; instead, derive the containing
 *    motor_data struct from the cdev pointer via container_of.
 */
static struct motor_data *motor_from_file(struct file *filp)
{
    /*
     * filp->private_data is set in open and points to motor_data.
     * If open did not set it, it can be obtained via container_of from inode->i_cdev.
     */
    return (struct motor_data *)filp->private_data;
}

static int motor_open(struct inode *inode, struct file *filp)
{
    /*
     * Get motor_data via inode->i_cdev.
     * This is because inode->i_cdev points to the cdev struct registered by cdev_init,
     * and motor_data contains the cdev member, so container_of can be used.
     *
     * container_of principle:
     *   container_of(ptr, type, member)
     *   = (type *)((char *)ptr - offsetof(type, member))
     *
     *   i.e. given a member's address, derive the address of the containing struct.
     */
    struct motor_data *dev = container_of(inode->i_cdev,
                                          struct motor_data, cdev);
    filp->private_data = dev;
    return 0;
}

/*
 * write(int step_count) — start the motor to rotate the specified number of steps
 *
 * Positive = CW steps, negative = CCW steps (absolute value used)
 * 0 = stop immediately
 */
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

    /* If currently running, stop first */
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

    /* Direction: positive = CW, negative = CCW */
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

    /* Output the first beat */
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

/*
 * read() — read motor status
 * Returns: 0=idle, 1=running
 */
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

/* ======================== ioctl ======================== */

#define MOTOR_IOC_MAGIC     'M'
#define MOTOR_IOC_SET_DIR   _IOW(MOTOR_IOC_MAGIC, 1, int)
#define MOTOR_IOC_SET_SPEED _IOW(MOTOR_IOC_MAGIC, 2, int)
#define MOTOR_IOC_SET_MODE  _IOW(MOTOR_IOC_MAGIC, 3, int)
#define MOTOR_IOC_GET_DIR   _IOR(MOTOR_IOC_MAGIC, 4, int)
#define MOTOR_IOC_GET_SPEED _IOR(MOTOR_IOC_MAGIC, 5, int)
#define MOTOR_IOC_GET_MODE  _IOR(MOTOR_IOC_MAGIC, 6, int)
#define MOTOR_IOC_STOP      _IO(MOTOR_IOC_MAGIC, 7)
#define MOTOR_IOC_GET_POS   _IOR(MOTOR_IOC_MAGIC, 8, int)  /* newly added */
#define MOTOR_IOC_RESET_POS _IO(MOTOR_IOC_MAGIC, 9)         /* newly added */

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

/* ======================== sysfs ======================== */

/*
 * ★ Refactoring key point: sysfs attribute functions no longer use the global
 *    variable motor_dev; instead obtain it via dev_get_drvdata.
 *
 *    dev_get_drvdata returns the private data pointer set by device_create via
 *    the fourth argument of device_create_with_groups.
 */

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

/* ======================== platform_driver ======================== */

/*
 * motor_probe — initialization function after platform device matched
 *
 * Called when: the kernel scans the device tree, finds a node with
 *              compatible="retail,motor", auto-creates a platform_device,
 *              and calls this function.
 *
 * Returns: 0 success, negative errno
 *
 * ★ Interview focus: what should probe do?
 *   1. Allocate device private data (devm_kzalloc)
 *   2. Parse device tree properties (of_property_read_*, devm_gpiod_get_index)
 *   3. Initialize locks/timers/work queues
 *   4. Register char device (alloc_chrdev_region + cdev_init + cdev_add)
 *   5. Create device node (class_create + device_create)
 *   6. Call platform_set_drvdata to bind data
 */
static int motor_probe(struct platform_device *pdev)
{
    int ret, i;
    struct motor_data *dev;
    struct device *parent = &pdev->dev;
    u32 default_interval = MOTOR_DEFAULT_INTERVAL_US;
    u32 default_mode = MOTOR_MODE_HALF;

    /* 1. Allocate device private data */
    dev = devm_kzalloc(parent, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    /* 2. Parse device tree properties */

    /* Get default step interval */
    of_property_read_u32(parent->of_node, "default-interval-us",
                         &default_interval);

    /* Get default drive mode */
    of_property_read_u32(parent->of_node, "default-mode",
                         &default_mode);

    mutex_init(&dev->lock);
    dev->status = MOTOR_IDLE;
    dev->direction = MOTOR_DIR_CW;
    dev->mode = (int)default_mode;
    dev->interval_us = (int)default_interval;
    dev->phase_idx = 0;
    dev->position = 0;

    /* Get 4 GPIOs (the gpios property in the device tree) */
    for (i = 0; i < MOTOR_NUM_GPIOS; i++) {
        dev->gpios[i] = devm_gpiod_get_index(parent, NULL, i, GPIOD_OUT_LOW);
        if (IS_ERR(dev->gpios[i])) {
            dev_err(parent, "Failed to get motor GPIO %d\n", i);
            return PTR_ERR(dev->gpios[i]);
        }
    }

    /* 3. Initialize work queue and timer */
    INIT_WORK(&dev->stop_work, motor_stop_work_fn);
    hrtimer_init(&dev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    dev->timer.function = motor_step_cb;

    /* 4. Register char device */
    ret = alloc_chrdev_region(&dev->devid, 0, 1, MOTOR_DEV_NAME);
    if (ret) return ret;

    cdev_init(&dev->cdev, &motor_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devid, 1);
    if (ret) {
        unregister_chrdev_region(dev->devid, 1);
        return ret;
    }

    /* 5. Create device node */
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

    /* 6. Bind private data to platform_device */
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

/*
 * motor_remove — cleanup function on device removal
 *
 * Called when: module unload or device hot-removal.
 *
 * Cleanup order is reverse of probe (LIFO):
 *   device_destroy → class_destroy → cdev_del → unregister_chrdev_region
 *
 * Note: devm_* allocated resources (devm_kzalloc, devm_gpiod_get) do not need
 *       manual release; the kernel releases them in LIFO order on device removal.
 */
static int motor_remove(struct platform_device *pdev)
{
    struct motor_data *dev = platform_get_drvdata(pdev);

    if (!dev) return 0;

    /* Stop any running motor */
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

/*
 * Device tree match table
 *
 * The compatible string is the driver's "ID card", matched against the
 * compatible property in the device tree.
 * Naming convention: "<vendor>,<device-name>"
 */
static const struct of_device_id motor_match[] = {
    { .compatible = "retail,motor" },
    {}
};
MODULE_DEVICE_TABLE(of, motor_match);

/*
 * platform_driver struct — the only "entry point" that needs to be exposed to the kernel
 *
 * .probe  = called on match success
 * .remove = called on device removal
 * .driver = driver metadata (name + of_match_table)
 */
static struct platform_driver motor_driver = {
    .probe  = motor_probe,
    .remove = motor_remove,
    .driver = {
        .name           = "motor",
        .owner          = THIS_MODULE,
        .of_match_table = motor_match,
    },
};

/*
 * module_platform_driver — macro expansion
 *
 * Equivalent to:
 *   static int __init motor_init(void) {
 *       return platform_driver_register(&motor_driver);
 *   }
 *   static void __exit motor_exit(void) {
 *       platform_driver_unregister(&motor_driver);
 *   }
 *   module_init(motor_init);
 *   module_exit(motor_exit);
 */
module_platform_driver(motor_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RetailSystem");
MODULE_DESCRIPTION("28BYJ-48 stepper motor driver (platform_driver + hrtimer)");
