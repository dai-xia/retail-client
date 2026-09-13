/*
 * DMA SPI Transfer Driver — SPI + DMA Engine
 */

#define pr_fmt(fmt) "dma_spi: " fmt

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/gpio/consumer.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/delay.h>

#define DMA_SPI_DEV_NAME    "dma_spi"
#define DMA_SPI_BUF_SIZE    65536   /* 64KB DMA buffer */
#define DMA_SPI_MAX_TRANSFER 32768  /* Max 32KB per transfer */

#define DMA_SPI_IOC_MAGIC       'D'
#define DMA_SPI_IOC_GET_STATS   _IOR(DMA_SPI_IOC_MAGIC, 1, struct dma_spi_stats)
#define DMA_SPI_IOC_RESET_STATS _IO(DMA_SPI_IOC_MAGIC, 2)
#define DMA_SPI_IOC_SET_SPEED   _IOW(DMA_SPI_IOC_MAGIC, 3, u32)
#define DMA_SPI_IOC_GET_SPEED   _IOR(DMA_SPI_IOC_MAGIC, 4, u32)

struct dma_spi_stats {
	u32 transfer_count;      /* Total number of transfers */
	u32 bytes_transferred;   /* Total bytes transferred */
	u32 dma_errors;          /* Number of DMA errors */
	u32 avg_transfer_us;     /* Average transfer time (μs) */
};

struct dma_spi_data {
	struct spi_device   *spi;

	/* DMA channels */
	struct dma_chan     *tx_chan;
	struct dma_chan     *rx_chan;

	/* DMA buffers (coherent DMA mapping) */
	void                *tx_buf;
	dma_addr_t           tx_dma;
	void                *rx_buf;
	dma_addr_t           rx_dma;

	/* Transfer completion signal */
	struct completion    tx_done;
	struct completion    rx_done;

	/* Character device */
	dev_t                devid;
	struct cdev          cdev;
	struct class        *class;
	struct device       *cls_dev;

	/* Mutex */
	struct mutex         lock;

	/* Transfer statistics */
	struct dma_spi_stats stats;

	/* SPI configuration */
	u32                  max_speed_hz;
};

/* DMA completion callback — interrupt context */
static void dma_spi_tx_callback(void *param)
{
	struct completion *done = param;
	complete(done);
}

static void dma_spi_rx_callback(void *param)
{
	struct completion *done = param;
	complete(done);
}

static ssize_t dma_spi_transfer(struct dma_spi_data *dev,
				const u8 *tx_data, u8 *rx_data,
				size_t len)
{
	struct dma_async_tx_descriptor *tx_desc = NULL;
	struct dma_async_tx_descriptor *rx_desc = NULL;
	struct spi_transfer t;
	struct spi_message  m;
	ssize_t ret = 0;
	ktime_t start, end;

	if (len > DMA_SPI_BUF_SIZE)
		len = DMA_SPI_BUF_SIZE;
	if (len == 0)
		return 0;

	start = ktime_get();

	if (tx_data)
		memcpy(dev->tx_buf, tx_data, len);
	else
		memset(dev->tx_buf, 0, len);

	reinit_completion(&dev->tx_done);

	tx_desc = dmaengine_prep_slave_single(dev->tx_chan,
						       dev->tx_dma, len,
						       DMA_MEM_TO_DEV,
						       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!tx_desc) {
		dev_err(&dev->spi->dev, "TX DMA prep failed\n");
		ret = -ENOMEM;
		goto err_unmap_tx;
	}

	tx_desc->callback = dma_spi_tx_callback;
	tx_desc->callback_param = &dev->tx_done;

	dmaengine_submit(tx_desc);
	dma_async_issue_pending(dev->tx_chan);

	if (rx_data) {
		reinit_completion(&dev->rx_done);

		rx_desc = dmaengine_prep_slave_single(dev->rx_chan,
							       dev->rx_dma, len,
							       DMA_DEV_TO_MEM,
							       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
		if (!rx_desc) {
			dev_err(&dev->spi->dev, "RX DMA prep failed\n");
			ret = -ENOMEM;
			goto err_unmap_tx;
		}

		rx_desc->callback = dma_spi_rx_callback;
		rx_desc->callback_param = &dev->rx_done;
		dmaengine_submit(rx_desc);
		dma_async_issue_pending(dev->rx_chan);
	}

	memset(&t, 0, sizeof(t));
	t.tx_buf = dev->tx_buf;
	t.rx_buf = rx_data ? dev->rx_buf : NULL;
	t.len    = len;
	t.speed_hz = dev->max_speed_hz;
	t.bits_per_word = 8;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	ret = spi_sync(dev->spi, &m);
	if (ret < 0) {
		dev_err(&dev->spi->dev, "SPI sync failed: %zd\n", ret);
		goto err_unmap_tx;
	}

	if (tx_data) {
		if (!wait_for_completion_timeout(&dev->tx_done,
						 msecs_to_jiffies(5000))) {
			dev_err(&dev->spi->dev, "TX DMA timeout\n");
			dmaengine_terminate_sync(dev->tx_chan);
			ret = -ETIMEDOUT;
			goto err_unmap_tx;
		}
	}

	if (rx_data) {
		if (!wait_for_completion_timeout(&dev->rx_done,
						 msecs_to_jiffies(5000))) {
			dev_err(&dev->spi->dev, "RX DMA timeout\n");
			dmaengine_terminate_sync(dev->rx_chan);
			ret = -ETIMEDOUT;
			goto err_unmap_tx;
		}
	}

	dma_unmap_single(&dev->spi->dev, dev->tx_dma, len, DMA_TO_DEVICE);

	if (rx_data)
		memcpy(rx_data, dev->rx_buf, len);

	end = ktime_get();
	dev->stats.transfer_count++;
	dev->stats.bytes_transferred += len;
	dev->stats.avg_transfer_us = (dev->stats.avg_transfer_us * 7 +
		(u32)ktime_us_delta(end, start)) / 8;  /* Exponential weighted average */

	return (ssize_t)len;

	err_unmap_tx:
	dma_unmap_single(&dev->spi->dev, dev->tx_dma, len, DMA_TO_DEVICE);
	dev->stats.dma_errors++;
	return ret;
}

static struct dma_spi_data *dma_spi_from_file(struct file *filp)
{
	return (struct dma_spi_data *)filp->private_data;
}

static int dma_spi_open(struct inode *inode, struct file *filp)
{
	struct dma_spi_data *dev = container_of(inode->i_cdev,
						struct dma_spi_data, cdev);
	filp->private_data = dev;
	dev_info(&dev->spi->dev, "DMA SPI device opened\n");
	return 0;
}

/* write(): send data to SPI peripheral via DMA */
static ssize_t dma_spi_write(struct file *filp, const char __user *buf,
			     size_t count, loff_t *off)
{
	struct dma_spi_data *dev = dma_spi_from_file(filp);
	u8 *kbuf;
	ssize_t ret;

	if (!dev)
		return -ENODEV;
	if (count > DMA_SPI_MAX_TRANSFER)
		count = DMA_SPI_MAX_TRANSFER;

	kbuf = kmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	if (copy_from_user(kbuf, buf, count)) {
		kfree(kbuf);
		return -EFAULT;
	}

	mutex_lock(&dev->lock);
	ret = dma_spi_transfer(dev, kbuf, NULL, count);
	mutex_unlock(&dev->lock);

	kfree(kbuf);
	return ret;
}

/* read(): send dummy bytes to clock out MISO data */
static ssize_t dma_spi_read(struct file *filp, char __user *buf,
			    size_t count, loff_t *off)
{
	struct dma_spi_data *dev = dma_spi_from_file(filp);
	u8 *kbuf;
	ssize_t ret;

	if (!dev)
		return -ENODEV;
	if (count > DMA_SPI_MAX_TRANSFER)
		count = DMA_SPI_MAX_TRANSFER;

	kbuf = kmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	mutex_lock(&dev->lock);
	ret = dma_spi_transfer(dev, NULL, kbuf, count);
	mutex_unlock(&dev->lock);

	if (ret > 0) {
		if (copy_to_user(buf, kbuf, ret)) {
			kfree(kbuf);
			return -EFAULT;
		}
	}

	kfree(kbuf);
	return ret;
}

static long dma_spi_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	struct dma_spi_data *dev = dma_spi_from_file(filp);
	u32 speed;

	if (!dev)
		return -ENODEV;
	if (_IOC_TYPE(cmd) != DMA_SPI_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case DMA_SPI_IOC_GET_STATS:
		if (copy_to_user((void __user *)arg, &dev->stats,
				 sizeof(dev->stats)))
			return -EFAULT;
		return 0;

	case DMA_SPI_IOC_RESET_STATS:
		mutex_lock(&dev->lock);
		memset(&dev->stats, 0, sizeof(dev->stats));
		mutex_unlock(&dev->lock);
		return 0;

	case DMA_SPI_IOC_SET_SPEED:
		if (copy_from_user(&speed, (u32 __user *)arg, sizeof(speed)))
			return -EFAULT;
		if (speed == 0 || speed > 50000000)
			return -EINVAL;
		dev->max_speed_hz = speed;
		return 0;

	case DMA_SPI_IOC_GET_SPEED:
		speed = dev->max_speed_hz;
		return put_user(speed, (u32 __user *)arg);

	default:
		return -ENOTTY;
	}
}

static struct file_operations dma_spi_fops = {
	.owner          = THIS_MODULE,
	.open           = dma_spi_open,
	.read           = dma_spi_read,
	.write          = dma_spi_write,
	.unlocked_ioctl = dma_spi_ioctl,
};

static inline struct dma_spi_data *to_dma_spi(struct device *dev)
{
	return dev_get_drvdata(dev);
}

static ssize_t transfers_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct dma_spi_data *d = to_dma_spi(dev);
	return sprintf(buf, "%u\n", d->stats.transfer_count);
}
static DEVICE_ATTR_RO(transfers);

static ssize_t bytes_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct dma_spi_data *d = to_dma_spi(dev);
	return sprintf(buf, "%u\n", d->stats.bytes_transferred);
}
static DEVICE_ATTR_RO(bytes);

static ssize_t errors_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct dma_spi_data *d = to_dma_spi(dev);
	return sprintf(buf, "%u\n", d->stats.dma_errors);
}
static DEVICE_ATTR_RO(errors);

static ssize_t avg_time_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct dma_spi_data *d = to_dma_spi(dev);
	return sprintf(buf, "%u\n", d->stats.avg_transfer_us);
}
static DEVICE_ATTR_RO(avg_time);

static ssize_t speed_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct dma_spi_data *d = to_dma_spi(dev);
	return sprintf(buf, "%u\n", d->max_speed_hz);
}

static ssize_t speed_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct dma_spi_data *d = to_dma_spi(dev);
	u32 val;

	if (kstrtou32(buf, 10, &val) != 0)
		return -EINVAL;
	if (val == 0 || val > 50000000)
		return -EINVAL;

	mutex_lock(&d->lock);
	d->max_speed_hz = val;
	mutex_unlock(&d->lock);

	return count;
}
static DEVICE_ATTR_RW(speed);

static struct attribute *dma_spi_sysfs_attrs[] = {
	&dev_attr_transfers.attr,
	&dev_attr_bytes.attr,
	&dev_attr_errors.attr,
	&dev_attr_avg_time.attr,
	&dev_attr_speed.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dma_spi_sysfs);

static int dma_spi_probe(struct spi_device *spi)
{
	int ret;
	struct dma_spi_data *dev;
	struct device *parent = &spi->dev;

	dev = devm_kzalloc(parent, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->spi = spi;
	dev->max_speed_hz = spi->max_speed_hz;
	spi_set_drvdata(spi, dev);

	mutex_init(&dev->lock);
	init_completion(&dev->tx_done);
	init_completion(&dev->rx_done);

	dev->tx_chan = dma_request_chan(parent, "tx");
	if (IS_ERR(dev->tx_chan)) {
		ret = PTR_ERR(dev->tx_chan);
		if (ret != -EPROBE_DEFER)
			dev_err(parent, "Failed to get TX DMA channel: %d\n", ret);
		return ret;
	}

	dev->rx_chan = dma_request_chan(parent, "rx");
	if (IS_ERR(dev->rx_chan)) {
		ret = PTR_ERR(dev->rx_chan);
		if (ret != -EPROBE_DEFER)
			dev_err(parent, "Failed to get RX DMA channel: %d\n", ret);
		goto err_free_tx;
	}

	dev->tx_buf = dma_alloc_coherent(parent, DMA_SPI_BUF_SIZE,
					  &dev->tx_dma, GFP_KERNEL);
	if (!dev->tx_buf) {
		dev_err(parent, "Failed to allocate TX DMA buffer\n");
		ret = -ENOMEM;
		goto err_free_rx;
	}

	dev->rx_buf = dma_alloc_coherent(parent, DMA_SPI_BUF_SIZE,
					  &dev->rx_dma, GFP_KERNEL);
	if (!dev->rx_buf) {
		dev_err(parent, "Failed to allocate RX DMA buffer\n");
		ret = -ENOMEM;
		goto err_free_tx_buf;
	}

	ret = alloc_chrdev_region(&dev->devid, 0, 1, DMA_SPI_DEV_NAME);
	if (ret) {
		dev_err(parent, "alloc_chrdev_region failed: %d\n", ret);
		goto err_free_rx_buf;
	}

	cdev_init(&dev->cdev, &dma_spi_fops);
	dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dev->cdev, dev->devid, 1);
	if (ret) {
		dev_err(parent, "cdev_add failed: %d\n", ret);
		goto err_unreg_region;
	}

	dev->class = class_create(THIS_MODULE, DMA_SPI_DEV_NAME);
	if (IS_ERR(dev->class)) {
		ret = PTR_ERR(dev->class);
		goto err_cdev_del;
	}

	dev->cls_dev = device_create_with_groups(dev->class, parent,
				  dev->devid, dev,
				  dma_spi_sysfs_groups, DMA_SPI_DEV_NAME);
	if (IS_ERR(dev->cls_dev)) {
		ret = PTR_ERR(dev->cls_dev);
		goto err_class_destroy;
	}

	dev_info(parent, "DMA SPI driver loaded (tx=%s, rx=%s, buf=%dKB, speed=%uHz)\n",
		 dma_chan_name(dev->tx_chan),
		 dma_chan_name(dev->rx_chan),
		 DMA_SPI_BUF_SIZE / 1024,
		 dev->max_speed_hz);
	return 0;

err_class_destroy:
	class_destroy(dev->class);
err_cdev_del:
	cdev_del(&dev->cdev);
err_unreg_region:
	unregister_chrdev_region(dev->devid, 1);
err_free_rx_buf:
	dma_free_coherent(parent, DMA_SPI_BUF_SIZE, dev->rx_buf, dev->rx_dma);
err_free_tx_buf:
	dma_free_coherent(parent, DMA_SPI_BUF_SIZE, dev->tx_buf, dev->tx_dma);
err_free_rx:
	dma_release_channel(dev->rx_chan);
err_free_tx:
	dma_release_channel(dev->tx_chan);
	return ret;
}

static int dma_spi_remove(struct spi_device *spi)
{
	struct dma_spi_data *dev = spi_get_drvdata(spi);

	if (!dev)
		return 0;

	if (dev->tx_chan)
		dmaengine_terminate_sync(dev->tx_chan);
	if (dev->rx_chan)
		dmaengine_terminate_sync(dev->rx_chan);

	device_destroy(dev->class, dev->devid);
	class_destroy(dev->class);
	cdev_del(&dev->cdev);
	unregister_chrdev_region(dev->devid, 1);

	dma_free_coherent(&spi->dev, DMA_SPI_BUF_SIZE, dev->rx_buf, dev->rx_dma);
	dma_free_coherent(&spi->dev, DMA_SPI_BUF_SIZE, dev->tx_buf, dev->tx_dma);

	if (dev->rx_chan)
		dma_release_channel(dev->rx_chan);
	if (dev->tx_chan)
		dma_release_channel(dev->tx_chan);

	dev_info(&spi->dev, "DMA SPI driver removed\n");
	return 0;
}

static const struct of_device_id dma_spi_of_match[] = {
	{ .compatible = "retail,dma-spi" },
	{}
};
MODULE_DEVICE_TABLE(of, dma_spi_of_match);

static const struct spi_device_id dma_spi_id[] = {
	{ "dma_spi", 0 },
	{}
};
MODULE_DEVICE_TABLE(spi, dma_spi_id);

static struct spi_driver dma_spi_driver = {
	.driver = {
		.name           = "dma_spi",
		.of_match_table = dma_spi_of_match,
	},
	.probe    = dma_spi_probe,
	.remove   = dma_spi_remove,
	.id_table = dma_spi_id,
};

module_spi_driver(dma_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RetailSystem");
MODULE_DESCRIPTION("DMA SPI Transfer Driver (DMA Engine API Demo)");
