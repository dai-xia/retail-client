/*
 * DMA SPI Transfer Driver — In-depth DMA Transfer Example
 *
 * Platform: RK3568
 * Bus: SPI + DMA
 * Framework: platform_driver + DMA Engine API
 *
 * Features:
 *   - Uses DMA for large-block SPI transfers (CPU zero-copy)
 *   - Supports independent TX/RX DMA channels
 *   - Character device /dev/dma_spi provides read/write/ioctl interface
 *   - sysfs attributes expose DMA status
 *   - Complete DMA transfer flow demonstration
 *
 * Hardware wiring (SPI peripheral → RK3568):
 *   MOSI → SPI0_MOSI
 *   MISO → SPI0_MISO
 *   SCK  → SPI0_CLK
 *   CS   → GPIO (chip select)
 *
 * Device tree:
 *   &spi0 {
 *       dma_spi@0 {
 *           compatible = "retail,dma-spi";
 *           reg = <0>;
 *           spi-max-frequency = <10000000>;
 *           dmas = <&dmac 0>, <&dmac 1>;
 *           dma-names = "tx", "rx";
 *       };
 *   };
 *
 * ==================== Interview Knowledge Points ====================
 *
 * 1. DMA (Direct Memory Access)
 *
 *    DMA is a mechanism that allows peripherals to directly access system
 *    memory without CPU involvement for each data transfer. The CPU only
 *    needs to configure the DMA controller, then continue executing other
 *    tasks; when the DMA transfer completes, the CPU is notified via interrupt.
 *
 *    Advantages:
 *      - CPU zero-copy: data goes directly from peripheral to memory, bypassing CPU
 *      - Bulk transfer: suitable for large data blocks (KB~MB level)
 *      - Reduced CPU load: CPU can process other tasks in parallel
 *
 *    Disadvantages:
 *      - Setup overhead: configuring DMA descriptors has some latency
 *      - Not worthwhile for small data: PIO is faster below ~64 bytes
 *      - Cache coherency issues: need dma_map_single to handle
 *
 * 2. Linux DMA Engine API
 *
 *    The Linux kernel provides a unified DMA engine framework that abstracts
 *    out the DMA controller differences across different SoCs.
 *
 *    Core API call flow:
 *
 *      Step 1: Request a DMA channel
 *        dma_request_chan(dev, name) → struct dma_chan *
 *
 *      Step 2: Allocate DMA buffer
 *        dma_alloc_coherent(dev, size, &dma_handle, GFP_KERNEL)
 *        → void *cpu_addr, dma_addr_t dma_handle
 *
 *        Or use streaming mapping:
 *        dma_map_single(dev, cpu_addr, size, direction)
 *        → dma_addr_t
 *
 *      Step 3: Prepare the transfer descriptor
 *        dmaengine_prep_slave_single(chan, dma_addr, len, dir, flags)
 *        → struct dma_async_tx_descriptor *
 *
 *        Configure callback:
 *        desc->callback = my_callback;
 *        desc->callback_param = my_data;
 *
 *      Step 4: Submit and start the transfer
 *        dmaengine_submit(desc);
 *        dma_async_issue_pending(chan);
 *
 *      Step 5: Wait for completion
 *        dma_async_tx_callback  — asynchronous callback
 *        dma_wait_for_async_tx  — synchronous wait
 *
 *      Step 6: Cleanup
 *        dma_unmap_single(dev, dma_addr, size, dir);
 *        dma_release_channel(chan);
 *        dma_free_coherent(dev, size, cpu_addr, dma_handle);
 *
 * 3. DMA address types
 *
 *    Bus Address / DMA Address:
 *      - The address seen by the DMA controller
 *      - On systems with an IOMMU, not equal to physical address
 *      - Obtained via dma_map_single / dma_alloc_coherent
 *
 *    Physical Address:
 *      - Actual memory hardware address
 *      - Equals DMA address when no IOMMU is present
 *
 *    Virtual Address:
 *      - The address seen by the CPU (kernel space)
 *      - The address returned by kmalloc/kzalloc/vmalloc
 *
 * 4. Cache coherency issues
 *
 *    Coherent DMA mapping (Coherent):
 *      dma_alloc_coherent — allocated memory is directly accessible by both CPU and DMA
 *      No need to manually flush the cache, but allocation cost is high
 *
 *    Streaming DMA mapping (Streaming):
 *      dma_map_single — maps ordinary memory as DMA-accessible
 *      Requires manual cache management:
 *        DMA_TO_DEVICE:   dma_sync_single_for_device  (flush before write)
 *        DMA_FROM_DEVICE: dma_sync_single_for_cpu      (invalidate before read)
 *        DMA_BIDIRECTIONAL: bidirectional
 *
 * 5. SPI + DMA transfer
 *
 *    Traditional SPI PIO approach:
 *      spi_write / spi_read — only a few bytes per transfer
 *      CPU polls or interrupts to wait for transfer completion
 *
 *    SPI DMA approach:
 *      spi_async + DMA — suitable for bulk data transfer
 *      A single transfer can be 64KB+
 *      CPU can do other work during DMA transfer
 *
 * 6. Real-world application scenarios
 *
 *    - TFT LCD display: frame buffer DMA transfer (320x240x2 = 150KB/frame)
 *    - Audio codec: PCM data DMA transfer
 *    - SD card / eMMC: block data DMA read/write
 *    - Sensor array: batch sample data DMA transfer
 *    - OTA firmware upgrade: large firmware package written to SPI Flash
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

/* ======================== ioctl commands ======================== */
#define DMA_SPI_IOC_MAGIC       'D'
#define DMA_SPI_IOC_GET_STATS   _IOR(DMA_SPI_IOC_MAGIC, 1, struct dma_spi_stats)
#define DMA_SPI_IOC_RESET_STATS _IO(DMA_SPI_IOC_MAGIC, 2)
#define DMA_SPI_IOC_SET_SPEED   _IOW(DMA_SPI_IOC_MAGIC, 3, u32)
#define DMA_SPI_IOC_GET_SPEED   _IOR(DMA_SPI_IOC_MAGIC, 4, u32)

/*
 * DMA transfer statistics
 *
 * These statistics show the throughput and efficiency of DMA.
 * For example: if bytes_transferred is large but transfer_count is small,
 * it means each transfer is a large block, and DMA efficiency is high.
 * If transfer_count is large but bytes_transferred is small,
 * it means many small data transfers, where DMA overhead is proportionally
 * high — PIO would be better.
 */
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

/* ======================== DMA callback ======================== */

/*
 * DMA transfer completion callback
 *
 * Executes in the DMA interrupt context; cannot do time-consuming operations.
 * Here it only sends the completion signal to let the waiting process continue.
 *
 * Note: cannot call mutex_lock in the callback, cannot sleep,
 *       cannot call functions that may block.
 */
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

/* ======================== DMA transfer core ======================== */

/*
 * dma_spi_transfer — SPI full-duplex transfer using DMA
 *
 * This is the core function of DMA transfer, demonstrating the complete
 * DMA programming flow.
 *
 * Flow:
 *   1. Copy user data to the DMA TX buffer
 *   2. dma_map_single streaming mapping (ensure cache coherency)
 *   3. dmaengine_prep_slave_single to prepare TX descriptor
 *   4. Set callback → dmaengine_submit → dma_async_issue_pending
 *   5. Similarly prepare RX descriptor
 *   6. Trigger SPI transfer (spi_async or direct SPI controller operation)
 *   7. wait_for_completion to wait for DMA completion
 *   8. dma_unmap_single to unmap
 *   9. Copy RX data back to user space
 *
 * Return value: actual bytes transferred, negative = error
 */
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

	/* Step 1: Copy transmit data to DMA buffer */
	if (tx_data)
		memcpy(dev->tx_buf, tx_data, len);
	else
		memset(dev->tx_buf, 0, len);

	/*
	 * Step 2: DMA streaming mapping
	 *
	 * dma_map_single — maps a virtual address to a DMA address
	 *
	 * Parameters:
	 *   &dev->spi->dev   — device pointer (used for IOMMU)
	 *   dev->tx_buf      — CPU virtual address
	 *   len              — mapping length
	 *   DMA_TO_DEVICE    — data transfer direction (CPU→device)
	 *
	 * Return value: DMA address (bus address)
	 *
	 * Note: For DMA_TO_DEVICE, the kernel flushes the CPU cache before
	 *       mapping, ensuring the DMA controller reads the latest data.
	 */
	if (dma_map_single(&dev->spi->dev, dev->tx_buf, len,
			   DMA_TO_DEVICE)) {
		dev_err(&dev->spi->dev, "TX DMA map failed\n");
		return -ENOMEM;
	}

	/*
	 * Step 3: Prepare the DMA transfer descriptor
	 *
	 * dmaengine_prep_slave_single — prepare a single-buffer transfer
	 *
	 * Parameters:
	 *   chan             — DMA channel
	 *   dev->tx_dma      — source address (DMA address)
	 *   len              — transfer length
	 *   DMA_MEM_TO_DEV   — transfer direction (memory→device)
	 *   DMA_PREP_INTERRUPT | DMA_CTRL_ACK
	 *     DMA_PREP_INTERRUPT — trigger interrupt after transfer completes
	 *     DMA_CTRL_ACK       — auto-acknowledge descriptor
	 *
	 * Return value: struct dma_async_tx_descriptor *
	 *               NULL = failure
	 *
	 * Other prep functions:
	 *   dmaengine_prep_slave_sg  — scatter-gather transfer
	 *   dmaengine_prep_dma_cyclic — cyclic transfer (audio)
	 *   dmaengine_prep_dma_memcpy — memory-to-memory copy
	 */
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

	/*
	 * Step 4: Set callback and submit
	 *
	 * The callback is invoked when the DMA transfer completes (interrupt context).
	 * Here completion is used to implement synchronous waiting.
	 */
	tx_desc->callback = dma_spi_tx_callback;
	tx_desc->callback_param = &dev->tx_done;

	/*
	 * dmaengine_submit — adds the descriptor to the DMA channel's pending queue
	 * dma_async_issue_pending — starts the DMA transfer
	 *
	 * Note: submitting is not the same as starting! You must submit first, then issue_pending.
	 *       This allows batch submission of multiple descriptors before a single start.
	 */
	dmaengine_submit(tx_desc);
	dma_async_issue_pending(dev->tx_chan);

	/* RX direction: same flow */
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

	/*
	 * Step 5: Trigger SPI transfer
	 *
	 * DMA descriptors are ready; now trigger the actual transfer via the SPI framework.
	 * The SPI controller will automatically use the DMA channels for data transfer.
	 */
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

	/*
	 * Step 6: Wait for DMA completion
	 *
	 * wait_for_completion — blocks until DMA transfer completes
	 *
	 * Timeout 5 seconds, prevents the process from blocking forever if DMA hangs.
	 * In production, the timeout should be adjusted based on actual transfer size.
	 *
	 * Note: If using asynchronous mode, you don't have to wait,
	 *       let the callback notify user space directly (via poll/signal).
	 */
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

	/*
	 * Step 7: Unmap DMA mapping
	 *
	 * For DMA_FROM_DEVICE mappings, the CPU cache must be invalidated
	 * before dma_unmap_single, so the CPU can read the data written by the device.
	 *
	 * dma_unmap_single handles cache sync internally:
	 *   DMA_FROM_DEVICE → invalidate CPU cache
	 *   DMA_TO_DEVICE   → no operation needed (data already sent)
	 */
	dma_unmap_single(&dev->spi->dev, dev->tx_dma, len, DMA_TO_DEVICE);

	/* Copy received data back to the user-provided buffer */
	if (rx_data)
		memcpy(rx_data, dev->rx_buf, len);

	/* Update statistics */
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

/* ======================== Character device operations ======================== */

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

/*
 * write — send data to the SPI peripheral via DMA
 *
 * Data written by user space is copied to the DMA buffer, then sent to
 * the SPI peripheral via DMA. Suitable for sending large amounts of data
 * (e.g. LCD frame buffer).
 */
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

	/* Copy data from user space */
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

/*
 * read — read data from the SPI peripheral via DMA
 *
 * Sends all-zero bytes (or dummy bytes) to generate the SPI clock,
 * while DMA receives data from MISO simultaneously.
 */
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
	/* Send dummy data, receive actual data */
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

/* ======================== sysfs ======================== */

static inline struct dma_spi_data *to_dma_spi(struct device *dev)
{
	return dev_get_drvdata(dev);
}

/*
 * transfers — total number of transfers (read-only)
 */
static ssize_t transfers_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct dma_spi_data *d = to_dma_spi(dev);
	return sprintf(buf, "%u\n", d->stats.transfer_count);
}
static DEVICE_ATTR_RO(transfers);

/*
 * bytes — total bytes transferred (read-only)
 */
static ssize_t bytes_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct dma_spi_data *d = to_dma_spi(dev);
	return sprintf(buf, "%u\n", d->stats.bytes_transferred);
}
static DEVICE_ATTR_RO(bytes);

/*
 * errors — number of DMA errors (read-only)
 */
static ssize_t errors_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct dma_spi_data *d = to_dma_spi(dev);
	return sprintf(buf, "%u\n", d->stats.dma_errors);
}
static DEVICE_ATTR_RO(errors);

/*
 * avg_time — average transfer time (μs) (read-only)
 */
static ssize_t avg_time_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct dma_spi_data *d = to_dma_spi(dev);
	return sprintf(buf, "%u\n", d->stats.avg_transfer_us);
}
static DEVICE_ATTR_RO(avg_time);

/*
 * speed — SPI clock frequency (read/write)
 */
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

/* ======================== probe / remove ======================== */

/*
 * dma_spi_probe — device initialization
 *
 * Two ways to request a DMA channel:
 *
 * Method 1: Device tree (recommended)
 *   Define dmas and dma-names properties in the device tree:
 *     dmas = <&dmac 0>, <&dmac 1>;
 *     dma-names = "tx", "rx";
 *   In the driver:
 *     dma_request_chan(dev, "tx") → TX channel
 *     dma_request_chan(dev, "rx") → RX channel
 *
 * Method 2: Manual specification (not recommended, for legacy kernels)
 *   dma_request_slave_channel(dev, "tx")
 *
 * Return value: 0 success, negative = error
 *   -EPROBE_DEFER: DMA controller not yet initialized; kernel will retry later
 */
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

	/*
	 * Step 1: Request DMA channel
	 *
	 * dma_request_chan — get a DMA channel from the device tree
	 *
	 * Channel names "tx" and "rx" correspond to the dma-names property in the device tree.
	 *
	 * Note: If it returns -EPROBE_DEFER, you must return this value;
	 *       the kernel will re-invoke probe after the DMA controller is ready.
	 */
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

	/*
	 * Step 2: Allocate coherent DMA buffer
	 *
	 * dma_alloc_coherent — allocate memory directly accessible by both CPU and DMA
	 *
	 * Parameters:
	 *   parent          — device pointer
	 *   DMA_SPI_BUF_SIZE — buffer size
	 *   &dev->tx_dma    — output: DMA address
	 *   GFP_KERNEL      — memory allocation flags
	 *
	 * Return value: CPU virtual address (NULL = failure)
	 *
	 * Coherent mapping characteristics:
	 *   - No need for manual dma_map/dma_unmap
	 *   - CPU writes are immediately visible to DMA (and vice versa)
	 *   - But allocation cost is high; not suitable for frequent alloc/free
	 *   - Usually used for long-lived buffers
	 *
	 * Contrast: If the data comes from user space (a temporary buffer after
	 *           copy_from_user), you should use dma_map_single streaming mapping
	 *           instead of copying into a coherent buffer.
	 */
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

	/*
	 * Step 3: Register character device
	 */
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

/*
 * dma_spi_remove — device removal
 *
 * Cleanup order must be the reverse of allocation order in probe (LIFO):
 *   device_destroy → class_destroy → cdev_del → unregister_chrdev_region
 *   → dma_free_coherent → dma_release_channel
 */
static int dma_spi_remove(struct spi_device *spi)
{
	struct dma_spi_data *dev = spi_get_drvdata(spi);

	if (!dev)
		return 0;

	/* Terminate any in-progress DMA transfers */
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

/*
 * spi_driver — SPI device driver entry
 *
 * Differences from platform_driver:
 *   - spi_driver is specifically for devices on the SPI bus
 *   - The kernel handles SPI device registration and matching automatically
 *   - The probe parameter is struct spi_device * instead of platform_device *
 */
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
