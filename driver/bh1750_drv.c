/*
 * BH1750 Light Sensor IIO Driver — I2C (addr 0x23)
 */

#define pr_fmt(fmt) "bh1750_iio: " fmt

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/of.h>

#define BH1750_DEV_NAME "bh1750"

/* BH1750 commands */
#define BH1750_POWER_ON      0x01
#define BH1750_POWER_DOWN    0x00
#define BH1750_RESET         0x07

#define BH1750_MODE_H_RES    0x10  /* Continuous high-resolution mode, 1lx, 120ms */
#define BH1750_MODE_H_RES2   0x11  /* Continuous high-resolution mode 2, 0.5lx, 120ms */
#define BH1750_MODE_L_RES    0x13  /* Continuous low-resolution mode, 4lx, 16ms */
#define BH1750_MODE_ONCE_H   0x20  /* One-shot high-resolution mode */
#define BH1750_MODE_ONCE_H2  0x21  /* One-shot high-resolution mode 2 */
#define BH1750_MODE_ONCE_L   0x23  /* One-shot low-resolution mode */

#define BH1750_DEFAULT_MTREG 69
#define BH1750_CMD_MTREG_H   0x40
#define BH1750_CMD_MTREG_L   0x60

/* Measurement wait time (ms) */
#define BH1750_H_RES_MS      180  /* High resolution needs 120-180ms */
#define BH1750_L_RES_MS      24   /* Low resolution needs 16-24ms */

#define BH1750_IOC_MAGIC        'B'
#define BH1750_IOC_SET_MODE     _IOW(BH1750_IOC_MAGIC, 1, u8)
#define BH1750_IOC_SET_MTREG    _IOW(BH1750_IOC_MAGIC, 2, u8)
#define BH1750_IOC_GET_LUX      _IOR(BH1750_IOC_MAGIC, 3, int)

struct bh1750_data {
    struct i2c_client *client;

    /* IIO */
    struct iio_dev *indio_dev;

    struct mutex    lock;
    u8              current_mode;
    u8              mtreg;

    /* Character device (backward compatible with legacy /dev/bh1750) */
    dev_t           devid;
    struct cdev     cdev;
    struct class   *class;
    struct device  *cls_dev;
};

static int bh1750_write_cmd(struct bh1750_data *data, u8 cmd)
{
    int ret = i2c_master_send(data->client, &cmd, 1);
    if (ret < 0) {
        dev_err(&data->client->dev,
                "i2c_master_send cmd 0x%02X failed: %d\n", cmd, ret);
        return ret;
    }
    if (ret != 1) {
        dev_err(&data->client->dev,
                "i2c_master_send cmd 0x%02X short write: %d\n", cmd, ret);
        return -EIO;
    }
    return 0;
}

static void bh1750_set_mtreg(struct bh1750_data *data, u8 mtreg)
{
    bh1750_write_cmd(data, BH1750_CMD_MTREG_H | (mtreg >> 5));
    bh1750_write_cmd(data, BH1750_CMD_MTREG_L | (mtreg & 0x1F));
    data->mtreg = mtreg;
}

/* Read lux: raw = (buf[0]<<8)|buf[1]; lux = raw/1.2*(MTreg/69); one-shot re-triggers */
static int bh1750_read_raw_lux(struct bh1750_data *data, int *lux)
{
    u8 buf[2];
    int ret;
    bool is_once = (data->current_mode >= BH1750_MODE_ONCE_H);

    /* One-shot mode: re-trigger measurement */
    if (is_once) {
        ret = bh1750_write_cmd(data, data->current_mode);
        if (ret < 0) return ret;
        msleep(BH1750_H_RES_MS);
    }

    ret = i2c_master_recv(data->client, buf, 2);
    if (ret < 0)
        return ret;
    if (ret != 2)
        return -EIO;

    *lux = ((buf[0] << 8) | buf[1]) * 10 / 12 * data->mtreg / 69;
    return 0;
}

static const struct iio_chan_spec bh1750_channels[] = {
    {
        .type = IIO_LIGHT,
        .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
        .info_mask_shared_by_all = BIT(IIO_CHAN_INFO_INT_TIME),
    },
};

/* read_raw — IIO core read callback */
static int bh1750_read_raw(struct iio_dev *indio_dev,
                           struct iio_chan_spec const *chan,
                           int *val, int *val2, long mask)
{
    struct bh1750_data *data = iio_priv(indio_dev);
    int ret;

    switch (mask) {
    case IIO_CHAN_INFO_PROCESSED:
        mutex_lock(&data->lock);
        ret = bh1750_read_raw_lux(data, val);
        mutex_unlock(&data->lock);
        if (ret < 0) return ret;
        return IIO_VAL_INT;

    case IIO_CHAN_INFO_INT_TIME:
        /* integration time = MTreg * 1.85ms, in μs */
        *val = 0;
        *val2 = data->mtreg * 1850;  /* μs */
        return IIO_VAL_INT_PLUS_MICRO;

    default:
        return -EINVAL;
    }
}

/* write_raw — adjust integration time via MTreg (31~254) */
static int bh1750_write_raw(struct iio_dev *indio_dev,
                            struct iio_chan_spec const *chan,
                            int val, int val2, long mask)
{
    struct bh1750_data *data = iio_priv(indio_dev);
    u8 mtreg;

    if (mask != IIO_CHAN_INFO_INT_TIME)
        return -EINVAL;

    mtreg = (u8)((val * 1000000 + val2) / 1850);
    if (mtreg < 31 || mtreg > 254)
        return -EINVAL;

    mutex_lock(&data->lock);
    bh1750_set_mtreg(data, mtreg);
    bh1750_write_cmd(data, data->current_mode);
    if ((data->current_mode & 0xF0) == 0x10)  /* Continuous mode */
        msleep(BH1750_H_RES_MS);
    mutex_unlock(&data->lock);

    return 0;
}

static const struct iio_info bh1750_info = {
    .read_raw  = bh1750_read_raw,
    .write_raw = bh1750_write_raw,
};

static struct bh1750_data *bh1750_from_file(struct file *filp)
{
    return (struct bh1750_data *)filp->private_data;
}

static int bh1750_chr_open(struct inode *inode, struct file *filp)
{
    struct bh1750_data *data = container_of(inode->i_cdev,
                                             struct bh1750_data, cdev);
    int ret;

    filp->private_data = data;

    mutex_lock(&data->lock);
    ret = bh1750_write_cmd(data, BH1750_POWER_ON);
    if (ret < 0) { mutex_unlock(&data->lock); return ret; }
    bh1750_set_mtreg(data, data->mtreg);
    ret = bh1750_write_cmd(data, data->current_mode);
    if (ret < 0) { mutex_unlock(&data->lock); return ret; }
    if ((data->current_mode & 0xF0) == 0x10)  /* Continuous mode */
        msleep(BH1750_H_RES_MS);
    mutex_unlock(&data->lock);

    return 0;
}

static ssize_t bh1750_chr_read(struct file *filp, char __user *buf,
                                size_t size, loff_t *offset)
{
    struct bh1750_data *data = bh1750_from_file(filp);
    int lux, ret;

    if (!data) return -ENODEV;
    if (size < sizeof(int)) return -EINVAL;

    mutex_lock(&data->lock);
    ret = bh1750_read_raw_lux(data, &lux);
    mutex_unlock(&data->lock);

    if (ret < 0) return ret;

    if (copy_to_user(buf, &lux, sizeof(lux)))
        return -EFAULT;

    return sizeof(lux);
}

static long bh1750_chr_ioctl(struct file *filp, unsigned int cmd,
                              unsigned long arg)
{
    struct bh1750_data *data = bh1750_from_file(filp);
    int ret = 0;

    if (!data) return -ENODEV;
    if (_IOC_TYPE(cmd) != BH1750_IOC_MAGIC)
        return -ENOTTY;

    mutex_lock(&data->lock);

    switch (cmd) {
    case BH1750_IOC_SET_MODE: {
        u8 mode = (u8)arg;
        switch (mode) {
        case BH1750_MODE_H_RES:
        case BH1750_MODE_H_RES2:
        case BH1750_MODE_L_RES:
        case BH1750_MODE_ONCE_H:
        case BH1750_MODE_ONCE_H2:
        case BH1750_MODE_ONCE_L:
            ret = bh1750_write_cmd(data, mode);
            if (ret < 0) break;
            data->current_mode = mode;
            if ((mode & 0xF0) == 0x10)
                msleep(BH1750_H_RES_MS);
            break;
        default:
            ret = -EINVAL;
        }
        break;
    }
    case BH1750_IOC_SET_MTREG: {
        u8 mtreg = (u8)arg;
        if (mtreg < 31 || mtreg > 254) { ret = -EINVAL; break; }
        bh1750_set_mtreg(data, mtreg);
        ret = bh1750_write_cmd(data, data->current_mode);
        if (ret == 0 && (data->current_mode & 0xF0) == 0x10)
            msleep(BH1750_H_RES_MS);
        break;
    }
    case BH1750_IOC_GET_LUX: {
        int lux;
        ret = bh1750_read_raw_lux(data, &lux);
        if (ret < 0) break;
        if (copy_to_user((int __user *)arg, &lux, sizeof(lux)))
            ret = -EFAULT;
        break;
    }
    default:
        ret = -ENOTTY;
    }

    mutex_unlock(&data->lock);
    return ret;
}

static struct file_operations bh1750_fops = {
    .owner          = THIS_MODULE,
    .open           = bh1750_chr_open,
    .read           = bh1750_chr_read,
    .unlocked_ioctl = bh1750_chr_ioctl,
};

static int bh1750_probe(struct i2c_client *client,
                        const struct i2c_device_id *id)
{
    struct bh1750_data *data;
    struct iio_dev *indio_dev;
    struct device *dev = &client->dev;
    int ret;
    u32 default_mtreg = BH1750_DEFAULT_MTREG;

    indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
    if (!indio_dev)
        return -ENOMEM;

    data = iio_priv(indio_dev);
    data->client = client;
    data->indio_dev = indio_dev;
    data->current_mode = BH1750_MODE_H_RES;
    data->mtreg = BH1750_DEFAULT_MTREG;
    mutex_init(&data->lock);

    of_property_read_u32(dev->of_node, "default-mtreg", &default_mtreg);
    data->mtreg = (u8)default_mtreg;

    i2c_set_clientdata(client, data);

    indio_dev->name = "bh1750";
    indio_dev->info = &bh1750_info;
    indio_dev->channels = bh1750_channels;
    indio_dev->num_channels = ARRAY_SIZE(bh1750_channels);
    indio_dev->modes = INDIO_DIRECT_MODE;

    ret = devm_iio_device_register(dev, indio_dev);
    if (ret) {
        dev_err(dev, "Failed to register IIO device: %d\n", ret);
        return ret;
    }

    bh1750_write_cmd(data, BH1750_POWER_ON);
    bh1750_set_mtreg(data, data->mtreg);
    bh1750_write_cmd(data, BH1750_MODE_H_RES);

    ret = alloc_chrdev_region(&data->devid, 0, 1, BH1750_DEV_NAME);
    if (ret) return ret;

    cdev_init(&data->cdev, &bh1750_fops);
    data->cdev.owner = THIS_MODULE;
    ret = cdev_add(&data->cdev, data->devid, 1);
    if (ret) {
        unregister_chrdev_region(data->devid, 1);
        return ret;
    }

    data->class = class_create(THIS_MODULE, BH1750_DEV_NAME);
    if (IS_ERR(data->class)) {
        ret = PTR_ERR(data->class);
        goto err_cdev;
    }

    data->cls_dev = device_create(data->class, dev,
                                  data->devid, data, BH1750_DEV_NAME);
    if (IS_ERR(data->cls_dev)) {
        ret = PTR_ERR(data->cls_dev);
        goto err_class;
    }

    dev_info(dev, "BH1750 IIO driver loaded @ 0x%02X (MTreg=%d)\n",
             client->addr, data->mtreg);
    return 0;

err_class:
    class_destroy(data->class);
err_cdev:
    cdev_del(&data->cdev);
    unregister_chrdev_region(data->devid, 1);
    return ret;
}

static int bh1750_remove(struct i2c_client *client)
{
    struct bh1750_data *data = i2c_get_clientdata(client);

    bh1750_write_cmd(data, BH1750_POWER_DOWN);

    device_destroy(data->class, data->devid);
    class_destroy(data->class);
    cdev_del(&data->cdev);
    unregister_chrdev_region(data->devid, 1);

    dev_info(&client->dev, "BH1750 IIO driver removed\n");
    return 0;
}

static const struct of_device_id bh1750_of_match[] = {
    { .compatible = "rohm,bh1750" },
    { .compatible = "bh1750,light" },  /* keep legacy compatibility */
    {}
};
MODULE_DEVICE_TABLE(of, bh1750_of_match);

static const struct i2c_device_id bh1750_id[] = {
    { "bh1750", 0 },
    {}
};
MODULE_DEVICE_TABLE(i2c, bh1750_id);

static struct i2c_driver bh1750_driver = {
    .driver = {
        .name           = "bh1750",
        .of_match_table = bh1750_of_match,
    },
    .probe    = bh1750_probe,
    .remove   = bh1750_remove,
    .id_table = bh1750_id,
};

module_i2c_driver(bh1750_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RetailSystem");
MODULE_DESCRIPTION("BH1750 Light Sensor IIO Driver");
