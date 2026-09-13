/*
 * RC522 RFID Kernel Driver — SPI (ISO14443-A)
 */

#define pr_fmt(fmt) "rc522: " fmt

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sysfs.h>
#include <linux/timer.h>
#include <linux/jiffies.h>

#define RC522_DEV_NAME  "rc522_dev"

#define CommandReg      0x01
#define ComIrqReg       0x04
#define ComIEnReg       0x02
#define ErrorReg        0x06
#define FIFODataReg     0x09
#define FIFOLevelReg    0x0A
#define ControlReg      0x0C
#define BitFramingReg   0x0D
#define ModeReg         0x11
#define TxControlReg    0x14
#define VersionReg      0x37

#define POLL_INTERVAL_MS    200   /* poll card detection every 200ms when interrupts fail */

/*
rc522@0 {
    compatible = "rc522,rfid";
    reg = <0>;
    spi-max-frequency = <1000000>;
    reset-gpios = <&gpio1 RK_PB0 GPIO_ACTIVE_HIGH>;
    irq-gpios = <&gpio1 RK_PB1 GPIO_ACTIVE_LOW>;  // RC522 IRQ pin, active on falling edge
    status = "okay";
};
*/


#define RC522_IOC_MAGIC         'R'

/* Read card UID (4 bytes) */
#define RC522_IOC_GET_UID       _IOR(RC522_IOC_MAGIC, 1, u8[4])
/* Control antenna switch, arg is int 0/1 */
#define RC522_IOC_SET_ANTENNA   _IOW(RC522_IOC_MAGIC, 2, int)
/* Read RC522 firmware version number (single byte) */
#define RC522_IOC_GET_VERSION   _IOR(RC522_IOC_MAGIC, 3, u8)

/* MIFARE sector authentication command, passes key, key type, block number */
#define RC522_IOC_AUTH          _IOW(RC522_IOC_MAGIC, 4, struct rc522_auth_info)
/* Read a specified data block, bidirectional transfer (block number + read data) */
#define RC522_IOC_READ_BLOCK    _IOWR(RC522_IOC_MAGIC, 5, struct rc522_block_rw)
/* Write a specified data block, passes block number + data to write */
#define RC522_IOC_WRITE_BLOCK   _IOW(RC522_IOC_MAGIC, 6, struct rc522_block_rw)

/**
 * rc522_auth_info - MIFARE authentication parameter struct
 * @key:    6-byte key, MIFARE default key is all 0xFF
 * @key_type: key type 0x60=KeyA, 0x61=KeyB
 * @block:  block number to authenticate (0~63)
 */
struct rc522_auth_info {
    u8  key[6];
    u8  key_type;
    u8  block;
};

/**
 * rc522_block_rw - MIFARE data block read/write struct
 * @block: block number (0~63)
 * @data:  16-byte block data, MIFARE standard block size
 */
struct rc522_block_rw {
    u8  block;
    u8  data[16];
};

struct rc522_dev_t {
    struct spi_device  *spi;        /* SPI device instance pointer */
    struct gpio_desc   *reset_gpio; /* Reset pin GPIO descriptor */
    struct gpio_desc   *irq_gpio;   /* Interrupt pin GPIO descriptor */
    int                 irq;         /* Kernel interrupt number */

    dev_t               devid;       /* Character device number (major+minor combined) */
    struct cdev         cdev;       /* Character device core struct */
    struct class       *class;      /* Device class, used to create /sys device directory */
    struct device      *cls_dev;    /* Device node instance under /dev */

    struct mutex        lock;        /* Global mutex, protects SPI operations, state reads/writes and other critical sections */
    wait_queue_head_t   card_wq;    /* Wait queue, implements blocking read waiting for card swipe */
    bool                card_present;/* Card present flag, true=card, false=no card */
    u8                  last_uid[4];/* UID of the last card read */

    struct timer_list   poll_timer; /* Periodic card detection timer */

    struct work_struct  detect_work; /* Deferred card detection work queue (process context, can sleep) */
    wait_queue_head_t   spi_wq;      /* SPI command completion wait queue */
    bool                spi_done;    /* SPI command completion flag, IRQ callback sets true and wakes waiters */
};

/* SPI address byte: bit7=1 read / 0 write; addr in bit6~bit1 */

/* rc522_write_reg — 0 success, <0 on SPI error */
static int rc522_write_reg(struct rc522_dev_t *dev, u8 reg, u8 val)
{
    u8 tx[2] = {(reg << 1) & 0x7E, val};
    int ret = spi_write(dev->spi, tx, 2);
    if (ret < 0)
        dev_err(&dev->spi->dev, "SPI write reg 0x%02X failed: %d\n", reg, ret);
    return ret;
}

/* rc522_read_reg — 0 success, <0 on SPI error */
static int rc522_read_reg(struct rc522_dev_t *dev, u8 reg, u8 *val)
{
    u8 tx = ((reg << 1) & 0x7E) | 0x80;
    u8 rx = 0;

    int ret = spi_write_then_read(dev->spi, &tx, 1, &rx, 1);
    if (ret < 0) {
        dev_err(&dev->spi->dev, "SPI read reg 0x%02X failed: %d\n", reg, ret);
        return ret;
    }

    *val = rx;
    return 0;
}

/* inline register read returning the value */
static inline u8 __rc522_read_reg(struct rc522_dev_t *dev, u8 reg)
{
    u8 val = 0;
    rc522_read_reg(dev, reg, &val);
    return val;
}

/* Antenna switch = TxControlReg bit0/bit1: 0x03=on, 0=off */
static void rc522_antenna_on(struct rc522_dev_t *dev, bool on)
{
    u8 temp = __rc522_read_reg(dev, TxControlReg);

    if (on) {
        if (!(temp & 0x03))
            rc522_write_reg(dev, TxControlReg, temp | 0x03);
    } else {
        rc522_write_reg(dev, TxControlReg, temp & ~0x03);
    }
}


/* rc522_get_uid — REQA + anticollision + select; 0=success, -1=no card/timeout */
static int rc522_get_uid(struct rc522_dev_t *dev, u8 *uid)
{
    int i;
    u8 n, len, bcc;

    rc522_write_reg(dev, CommandReg, 0x00);
    rc522_write_reg(dev, ComIrqReg, 0x7F);          // clear all interrupt flags
    rc522_write_reg(dev, FIFOLevelReg, 0x80);      // bit7 set = clear FIFO

    rc522_write_reg(dev, BitFramingReg, 0x07);      // short-frame framing (REQA)
    rc522_write_reg(dev, FIFODataReg, 0x26);       // 0x26 = REQA
    rc522_write_reg(dev, BitFramingReg, 0x87);
    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);        // 0x0C = Transceive

    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    if (!dev->spi_done ||
        (__rc522_read_reg(dev, ErrorReg) & 0x1B))
        return -1;

    rc522_write_reg(dev, FIFOLevelReg, 0x80);      // clear FIFO
    rc522_write_reg(dev, BitFramingReg, 0x00);
    rc522_write_reg(dev, FIFODataReg, 0x93);       // anticollision
    rc522_write_reg(dev, FIFODataReg, 0x20);
    rc522_write_reg(dev, BitFramingReg, 0x80);
    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);        // 0x0C = Transceive

    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    if (!dev->spi_done)
        return -1;

    len = __rc522_read_reg(dev, FIFOLevelReg);
    if (len < 4) return -1;

    for (i = 0; i < 4; i++)
        uid[i] = __rc522_read_reg(dev, FIFODataReg);

    bcc = uid[0] ^ uid[1] ^ uid[2] ^ uid[3];

    rc522_write_reg(dev, FIFOLevelReg, 0x80);      // clear FIFO
    rc522_write_reg(dev, BitFramingReg, 0x00);
    rc522_write_reg(dev, FIFODataReg, 0x93);       // select
    rc522_write_reg(dev, FIFODataReg, 0x70);
    rc522_write_reg(dev, FIFODataReg, uid[0]);
    rc522_write_reg(dev, FIFODataReg, uid[1]);
    rc522_write_reg(dev, FIFODataReg, uid[2]);
    rc522_write_reg(dev, FIFODataReg, uid[3]);
    rc522_write_reg(dev, FIFODataReg, bcc);
    rc522_write_reg(dev, BitFramingReg, 0x80);
    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);        // 0x0C = Transceive

    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    if (!dev->spi_done ||
        (__rc522_read_reg(dev, ErrorReg) & 0x1B))
        return -1;

    // discard SAK reply
    len = __rc522_read_reg(dev, FIFOLevelReg);
    if (len > 0)
        (void)__rc522_read_reg(dev, FIFODataReg);

    return 0;
}


/* rc522_mifare_auth — 0=success, -1=fail */
static int rc522_mifare_auth(struct rc522_dev_t *dev, u8 *key, u8 key_type, u8 block, u8 *uid)
{
    int i;
    u8 n;

    rc522_write_reg(dev, FIFOLevelReg, 0x80);      // clear FIFO
    rc522_write_reg(dev, BitFramingReg, 0x00);

    // 12-byte auth frame: cmd + block + 6-byte key + 4-byte UID
    rc522_write_reg(dev, FIFODataReg, key_type);
    rc522_write_reg(dev, FIFODataReg, block);
    for (i = 0; i < 6; i++)
        rc522_write_reg(dev, FIFODataReg, key[i]);
    for (i = 0; i < 4; i++)
        rc522_write_reg(dev, FIFODataReg, uid[i]);  // UID last (critical)

    rc522_write_reg(dev, BitFramingReg, 0x80);

    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);        // 0x0C = Transceive

    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    if (!dev->spi_done ||
        (__rc522_read_reg(dev, ErrorReg) & 0x1B))
        return -1;

    return 0;
}

/* rc522_mifare_read_block — 0=success (16 bytes), -1=fail */
static int rc522_mifare_read_block(struct rc522_dev_t *dev, u8 block, u8 *data)
{
    int i;
    u8 n;

    rc522_write_reg(dev, FIFOLevelReg, 0x80);      // clear FIFO
    rc522_write_reg(dev, BitFramingReg, 0x00);
    rc522_write_reg(dev, FIFODataReg, 0x30);       // 0x30 = MIFARE read block
    rc522_write_reg(dev, FIFODataReg, block);
    rc522_write_reg(dev, BitFramingReg, 0x80);

    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);        // 0x0C = Transceive

    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    if (!dev->spi_done)
        return -1;

    if (__rc522_read_reg(dev, FIFOLevelReg) != 16)
        return -1;

    for (i = 0; i < 16; i++)
        data[i] = __rc522_read_reg(dev, FIFODataReg);

    return 0;
}

/* rc522_mifare_write_block — 0=success, -1=fail */
static int rc522_mifare_write_block(struct rc522_dev_t *dev, u8 block, u8 *data)
{
    int i;
    u8 n;

    rc522_write_reg(dev, FIFOLevelReg, 0x80);    // clear FIFO
    rc522_write_reg(dev, BitFramingReg, 0x00);
    rc522_write_reg(dev, FIFODataReg, 0xA0);     // 0xA0 = MIFARE write block
    rc522_write_reg(dev, FIFODataReg, block);

    for (i = 0; i < 16; i++)
        rc522_write_reg(dev, FIFODataReg, data[i]);

    rc522_write_reg(dev, BitFramingReg, 0x80);
    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);     // 0x0C = Transceive

    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    return dev->spi_done ? 0 : -1;
}

/* card_detect — caller must hold dev->lock */
static void card_detect(struct rc522_dev_t *dev)
{
    u8 uid[4];

    if (rc522_get_uid(dev, uid) == 0) {
        memcpy(dev->last_uid, uid, 4);
        dev->card_present = true;
        dev_info(&dev->spi->dev, "Card detected: %02X%02X%02X%02X\n",
                 uid[0], uid[1], uid[2], uid[3]);
    } else {
        dev->card_present = false;
    }
}

/* IRQ handler — only signals SPI completion; must not call rc522_get_uid (ONESHOT deadlock) */
static irqreturn_t rc522_irq_handler(int irq, void *dev_id)
{
    struct rc522_dev_t *dev = (struct rc522_dev_t *)dev_id;

    dev->spi_done = true;
    wake_up(&dev->spi_wq);

    return IRQ_HANDLED;
}

/* detect_work_func — card detection in process context (can sleep) */
static void detect_work_func(struct work_struct *work)
{
    struct rc522_dev_t *dev = container_of(work, struct rc522_dev_t, detect_work);
    bool old_present = dev->card_present;

    mutex_lock(&dev->lock);
    card_detect(dev);
    mutex_unlock(&dev->lock);

    if (dev->card_present != old_present)
        wake_up_interruptible(&dev->card_wq);
}

/* poll_timer callback — schedules detect_work; softirq context (no sleeping) */
static void rc522_poll_timer_callback(struct timer_list *t)
{
    struct rc522_dev_t *dev = from_timer(dev, t, poll_timer);

    schedule_work(&dev->detect_work);
    mod_timer(&dev->poll_timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
}

/* rc522_open — turn on antenna */
static int rc522_open(struct inode *inode, struct file *filp)
{
    struct rc522_dev_t *dev = container_of(inode->i_cdev, struct rc522_dev_t, cdev);
    filp->private_data = dev;

    mutex_lock(&dev->lock);
    rc522_antenna_on(dev, true);
    mutex_unlock(&dev->lock);

    return 0;
}

/* antenna/IRQ are global; not turned off on close (multi-process) */
static int rc522_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/* rc522_read — 4-byte UID; blocking waits, non-blocking returns -EAGAIN */
static ssize_t rc522_read(struct file *filp, char __user *buf,
                          size_t count, loff_t *off)
{
    struct rc522_dev_t *dev = (struct rc522_dev_t *)filp->private_data;
    int ret;

    if (count < 4)
        return -EINVAL;

    if (filp->f_flags & O_NONBLOCK) {
        mutex_lock(&dev->lock);
        if (dev->card_present) {
            if (copy_to_user(buf, dev->last_uid, 4)) {
                mutex_unlock(&dev->lock);
                return -EFAULT;
            }
            dev->card_present = false;
            mutex_unlock(&dev->lock);
            return 4;
        }
        mutex_unlock(&dev->lock);
        return -EAGAIN;
    }

    ret = wait_event_interruptible(dev->card_wq, dev->card_present);
    if (ret)
        return ret;

    mutex_lock(&dev->lock);
    if (copy_to_user(buf, dev->last_uid, 4)) {
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }
    dev->card_present = false;
    mutex_unlock(&dev->lock);

    return 4;
}

/* rc522_ioctl — command dispatch */
static long rc522_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct rc522_dev_t *dev = (struct rc522_dev_t *)filp->private_data;
    int ret = 0;

    if (_IOC_TYPE(cmd) != RC522_IOC_MAGIC)
        return -ENOTTY;

    mutex_lock(&dev->lock);

    switch (cmd) {
    case RC522_IOC_GET_UID: {
        u8 uid[4];
        ret = rc522_get_uid(dev, uid);
        if (ret == 0) {
            if (copy_to_user((u8 __user *)arg, uid, 4))
                ret = -EFAULT;
        }
        break;
    }
    case RC522_IOC_SET_ANTENNA: {
        int on = (int)arg;
        rc522_antenna_on(dev, on ? true : false);
        break;
    }
    case RC522_IOC_GET_VERSION: {
        u8 ver = __rc522_read_reg(dev, VersionReg);
        if (copy_to_user((u8 __user *)arg, &ver, 1))
            ret = -EFAULT;
        break;
    }
    case RC522_IOC_AUTH: {
        struct rc522_auth_info auth;
        u8 uid[4];

        if (copy_from_user(&auth, (void __user *)arg, sizeof(auth))) {
            ret = -EFAULT;
            break;
        }
        // re-select to get UID
        if (rc522_get_uid(dev, uid) != 0) {
            ret = -ENODEV;
            break;
        }
        // UID must be passed into auth
        ret = rc522_mifare_auth(dev, auth.key, auth.key_type, auth.block, uid);
        break;
    }
    case RC522_IOC_READ_BLOCK: {
        struct rc522_block_rw rw;
        if (copy_from_user(&rw, (void __user *)arg, sizeof(rw))) {
            ret = -EFAULT;
            break;
        }
        // keep existing auth state
        ret = rc522_mifare_read_block(dev, rw.block, rw.data);
        if (ret == 0) {
            if (copy_to_user((void __user *)arg, &rw, sizeof(rw)))
                ret = -EFAULT;
        }
        break;
    }
    case RC522_IOC_WRITE_BLOCK: {
        struct rc522_block_rw rw;
        if (copy_from_user(&rw, (void __user *)arg, sizeof(rw))) {
            ret = -EFAULT;
            break;
        }
        ret = rc522_mifare_write_block(dev, rw.block, rw.data);
        break;
    }
    default:
        ret = -ENOTTY;
    }

    mutex_unlock(&dev->lock);
    return ret;
}

/* file operations: modern kernels use unlocked_ioctl */
static struct file_operations rc522_fops = {
    .owner          = THIS_MODULE,
    .open           = rc522_open,
    .release        = rc522_release,
    .read           = rc522_read,
    .unlocked_ioctl = rc522_ioctl,
};

/* antenna on/off */
static ssize_t antenna_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    struct rc522_dev_t *rc522 = dev_get_drvdata(dev);
    u8 val = __rc522_read_reg(rc522, TxControlReg);
    return sprintf(buf, "%s\n", (val & 0x03) ? "on" : "off");
}

/* write 0/1 to control antenna */
static ssize_t antenna_store(struct device *dev,
                              struct device_attribute *attr,
                              const char *buf, size_t count)
{
    struct rc522_dev_t *rc522 = dev_get_drvdata(dev);
    int on;

    if (kstrtoint(buf, 10, &on) != 0)
        return -EINVAL;

    mutex_lock(&rc522->lock);
    rc522_antenna_on(rc522, on ? true : false);
    mutex_unlock(&rc522->lock);

    return count;
}
static DEVICE_ATTR_RW(antenna);

/* 1=card present, 0=no card */
static ssize_t card_present_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    struct rc522_dev_t *rc522 = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", rc522->card_present ? 1 : 0);
}
static DEVICE_ATTR_RO(card_present);

/* firmware version register */
static ssize_t firmware_ver_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    struct rc522_dev_t *rc522 = dev_get_drvdata(dev);
    u8 ver = __rc522_read_reg(rc522, VersionReg);
    return sprintf(buf, "0x%02X\n", ver);
}
static DEVICE_ATTR_RO(firmware_ver);

static struct attribute *rc522_sysfs_attrs[] = {
    &dev_attr_antenna.attr,
    &dev_attr_card_present.attr,
    &dev_attr_firmware_ver.attr,
    NULL,
};

ATTRIBUTE_GROUPS(rc522_sysfs);

/* rc522_probe — init SPI/GPIO/IRQ/chrdev/timer */
static int rc522_probe(struct spi_device *spi)
{
    int ret;
    struct rc522_dev_t *rc522;
    struct device *dev = &spi->dev;

    rc522 = devm_kzalloc(dev, sizeof(*rc522), GFP_KERNEL);
    if (!rc522)
        return -ENOMEM;

    rc522->spi = spi;
    spi_set_drvdata(spi, rc522);

    mutex_init(&rc522->lock);
    init_waitqueue_head(&rc522->card_wq);
    init_waitqueue_head(&rc522->spi_wq);
    INIT_WORK(&rc522->detect_work, detect_work_func);
    rc522->card_present = false;
    rc522->spi_done = false;
    rc522->irq = -1;

    spi->mode = SPI_MODE_0;               // CPOL=0, CPHA=0
    spi->bits_per_word = 8;
    spi->max_speed_hz = 5000000;          // RC522 supports up to 10MHz; 5MHz more stable

    ret = spi_setup(spi);
    if (ret < 0)
    {
        dev_err(dev, "SPI setup failed: %d\n", ret);
        return ret;
    }

    rc522->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(rc522->reset_gpio)) {
        dev_err(dev, "Failed to get reset GPIO\n");
        return PTR_ERR(rc522->reset_gpio);
    }

    if (rc522->reset_gpio) {
        gpiod_set_value(rc522->reset_gpio, 0);
        msleep(20);                          // reset low 20ms
        gpiod_set_value(rc522->reset_gpio, 1);
        msleep(50);                          // wait for chip init
    }

    rc522_write_reg(rc522, CommandReg, 0x0F); // soft reset
    msleep(10);
    rc522_write_reg(rc522, 0x2A, 0x8D);      // RF config
    rc522_write_reg(rc522, 0x2B, 0x3E);      // RX gain
    rc522_write_reg(rc522, 0x2D, 30);        // timer timeout
    rc522_write_reg(rc522, ModeReg, 0x3D);   // communication mode
    rc522_antenna_on(rc522, true);

    ret = alloc_chrdev_region(&rc522->devid, 0, 1, RC522_DEV_NAME);
    if (ret) {
        dev_err(dev, "alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&rc522->cdev, &rc522_fops);
    ret = cdev_add(&rc522->cdev, rc522->devid, 1);
    if (ret) {
        unregister_chrdev_region(rc522->devid, 1);
        return ret;
    }

    rc522->class = class_create(THIS_MODULE, RC522_DEV_NAME);
    if (IS_ERR(rc522->class)) {
        cdev_del(&rc522->cdev);
        unregister_chrdev_region(rc522->devid, 1);
        return PTR_ERR(rc522->class);
    }

    rc522->cls_dev = device_create_with_groups(rc522->class, dev,
                              rc522->devid, rc522,
                              rc522_sysfs_groups, RC522_DEV_NAME);
    if (IS_ERR(rc522->cls_dev)) {
        class_destroy(rc522->class);
        cdev_del(&rc522->cdev);
        unregister_chrdev_region(rc522->devid, 1);
        return PTR_ERR(rc522->cls_dev);
    }

    rc522->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
    if (IS_ERR(rc522->irq_gpio)) {
        dev_err(dev, "Failed to get IRQ GPIO: %ld\n", PTR_ERR(rc522->irq_gpio));
        device_destroy(rc522->class, rc522->devid);
        class_destroy(rc522->class);
        cdev_del(&rc522->cdev);
        unregister_chrdev_region(rc522->devid, 1);
        return PTR_ERR(rc522->irq_gpio);
    }

    rc522->irq = gpiod_to_irq(rc522->irq_gpio);
    if (rc522->irq < 0) {
        dev_err(dev, "Failed to get IRQ number: %d\n", rc522->irq);
        device_destroy(rc522->class, rc522->devid);
        class_destroy(rc522->class);
        cdev_del(&rc522->cdev);
        unregister_chrdev_region(rc522->devid, 1);
        return rc522->irq;
    }

    ret = request_irq(rc522->irq, rc522_irq_handler,   // falling-edge, IRQ active-low
                      IRQF_TRIGGER_FALLING,
                      "rc522", rc522);
    if (ret) {
        dev_err(dev, "Failed to request IRQ %d: %d\n", rc522->irq, ret);
        device_destroy(rc522->class, rc522->devid);
        class_destroy(rc522->class);
        cdev_del(&rc522->cdev);
        unregister_chrdev_region(rc522->devid, 1);
        return ret;
    }

    // 0x71 = TimerIRq | IdleIRq | TxIRq
    rc522_write_reg(rc522, ComIEnReg, 0x71);
    dev_info(dev, "IRQ registered (IRQ=%d), SPI commands interrupt-driven\n", rc522->irq);

    // RC522 does not self-detect; poll REQA periodically
    timer_setup(&rc522->poll_timer, rc522_poll_timer_callback, 0);
    mod_timer(&rc522->poll_timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
    dev_info(dev, "poll_timer started (%dms interval)\n", POLL_INTERVAL_MS);

    dev_info(dev, "RC522 driver loaded successfully\n");
    return 0;
}

/* rc522_remove — reverse-order resource release */
static int rc522_remove(struct spi_device *spi)
{
    struct rc522_dev_t *rc522 = spi_get_drvdata(spi);

    rc522_write_reg(rc522, ComIEnReg, 0x00);
    free_irq(rc522->irq, rc522);

    del_timer_sync(&rc522->poll_timer);
    cancel_work_sync(&rc522->detect_work);

    device_destroy(rc522->class, rc522->devid);
    class_destroy(rc522->class);
    cdev_del(&rc522->cdev);
    unregister_chrdev_region(rc522->devid, 1);

    dev_info(&spi->dev, "RC522 driver removed\n");
    return 0;
}


static const struct of_device_id rc522_match[] = {
    { .compatible = "nxp,rc522" },
    { .compatible = "rc522,rfid" },
    {}
};
MODULE_DEVICE_TABLE(of, rc522_match);

static struct spi_driver rc522_driver = {
    .probe    = rc522_probe,
    .remove   = rc522_remove,
    .driver   = {
        .name           = "rc522",
        .of_match_table = rc522_match,
    },
};

module_spi_driver(rc522_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RetailSystem");
MODULE_DESCRIPTION("RC522 RFID Driver with IRQ/ioctl/sysfs");
