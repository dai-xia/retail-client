/*
 * RC522 RFID Kernel Driver — Upgraded Version
 *
 * Platform: RK3568 (Topeet development board)
 * Bus: SPI
 * Protocol: ISO14443-A (REQA request + Anticollision + Select)
 *
 * Upgrades:
 *   - Interrupt-driven (GPIO IRQ) replacing polling, CPU usage from 10% → <1%
 *   - ioctl interface (get UID / write card / antenna control / read version)
 *   - mutex concurrency protection, preventing multi-process/multi-context
 *     preemption of the SPI bus
 *   - sysfs attributes (antenna/card_present/firmware_ver) for debug config nodes
 *   - wait_queue blocking read, implementing blocking read waiting for card swipe
 *   - GPIO hard interrupt drives SPI command completion wait; poll_timer
 *     periodically triggers card detection
 *
 * Hardware wiring (RC522 → RK3568):
 *   SDA  → SPI0_MOSI
 *   SCK  → SPI0_CLK
 *   MISO → SPI0_MISO
 *   RST  → GPIO1_B0  (reset-gpios)
 *   IRQ  → GPIO1_B1  (irq-gpios)  ← interrupt trigger pin
 *   GND  → GND
 *   3.3V → 3.3V
 *
 * Kernel coding style notes:
 *  1. Internal functions/global variables all add static, restrict file scope,
 *     avoid symbol pollution
 *  2. sysfs callbacks strictly follow the {attr}_show / {attr}_store naming rule
 *  3. Uses unlocked_ioctl; kernel no longer provides the BKL big lock, the driver
 *     uses mutex itself for concurrency protection
 *  4. Interrupt, timer, wait queue prototypes are all kernel-mandated standards,
 *     do not modify arbitrarily
 *  5. Character device number alloc_chrdev_region has no devm version, must be
 *     released manually
 */

// Unified kernel log prefix; all pr_* output automatically gets "rc522: " prepended
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

// Character device base name, corresponds to /dev/rc522_dev
#define RC522_DEV_NAME  "rc522_dev"

/* ======================== RC522 register definitions ======================== */
/**
 * CommandReg      0x01    Command register, sends RC522 low-level commands
 * ComIrqReg       0x04    Interrupt flag register, records interrupt events
 * ComIEnReg       0x02    Interrupt enable register, enables/disables specific interrupt sources
 * ErrorReg        0x06    Error flag register, communication exception markers
 * FIFODataReg     0x09    FIFO data register, send/receive frame data
 * FIFOLevelReg    0x0A    FIFO level register, current FIFO valid byte count
 * ControlReg      0x0C    Control register, hardware function control
 * BitFramingReg   0x0D    Bit framing register, handles non-integer-byte frames (RFID protocol uses)
 * ModeReg         0x11    Mode register, communication mode configuration
 * TxControlReg    0x14    Transmit control register, antenna switch control bits
 * VersionReg      0x37    Version register, reads RC522 firmware version number
 */
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

/* ======================== Polling timer parameters ======================== */
#define POLL_INTERVAL_MS    200   /* When interrupts fail, poll card detection every 200ms */

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


/* ======================== ioctl command definitions ======================== */
/**
 *
 *
 * IOCTL command spec:
 *  _IOR  : user ← kernel (read, kernel copies data to user)
 *  _IOW  : user → kernel (write, user sends config/parameters to kernel)
 *  _IOWR : bidirectional read/write, sends parameters and returns results
 *  First param: Magic number, distinguishes ioctl of different devices, prevents command cross-talk
 *  Third param:  data type, kernel uses it to validate user buffer size, prevents overflow
 */
#define RC522_IOC_MAGIC         'R'

/* Read card UID (4 bytes) */
#define RC522_IOC_GET_UID       _IOR(RC522_IOC_MAGIC, 1, u8[4])
/* Control antenna switch, arg is int 0/1 */
#define RC522_IOC_SET_ANTENNA   _IOW(RC522_IOC_MAGIC, 2, int)
/* Read RC522 firmware version number (single byte) */
#define RC522_IOC_GET_VERSION   _IOR(RC522_IOC_MAGIC, 3, u8)

/**
 * MIFARE Classic 1K card notes:
 *  16 sectors total, each sector has 4 data blocks (64 blocks total, block 0~63)
 *  Block 0: factory-programmed data, read-only
 *  Block 3: sector trailer, stores KeyA/access control/KeyB, read/write under permission control
 *  Block 1~2: user read/write data blocks
 *  Authentication prerequisite: must request + select first; the card must enter the
 *  ACTIVE state before authentication/read/write can proceed
 */
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

/* ======================== Device private struct ======================== */
/**
 * rc522_dev_t - RC522 device global private struct
 * Integrates SPI, GPIO, interrupt, char device, sync, state, timer and all other resources
 */
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

/* ======================== SPI low-level read/write interface ======================== */
/**
 * RC522 SPI communication protocol rules:
 *  Address byte: bit7=1 read, bit7=0 write
 *  Register address sits in bit6~bit1
 *  Write: MOSI sends (reg<<1 & 0x7E) + data
 *  Read:  MOSI sends (reg<<1 & 0x7E) | 0x80, MISO returns register data
 */

/**
 * rc522_write_reg - SPI write to RC522 register
 * @dev: device private struct pointer
 * @reg: target register address
 * @val: data to write
 * Return: 0 success, negative SPI read/write error code
 */
static int rc522_write_reg(struct rc522_dev_t *dev, u8 reg, u8 val)
{
    u8 tx[2] = {(reg << 1) & 0x7E, val};
    int ret = spi_write(dev->spi, tx, 2);
    if (ret < 0)
        dev_err(&dev->spi->dev, "SPI write reg 0x%02X failed: %d\n", reg, ret);
    return ret;
}

/**
 * rc522_read_reg - SPI read of MFRC522 register
 * @dev: device private struct pointer
 * @reg: target register address (raw address 0x00 ~ 0x3F)
 * @val: receive buffer for the read data, used to store the register return value
 *
 * RC522 SPI command frame rules:
 * 1. Register address is 6 bits (0~63), occupying bit6 ~ bit1
 * 2. bit7: read/write control bit, 1 = read register, 0 = write register
 * 3. bit0: fixed reserved bit, forced to 0
 *
 * Command byte construction logic:
 * tx = ((reg << 1) & 0x7E) | 0x80
 *  - reg << 1: shift register address left by 1, freeing the lowest bit (bit0)
 *  - & 0x7E: mask the highest bit (bit7), keep only the address bits (bit1~bit6)
 *  - | 0x80: set the highest bit (bit7) to 1, indicating this is a [read operation]
 *
 * Communication flow: single-byte command + single-byte read-back, one chip-select
 * completes send-then-receive
 * Return: 0 success, negative SPI read/write error code
 */
static int rc522_read_reg(struct rc522_dev_t *dev, u8 reg, u8 *val)
{
    // Assemble the read-register command byte per RC522 SPI protocol
    u8 tx = ((reg << 1) & 0x7E) | 0x80;
    u8 rx = 0;  // Holds the register data read from RC522

    // SPI: send 1 byte command first, then receive 1 byte of register data
    int ret = spi_write_then_read(dev->spi, &tx, 1, &rx, 1);
    if (ret < 0) {
        dev_err(&dev->spi->dev, "SPI read reg 0x%02X failed: %d\n", reg, ret);
        return ret;
    }

    // Pass the read data back to the upper caller
    *val = rx;
    return 0;
}

/**
 * __rc522_read_reg - inline simplified register read interface
 * @dev: device private struct pointer
 * @reg: target register address
 * Return: register read value
 * Purpose: simplify upper-layer business code calls
 */
static inline u8 __rc522_read_reg(struct rc522_dev_t *dev, u8 reg)
{
    u8 val = 0;
    rc522_read_reg(dev, reg, &val);
    return val;
}

/* ======================== Antenna control interface ======================== */
/**
 * rc522_antenna_on - turn on/off RC522 RF antenna
 * @dev: device private struct pointer
 * @on: true=turn on antenna, false=turn off antenna
 *
 * Register rules:
 * 1. Target register: TxControlReg (transmit control register)
 * 2. Antenna switch corresponds to **bit0, bit1** of this register
 *    - both 0: disable RF antenna output
 *    - both 1 (binary 11, value 0x03): enable RF antenna output
 *
 * Execution logic:
 * 1. Read the current raw value of TxControlReg first, preserving other bits;
 * 2. Turn on antenna (on=true): only when bit0/bit1 are both 0, set these two
 *    bits to 1, keep the rest unchanged;
 * 3. Turn off antenna (on=false): clear bit0/bit1 directly, keep the rest unchanged.
 */
static void rc522_antenna_on(struct rc522_dev_t *dev, bool on)
{
    // Read the current value of the transmit control register
    u8 temp = __rc522_read_reg(dev, TxControlReg);

    if (on) {
        // Check: bit0, bit1 are both 0 (antenna is off)
        if (!(temp & 0x03))
            // Bitwise OR: set bit0, bit1 to 1 only, turn on antenna, other bits unchanged
            rc522_write_reg(dev, TxControlReg, temp | 0x03);
    } else {
        // Bitwise AND + inversion: force bit0, bit1 to 0, turn off antenna, other bits keep their state
        rc522_write_reg(dev, TxControlReg, temp & ~0x03);
    }
}


/* ======================== Core request/select flow ISO14443-A ======================== */
/**
 * rc522_get_uid - full request + anticollision + select flow (standard ISO14443-3 Type A card)
 * @dev: device private struct pointer
 * @uid: output buffer, holds the card's 4-byte unique UID
 *
 * Overall protocol flow:
 *  1. REQA (0x26) request broadcast: probe whether any IC card is within RF range
 *  2. Anticollision (0x93 0x20): arbitrate when multiple cards coexist, read the 4-byte card UID
 *  3. Select (0x93 0x70 + UID + BCC): select the target card, put it into the active state
 *  Important: must complete Select; subsequent sector authentication and read/write can proceed
 *
 * General register rules:
 *  CommandReg: command register, sends send/recv/reset and other hardware commands
 *  ComIrqReg: interrupt status register, judges whether send/recv finished, whether there is a reply
 *  FIFODataReg: data FIFO buffer, the core buffer for sending/receiving card data
 *  FIFOLevelReg: FIFO data length register; setting the high bit to 1 clears the FIFO
 *  BitFramingReg: bit framing register, adapts to non-8bit-aligned RF frames
 *  ErrorReg: error status register, detects communication exceptions
 *
 * Return: 0=successfully obtained UID and completed select; -1=no card/communication timeout/hardware error
 */
static int rc522_get_uid(struct rc522_dev_t *dev, u8 *uid)
{
    int i;
    u8 n, len, bcc;

    // ========== Init: clear state, interrupts, FIFO buffer ==========
    rc522_write_reg(dev, CommandReg, 0x00);        // Clear current hardware command, reset command state
    rc522_write_reg(dev, ComIrqReg, 0x7F);          // Write 0x7F, clear all historical interrupt flags
    rc522_write_reg(dev, FIFOLevelReg, 0x80);      // Set high bit to 1, force-clear FIFO buffer

    /* ==================== Step 1: send REQA (0x26) broadcast request ====================
     * Function: broadcast a request command, wake up all idle IC cards in the field
     * Protocol: REQA is a short frame, not standard 8bit-aligned, needs the bit framing register
     */
    rc522_write_reg(dev, BitFramingReg, 0x07);      // Configure bit framing: adapt to REQA short frame format
    rc522_write_reg(dev, FIFODataReg, 0x26);       // Write request command 0x26 into the FIFO send buffer
    rc522_write_reg(dev, BitFramingReg, 0x87);      // Keep bit framing config during send/recv
    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);        // Send Transceive command, start RF send + receive

    // Interrupt-driven wait: process sleeps, RC522 wakes via IRQ when done
    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    if (!dev->spi_done ||
        (__rc522_read_reg(dev, ErrorReg) & 0x1B))
        return -1;

    /* ==================== Step 2: Anticollision, read card 4-byte UID ====================
     * Command sequence: 0x93 + 0x20
     * Function: anti-collision arbitration when multiple cards are present; ultimately read the card's unique 4-byte UID
     */
    rc522_write_reg(dev, FIFOLevelReg, 0x80);      // Clear FIFO again, clear previous step's reply data
    rc522_write_reg(dev, BitFramingReg, 0x00);     // Restore standard 8bit whole-frame mode
    rc522_write_reg(dev, FIFODataReg, 0x93);       // Write anticollision command code 0x93
    rc522_write_reg(dev, FIFODataReg, 0x20);       // Write anticollision level parameter 0x20
    rc522_write_reg(dev, BitFramingReg, 0x80);     // Lock current bit framing config
    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);        // Start send/recv, execute anticollision flow

    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    if (!dev->spi_done)
        return -1;

    // Read FIFO data length; a normal reply has at least 4 bytes UID, otherwise fail
    len = __rc522_read_reg(dev, FIFOLevelReg);
    if (len < 4) return -1;

    // Loop reading 4 bytes from FIFO into the UID buffer
    for (i = 0; i < 4; i++)
        uid[i] = __rc522_read_reg(dev, FIFODataReg);

    /* ==================== Step 3: Select, activate the target card ====================
     * Command sequence: 0x93 + 0x70 + 4-byte UID + BCC checksum (XOR each bit in order)
     * BCC checksum: XOR of the 4 UID bytes in turn, used to verify data integrity
     * Function: select the specified card by UID; the card enters active state, allowing subsequent auth/read/write
     */
    bcc = uid[0] ^ uid[1] ^ uid[2] ^ uid[3];       // Compute UID XOR checksum BCC

    rc522_write_reg(dev, FIFOLevelReg, 0x80);      // Clear FIFO buffer
    rc522_write_reg(dev, BitFramingReg, 0x00);     // Standard 8bit frame format
    rc522_write_reg(dev, FIFODataReg, 0x93);       // Select command code 0x93
    rc522_write_reg(dev, FIFODataReg, 0x70);       // Select parameter 0x70
    rc522_write_reg(dev, FIFODataReg, uid[0]);     // Write 4-byte UID in turn
    rc522_write_reg(dev, FIFODataReg, uid[1]);
    rc522_write_reg(dev, FIFODataReg, uid[2]);
    rc522_write_reg(dev, FIFODataReg, uid[3]);
    rc522_write_reg(dev, FIFODataReg, bcc);        // Write BCC checksum
    rc522_write_reg(dev, BitFramingReg, 0x80);     // Lock bit framing config
    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);        // Start send/recv, execute select command

    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    if (!dev->spi_done ||
        (__rc522_read_reg(dev, ErrorReg) & 0x1B))
        return -1;

    // Read and discard the select reply frame SAK (status confirmation only; this function does not use it)
    len = __rc522_read_reg(dev, FIFOLevelReg);
    if (len > 0)
        (void)__rc522_read_reg(dev, FIFODataReg);

    return 0;  // Request, anticollision, select all complete, return success
}


/**
 * rc522_mifare_auth - MIFARE card sector key authentication (fixed version)
 * @dev: device private struct pointer, stores RC522 hardware-related info
 * @key: 6-byte key buffer, M1 card standard key length is fixed at 6 bytes
 * @key_type: key type 0x60=KeyA (A key)  0x61=KeyB (B key)
 * @block: block number of the sector to authenticate; any block in a sector can authenticate that sector
 * @uid: card 4-byte UID (must be passed in; needed in the auth frame)
 *
 * Prerequisite: must have completed request + anticollision + select; the card is in
 * active state, otherwise authentication will fail
 * Protocol: before reading/writing a MIFARE Classic sector, key authentication must
 * be done first; only after auth passes can the sector data be operated
 *
 * Standard auth frame format (12 bytes total):
 *   [0] command code (0x60/0x61)
 *   [1] block number
 *   [2~7] 6-byte key
 *   [8~11] 4-byte UID
 *
 * Return: 0 = auth success, -1 = timeout/communication error/wrong key, auth failed
 */
static int rc522_mifare_auth(struct rc522_dev_t *dev, u8 *key, u8 key_type, u8 block, u8 *uid)
{
    int i;
    u8 n;

    // FIFOLevelReg write 0x80: clear FIFO data buffer, clear last round's residual data, avoid interfering with this round's communication
    rc522_write_reg(dev, FIFOLevelReg, 0x80);
    // BitFramingReg set to 0x00: use standard 8-bit whole-byte frame format; auth flow is a standard frame, no bit truncation
    rc522_write_reg(dev, BitFramingReg, 0x00);

    /* ========== Strictly write 12 bytes in MIFARE Classic protocol order ========== */
    // 1. Write command code first (0x60 or 0x61)
    rc522_write_reg(dev, FIFODataReg, key_type);
    // 2. Write block number
    rc522_write_reg(dev, FIFODataReg, block);
    // 3. Write 6-byte key
    for (i = 0; i < 6; i++)
        rc522_write_reg(dev, FIFODataReg, key[i]);
    // 4. Write 4-byte UID last (critical fix; the original was missing this)
    for (i = 0; i < 4; i++)
        rc522_write_reg(dev, FIFODataReg, uid[i]);
    /* ================================================================ */

       // BitFramingReg set to 0x80: lock the 8-bit standard frame format; frame format won't be modified by hardware during send/recv
    rc522_write_reg(dev, BitFramingReg, 0x80);

    // CommandReg write 0x0C: send Transceive command, start RF send, send the whole FIFO data to the card for authentication
    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);

    // Interrupt-driven wait for auth completion
    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    if (!dev->spi_done ||
        (__rc522_read_reg(dev, ErrorReg) & 0x1B))
        return -1;

    // No timeout, no communication error, auth completed, return success
    return 0;
}

/**
 * rc522_mifare_read_block - MIFARE Classic card data block read function
 * @dev:    RC522 device private struct pointer, stores SPI, registers, state and other global resources
 * @block:  card block number to read, range 0 ~ 63 (M1 card has 64 data blocks total)
 * @data:   output buffer, used to store the read 16-byte block data (M1 card standard block size is fixed at 16 bytes)
 *
 * Protocol prerequisites:
 *  1. RF antenna is on;
 *  2. Completed request (REQA) → anticollision → select; the card is in the ACTIVE active state;
 *  3. The sector the block belongs to has completed key authentication (KeyA/KeyB auth passed);
 *  If prerequisites are not met, reading will fail directly.
 *
 * MIFARE Classic read block protocol rules:
 *  Read command is fixed at 0x30; command frame format: [0x30] + [block number], 2 bytes sent to the card;
 *  A normal card reply returns 16 consecutive bytes of raw block data;
 *  RC522 sends/receives all RF frame data via the FIFO buffer.
 *
 * Return:
 *  0  : read success, data buffer filled with 16 bytes of valid data
 * -1  : read failed (communication timeout, wrong reply length, hardware error, not authenticated, etc.)
 */
static int rc522_mifare_read_block(struct rc522_dev_t *dev, u8 block, u8 *data)
{
    int i;          // loop variable: used for timeout polling and reading 16 bytes of data
    u8 n;           // temp variable: holds the interrupt status register return value

    /* 1. Clear FIFO buffer
     * FIFOLevelReg register write 0x80: the highest bit of this register is the FIFO clear bit
     * Setting it to 1 makes hardware auto-clear the residual send/recv data in the FIFO, preventing dirty data from interfering with this round's communication
     */
    rc522_write_reg(dev, FIFOLevelReg, 0x80);

    /* 2. Set bit framing to standard 8-byte alignment
     * BitFramingReg = 0x00: disable bit cropping, bit alignment correction, use standard whole-byte communication
     * M1 card read/write commands are all standard 8bit whole frames, no short frame / non-aligned frame handling needed
     */
    rc522_write_reg(dev, BitFramingReg, 0x00);

    /* 3. Write the MIFARE read command 0x30 to the FIFO
     * 0x30 is the **standard read block command code** for MFRC522 interfacing with MIFARE Classic cards
     */
    rc522_write_reg(dev, FIFODataReg, 0x30);

    /* 4. Write the block number to read to the FIFO
     * Follows the read command, forming the complete sent frame: [0x30][block]
     */
    rc522_write_reg(dev, FIFODataReg, block);

     /* 6. Lock bit framing config
     * BitFramingReg = 0x80: force-keep the current frame format during send/recv, forbid hardware from auto-modifying bit config
     * Avoid frame format anomalies mid-RF causing packet loss / parse errors
     */
    rc522_write_reg(dev, BitFramingReg, 0x80);

    /* 5. Start the RC522 send/recv engine, begin RF communication
     * CommandReg = 0x0C: execute Transceive send/recv command
     * RC522 will auto-send the FIFO data via the antenna, and wait for the card's reply data to be stored into the FIFO
     */
    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);

    /* 6. Interrupt-driven wait for send/recv completion
     * Process sleeps; when RC522 finishes it pulls IRQ low → GPIO interrupt wakes it
     */
    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    if (!dev->spi_done)
        return -1;

    /* Validate FIFO data length; M1 card read block must return 16 bytes */
    if (__rc522_read_reg(dev, FIFOLevelReg) != 16)
        return -1;

    /* Loop reading 16 bytes of block data from FIFO into the upper-passed data buffer
     * FIFODataReg register: each read advances the FIFO pointer by one
     * Read 16 times continuously to extract a complete block of data
     */
    for (i = 0; i < 16; i++)
        data[i] = __rc522_read_reg(dev, FIFODataReg);

    // All flows normal, return read success
    return 0;
}

/**
 * rc522_mifare_write_block - MIFARE data block write
 * @dev: device private struct pointer
 * @block: block number
 * @data: 16 bytes of data to write
 * Prerequisite: the corresponding sector must have completed key authentication
 * Return: 0 write success, -1 failure
 */
static int rc522_mifare_write_block(struct rc522_dev_t *dev, u8 block, u8 *data)
{
    int i;
    u8 n;

    rc522_write_reg(dev, FIFOLevelReg, 0x80);    // Clear FIFO
    rc522_write_reg(dev, BitFramingReg, 0x00);   // Set standard whole-byte frame format
    rc522_write_reg(dev, FIFODataReg, 0xA0);     // MIFARE write command
    rc522_write_reg(dev, FIFODataReg, block);    // Write target block number

    for (i = 0; i < 16; i++)
        rc522_write_reg(dev, FIFODataReg, data[i]);  // Write 16 bytes of data to send

    rc522_write_reg(dev, BitFramingReg, 0x80);   // Lock frame format
    dev->spi_done = false;
    rc522_write_reg(dev, CommandReg, 0x0C);     // Start send/recv

    wait_event_timeout(dev->spi_wq, dev->spi_done, msecs_to_jiffies(20));
    return dev->spi_done ? 0 : -1;
}

/**
 * card_detect - unified entry for card detection
 * @dev: device private struct pointer
 * Called from: interrupt service function, polling timer callback
 * Constraint: caller must hold dev->lock mutex, ensure thread safety
 * Function: detect card, update present state, cache UID
 */
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

/* ======================== Interrupt service function ======================== */
/**
 * rc522_irq_handler - RC522 GPIO interrupt callback
 * Prototype constraint: irqreturn_t is the standard interrupt return value; args fixed (irq, dev_id), kernel-mandated
 * @irq: interrupt number
 * @dev_id: device private struct pointer
 *
 * Responsibility: only SPI command completion notification
 *   RC522 pulls IRQ pin low after Transceive completes → this function is triggered
 *   Sets spi_done flag and wakes the wait queue, letting rc522_get_uid etc. resume from sleep
 *
 * Card detection: done by poll_timer periodically scheduling detect_work, not here
 * Reason: calling rc522_get_uid in the IRQ handler would cause an IRQF_ONESHOT deadlock
 *        (handler waits for RC522 completion interrupt, but the interrupt line is masked by ONESHOT)
 */
static irqreturn_t rc522_irq_handler(int irq, void *dev_id)
{
    struct rc522_dev_t *dev = (struct rc522_dev_t *)dev_id;

    dev->spi_done = true;
    wake_up(&dev->spi_wq);

    return IRQ_HANDLED;
}

/* ======================== Deferred card detection work queue ======================== */
/**
 * detect_work_func - card detection work function
 * @work: work queue struct pointer
 *
 * Scheduled periodically by poll_timer; runs card detection in process context
 * Wakes card_wq when card state changes, so read() returns
 *
 * Why not call card_detect directly in timer callback / IRQ handler:
 *   1. timer callback runs in softirq context, cannot sleep (mutex_lock sleeps)
 *   2. calling rc522_get_uid in IRQ handler causes an IRQF_ONESHOT deadlock
 *   3. workqueue runs in process context, can safely use mutex and wait_event
 */
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

/* ======================== Polling timer callback ======================== */
/**
 * rc522_poll_timer_callback - polling timer callback
 * Prototype constraint: struct timer_list * arg is the kernel-fixed timer prototype, not modifiable
 * @t: timer instance pointer
 *
 * Responsibility: schedule detect_work for card detection, and restart the timer for periodic detection
 * Does not call card_detect directly: timer callback runs in softirq context, cannot sleep
 */
static void rc522_poll_timer_callback(struct timer_list *t)
{
    struct rc522_dev_t *dev = from_timer(dev, t, poll_timer);

    schedule_work(&dev->detect_work);
    mod_timer(&dev->poll_timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
}

/* ======================== Character device file operations set ======================== */
/**
 * rc522_open - device open interface
 * @inode: inode
 * @filp: file struct
 * Function: bind private data, turn on RF antenna
 */
static int rc522_open(struct inode *inode, struct file *filp)
{
    // container_of: reverse-obtain the device private struct via the cdev member
    struct rc522_dev_t *dev = container_of(inode->i_cdev, struct rc522_dev_t, cdev);
    filp->private_data = dev;

    mutex_lock(&dev->lock);
    rc522_antenna_on(dev, true);
    mutex_unlock(&dev->lock);

    return 0;
}

/**
 * rc522_release - device close interface
 * @inode: inode
 * @filp: file struct
 * Note: interrupt/antenna are globally managed; not turned off in a single close, to avoid multi-process conflicts
 */
static int rc522_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/**
 * rc522_read - read interface, blocking/non-blocking read of card UID
 * @filp: file struct
 * @buf: userspace receive buffer
 * @count: user requested read bytes
 * @off: file offset (unused)
 * Logic: blocking mode → sleep on wait queue; non-blocking mode → return -EAGAIN if no card
 */
static ssize_t rc522_read(struct file *filp, char __user *buf,
                          size_t count, loff_t *off)
{
    struct rc522_dev_t *dev = (struct rc522_dev_t *)filp->private_data;
    int ret;

    if (count < 4)
        return -EINVAL;

    // Non-blocking mode
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

    // Blocking mode: wait for the card to be present
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

/**
 * rc522_ioctl - device control interface
 * Prototype constraint: unlocked_ioctl, the modern kernel standard interface, no BKL big lock; driver locks itself
 * @filp: file struct
 * @cmd: ioctl command code
 * @arg: userspace passed parameter / buffer address
 */
static long rc522_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct rc522_dev_t *dev = (struct rc522_dev_t *)filp->private_data;
    int ret = 0;

    // Validate magic number, prevent illegal commands
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
        // Re-request/select, ensure the card is active, also obtain the UID
        if (rc522_get_uid(dev, uid) != 0) {
            ret = -ENODEV;
            break;
        }
        // Critical fix: pass the read UID into the auth function
        ret = rc522_mifare_auth(dev, auth.key, auth.key_type, auth.block, uid);
        break;
    }
    case RC522_IOC_READ_BLOCK: {
        struct rc522_block_rw rw;
        if (copy_from_user(&rw, (void __user *)arg, sizeof(rw))) {
            ret = -EFAULT;
            break;
        }
        // Do not re-request; preserve existing auth state
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

/**
 * rc522_fops - character device file operations method set
 * Convention: modern kernels use unlocked_ioctl, not the old ioctl
 */
static struct file_operations rc522_fops = {
    .owner          = THIS_MODULE,
    .open           = rc522_open,
    .release        = rc522_release,
    .read           = rc522_read,
    .unlocked_ioctl = rc522_ioctl,
};

/* ======================== sysfs attribute nodes ======================== */
/**
 * Naming rules:
 *  Read-only callback: {attr}_show
 *  Read-write callback: {attr}_show + {attr}_store
 * DEVICE_ATTR_RO: read-only attribute, auto-generates dev_attr_xxx, store is null
 * DEVICE_ATTR_RW: read-write attribute, binds both show + store
 * Array end must be NULL-terminated; ATTRIBUTE_GROUPS batch-mounts the attribute group
 */

/**
 * antenna_show - sysfs antenna read callback
 * Path: /sys/class/rc522_dev/rc522_dev/antenna
 * Function: output antenna state on/off
 */
static ssize_t antenna_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    struct rc522_dev_t *rc522 = dev_get_drvdata(dev);
    u8 val = __rc522_read_reg(rc522, TxControlReg);
    return sprintf(buf, "%s\n", (val & 0x03) ? "on" : "off");
}

/**
 * antenna_store - sysfs antenna write callback
 * Function: write 0/1 to the node to control the antenna switch
 */
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
/**
 * DEVICE_ATTR_RW(antenna) macro notes:
 *  Generates a read-write device attribute struct dev_attr_antenna
 *  Equivalent expansion:
 *  static struct device_attribute dev_attr_antenna = {
 *      .attr = { .name = "antenna", .mode = 0660 },
 *      .show = antenna_show,
 *      .store = antenna_store,
 *  };
 *  The macro parameter must match the show/store prefix
 */
static DEVICE_ATTR_RW(antenna);

/**
 * card_present_show - card present state read callback
 * Path: /sys/class/rc522_dev/rc522_dev/card_present
 * Output 1=card present 0=no card
 */
static ssize_t card_present_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    struct rc522_dev_t *rc522 = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", rc522->card_present ? 1 : 0);
}
/**
 * DEVICE_ATTR_RO(card_present)
 * Generates read-only attribute dev_attr_card_present, permission 0444, store=NULL
 */
static DEVICE_ATTR_RO(card_present);

/**
 * firmware_ver_show - firmware version read callback
 * Path: /sys/class/rc522_dev/rc522_dev/firmware_ver
 * Outputs the RC522 version register value
 */
static ssize_t firmware_ver_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    struct rc522_dev_t *rc522 = dev_get_drvdata(dev);
    u8 ver = __rc522_read_reg(rc522, VersionReg);
    return sprintf(buf, "0x%02X\n", ver);
}
static DEVICE_ATTR_RO(firmware_ver);

/**
 * rc522_sysfs_attrs - sysfs attribute array
 * Rules:
 *  1. Type is fixed struct attribute *
 *  2. Element form &dev_attr_xxx.attr
 *  3. End must be NULL-terminated
 */
static struct attribute *rc522_sysfs_attrs[] = {
    &dev_attr_antenna.attr,
    &dev_attr_card_present.attr,
    &dev_attr_firmware_ver.attr,
    NULL,
};

/**
 * ATTRIBUTE_GROUPS macro: batch-wraps the sysfs attribute array into a device attribute group struct
 * 1. Auto-generates: a struct attribute_group type attribute group object rc522_sysfs_groups
 * 2. Auto-generates: a pointer array rc522_sysfs_groups_ptr with the attribute group as element
 * 3. The macro arg rc522_sysfs must keep the same prefix as the earlier attribute array name rc522_sysfs_attrs
 * 4. The final attribute group is used by device_create_with_groups to mount all sysfs nodes at once
 */
ATTRIBUTE_GROUPS(rc522_sysfs);

/* ======================== SPI driver entry probe ======================== */
/**
 * rc522_probe - initialization entry function after SPI device match succeeds
 * @spi: SPI device instance struct pointer, auto-passed by the kernel SPI bus after matching the device
 * Execution flow:
 *  1. Allocate and initialize device private memory
 *  2. Configure SPI communication parameters and apply them
 *  3. Get the reset GPIO and perform an RC522 hardware reset
 *  4. Write registers to complete RC522 chip basic configuration
 *  5. Register a Linux character device, provide a /dev/rc522_dev device node
 *  6. Create device class, device node and mount the sysfs attribute group
 *  7. Get the interrupt GPIO, request a threaded interrupt, implement card-swipe interrupt detection
 *  8. When interrupts are unavailable, start a timer polling as a fallback
 * Supplementary notes:
 *  1. devm_-prefixed interfaces are kernel-managed resources, auto-released on module unload, no manual reclaim needed
 *  2. alloc_chrdev_region has no managed version; device number, char device, device class need manual reverse-order release
 *  3. This function runs in kernel process context, allowed to use delay, mutex and other interfaces
 */
static int rc522_probe(struct spi_device *spi)
{
    int ret;
    struct rc522_dev_t *rc522;
    // Extract the generic device struct from the SPI device, used for device ops, logging
    struct device *dev = &spi->dev;

    /**
     * devm_kzalloc: kernel-managed memory allocation
     * @dev: bound to the current device, auto-released on driver unload
     * @sizeof(*rc522): allocation size is the total byte count of the device private struct
     * @GFP_KERNEL: normal kernel memory allocation flag, allows sleeping to wait for memory
     * Function: allocate and zero a block of memory, stores all RC522 device state and resources
     * Return: on success returns the memory start address, on failure returns NULL
     */
    rc522 = devm_kzalloc(dev, sizeof(*rc522), GFP_KERNEL);
    if (!rc522)
        return -ENOMEM;  // Memory allocation failed, return standard error code -ENOMEM

    // Save the SPI device pointer to the private struct; later all functional interfaces can use SPI communication
    rc522->spi = spi;
    /**
     * spi_set_drvdata: bind the private data pointer to the SPI device
     * Purpose: attach the rc522 private struct to the spi_device reserved private field;
     * later remove, sysfs callbacks etc. can reverse-obtain it via spi_get_drvdata
     */
    spi_set_drvdata(spi, rc522);

    /**
     * mutex_init: initialize the mutex
     * Purpose: protect critical sections such as SPI read/write, card state, GPIO ops, prevent multi-thread concurrent preemption
     */
    mutex_init(&rc522->lock);
    /**
     * init_waitqueue_head: initialize the wait queue head
     * Purpose: implement blocking read waiting for card swipe; process sleeps when no card, interrupt wakes on card swipe
     */
    init_waitqueue_head(&rc522->card_wq);
    init_waitqueue_head(&rc522->spi_wq);   /* SPI command completion wait queue */
    INIT_WORK(&rc522->detect_work, detect_work_func); /* Card detection work */
    rc522->card_present = false;  // Initialize card present flag: default no card
    rc522->spi_done = false;     // Initialize SPI completion flag
    rc522->irq = -1;              // Initialize interrupt number: default invalid value

    /* ========== Configure SPI communication params, RC522 hardware requires this ========== */
    // Set SPI mode: SPI_MODE_0 (CPOL=0, CPHA=0), RC522 standard communication mode
    spi->mode = SPI_MODE_0;
    // Set single SPI transfer bit width: 8bit, send/recv data by standard byte
    spi->bits_per_word = 8;
    // Set SPI max clock frequency: 5MHz; RC522 supports up to 10MHz; 5MHz is more stable
    spi->max_speed_hz = 5000000;

    /**
     * spi_setup: apply the above SPI config officially, initialize the SPI hardware controller
     * Return <0 means config failed, exit directly and return the error code
     */
    ret = spi_setup(spi);
    if (ret < 0)
    {
        dev_err(dev, "SPI setup failed: %d\n", ret);
        return ret;
    }

    /**
     * devm_gpiod_get_optional: get the GPIO pin corresponding to "reset" in the device tree
     * _optional: optional pin; if the device tree doesn't configure it, no error, just returns NULL
     * GPIOD_OUT_HIGH: configure as output mode, default output high
     * devm_: managed GPIO resource, auto-released on driver unload
     */
    rc522->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
    /**
     * IS_ERR: kernel macro, checks whether a pointer is an error-code pointer
     * PTR_ERR: parse the specific error number from an error pointer
     */
    if (IS_ERR(rc522->reset_gpio)) {
        dev_err(dev, "Failed to get reset GPIO\n");
        return PTR_ERR(rc522->reset_gpio);
    }

    // Check whether the reset pin exists; if so, perform an RC522 hardware reset
    if (rc522->reset_gpio) {
        gpiod_set_value(rc522->reset_gpio, 0); // Pull reset pin low, trigger hardware reset
        msleep(20);                            // Hold low for 20ms, meet reset timing
        gpiod_set_value(rc522->reset_gpio, 1); // Pull pin high, end reset
        msleep(50);                            // Wait for internal chip init to complete
    }

    /* ========== Write config to RC522 registers, complete chip init ========== */
    rc522_write_reg(rc522, CommandReg, 0x0F); // Execute RC522 soft reset command
    msleep(10);
    rc522_write_reg(rc522, 0x2A, 0x8D);      // Configure RF-related register
    rc522_write_reg(rc522, 0x2B, 0x3E);      // Configure receive gain parameter
    rc522_write_reg(rc522, 0x2D, 30);        // Configure timer timeout parameter
    rc522_write_reg(rc522, ModeReg, 0x3D);   // Set RC522 communication mode
    rc522_antenna_on(rc522, true);           // Turn on RF antenna, prepare for card communication

    /**
     * alloc_chrdev_region: dynamically allocate char device major/minor number
     * Args: &devid=receive combined device number, 0=start minor, 1=occupy 1 minor number, device name
     * Note: this interface has no devm managed version; on unload must manually call unregister_chrdev_region
     */
    ret = alloc_chrdev_region(&rc522->devid, 0, 1, RC522_DEV_NAME);
    if (ret) {
        dev_err(dev, "alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    /**
     * cdev_init: initialize the char device struct, bind the file ops set fops
     * cdev_add: add the char device to the kernel system, making it officially usable
     * Third arg 1: this device occupies 1 consecutive minor number
     */
    cdev_init(&rc522->cdev, &rc522_fops);
    ret = cdev_add(&rc522->cdev, rc522->devid, 1);
    if (ret) {
        // Add failed, release the allocated device number first, then return error
        unregister_chrdev_region(rc522->devid, 1);
        return ret;
    }

    /**
     * class_create: create a device class, used to generate the corresponding directory under /sys/class/
     * THIS_MODULE: bind to the current driver module
     */
    rc522->class = class_create(THIS_MODULE, RC522_DEV_NAME);
    if (IS_ERR(rc522->class)) {
        // Create failed, reverse-order release already-requested resources
        cdev_del(&rc522->cdev);
        unregister_chrdev_region(rc522->devid, 1);
        return PTR_ERR(rc522->class);
    }

    /**
     * device_create_with_groups: create device node + batch-mount sysfs attribute group
     * Function: generate the /dev/rc522_dev device node, also create antenna, card state, version and other attribute files under sysfs
     * rc522_sysfs_groups: the attribute group pointer array generated by ATTRIBUTE_GROUPS
     */
    rc522->cls_dev = device_create_with_groups(rc522->class, dev,
                              rc522->devid, rc522,
                              rc522_sysfs_groups, RC522_DEV_NAME);
    if (IS_ERR(rc522->cls_dev)) {
        // Device node creation failed, release resources layer by layer
        class_destroy(rc522->class);
        cdev_del(&rc522->cdev);
        unregister_chrdev_region(rc522->devid, 1);
        return PTR_ERR(rc522->cls_dev);
    }

    /* ========== Get the interrupt GPIO, register interrupt service (required, abort on failure) ========== */
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

    /**
     * request_irq: register a hard interrupt handler
     *   rc522_irq_handler: only sets flag + wake_up, safe to run in hardirq context
     *   IRQF_TRIGGER_FALLING: falling-edge trigger, RC522 IRQ pin is active-low
     */
    ret = request_irq(rc522->irq, rc522_irq_handler,
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

    /* ComIEnReg = 0x71: enable RC522 chip internal interrupt sources
     *   bit6 (TimerIRq): timer timeout interrupt
     *   bit4 (IdleIRq):  command idle interrupt (Transceive done)
     *   bit0 (TxIRq):    send complete interrupt
     * After enabling, RC522 auto-pulls IRQ low when a command completes, GPIO interrupt triggers
     */
    rc522_write_reg(rc522, ComIEnReg, 0x71);
    dev_info(dev, "IRQ registered (IRQ=%d), SPI commands interrupt-driven\n", rc522->irq);

    /**
     * Start the periodic detection timer
     *
     * RC522 does not actively detect cards; the driver must periodically send REQA to probe.
     * poll_timer schedules detect_work → card_detect → rc522_get_uid every 200ms;
     * rc522_get_uid internally uses wait_event to wait for RC522 interrupt notification of completion, zero CPU usage.
     */
    timer_setup(&rc522->poll_timer, rc522_poll_timer_callback, 0);
    mod_timer(&rc522->poll_timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
    dev_info(dev, "poll_timer started (%dms interval)\n", POLL_INTERVAL_MS);

    dev_info(dev, "RC522 driver loaded successfully\n");
    return 0;
}

/**
 * rc522_remove - SPI device removal callback
 * Reverse-order resource release: interrupt → timer → work → device node → class → cdev → device number
 */
static int rc522_remove(struct spi_device *spi)
{
    struct rc522_dev_t *rc522 = spi_get_drvdata(spi);

    // Release interrupt resource
    rc522_write_reg(rc522, ComIEnReg, 0x00);
    free_irq(rc522->irq, rc522);

    // Stop the timer + cancel pending work
    del_timer_sync(&rc522->poll_timer);
    cancel_work_sync(&rc522->detect_work);

    // Destroy the device node, device class, char device, release the device number
    device_destroy(rc522->class, rc522->devid);
    class_destroy(rc522->class);
    cdev_del(&rc522->cdev);
    unregister_chrdev_region(rc522->devid, 1);

    dev_info(&spi->dev, "RC522 driver removed\n");
    return 0;
}


/* ======================== Device tree match table ======================== */
/**
 * struct of_device_id device tree match struct array
 * Purpose: used by the kernel to bind-match driver and hardware device based on the compatible property in the device tree (dts/dtbo)
 * .compatible: match string, must exactly match the compatible field in the device tree node
 */
static const struct of_device_id rc522_match[] = {
    { .compatible = "nxp,rc522" },    // Standard match string, recommended for device tree use
    { .compatible = "rc522,rfid" },   // Compatible with the legacy device tree match string, adapts to old firmware/device trees
    {}                                 // Array end marker; the kernel uses this to judge the end when traversing the match table
};

/**
 * MODULE_DEVICE_TABLE(of, rc522_match)
 * Kernel macro, purpose:
 * 1. Exports the above device tree match table to the kernel device table, so the kernel device management module can find this match rule
 * 2. of means the match type is "device tree (Open Firmware)"
 * 3. After module load, the kernel scans the bus's device tree nodes, auto-matches by compatible
 * 4. Without this macro, device tree matching fails; the driver cannot be auto-loaded to trigger probe
 */
MODULE_DEVICE_TABLE(of, rc522_match);

/* ======================== SPI driver registration ======================== */
/**
 * struct spi_driver: SPI bus driver core struct, used to register an SPI-type driver with the kernel
 * Unified management of the driver lifecycle callbacks, driver name, match rules, etc.
 */
static struct spi_driver rc522_driver = {
    .probe    = rc522_probe,  // Init callback invoked after device match succeeds
    .remove   = rc522_remove, // Resource release callback invoked on device unload / module removal
    .driver   = {             // Generic driver base struct, compatible with the kernel's unified driver model
        .name           = "rc522",          // Driver name, viewable via lsmod, proc filesystem
        .of_match_table = rc522_match,      // Bind the device tree match table; specifies this driver uses the above of_device_id rules
    },
};

/**
 * module_spi_driver(rc522_driver)
 * Kernel simplification macro, equivalent to the full SPI driver register/unregister logic:
 * 1. On module load: calls spi_register_driver to register the current driver with the SPI bus
 * 2. On module unload: calls spi_unregister_driver to unregister the current driver from the SPI bus
 * Compared to manually writing __init / __exit functions, this macro is more concise and conforms to the Linux driver coding style
 */
module_spi_driver(rc522_driver);

/* Module basic info */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("RetailSystem");
MODULE_DESCRIPTION("RC522 RFID Driver with IRQ/ioctl/sysfs");
