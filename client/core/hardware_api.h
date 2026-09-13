#ifndef __HARDWARE_API_H
#define __HARDWARE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int bh1750_open(void);
int bh1750_read_light(int fd);
void bh1750_close(int fd);

/* BH1750 ioctl interface */
#define BH1750_IOC_MAGIC        'B'               //ioctl magic
#define BH1750_IOC_SET_MODE     _IOW(BH1750_IOC_MAGIC, 1, uint8_t)
#define BH1750_IOC_SET_MTREG    _IOW(BH1750_IOC_MAGIC, 2, uint8_t)
#define BH1750_IOC_GET_LUX      _IOR(BH1750_IOC_MAGIC, 3, int)

int bh1750_ioctl_set_mode(int fd, uint8_t mode);
int bh1750_ioctl_set_mtreg(int fd, uint8_t mtreg);
int bh1750_ioctl_get_lux(int fd, int *lux);

int rc522_open(void);
int rc522_get_uid(int fd, uint8_t *uid_buf);
void rc522_close(int fd);

/* RC522 ioctl interface */
#define RC522_IOC_MAGIC         'R'
#define RC522_IOC_GET_UID       _IOR(RC522_IOC_MAGIC, 1, uint8_t[4])
#define RC522_IOC_SET_ANTENNA   _IOW(RC522_IOC_MAGIC, 2, int)
#define RC522_IOC_GET_VERSION   _IOR(RC522_IOC_MAGIC, 3, uint8_t)

struct rc522_auth_info {
    uint8_t  key[6];
    uint8_t  key_type;
    uint8_t  block;
};

struct rc522_block_rw {
    uint8_t  block;
    uint8_t  data[16];
};

#define RC522_IOC_AUTH          _IOW(RC522_IOC_MAGIC, 4, struct rc522_auth_info)
#define RC522_IOC_READ_BLOCK    _IOWR(RC522_IOC_MAGIC, 5, struct rc522_block_rw)
#define RC522_IOC_WRITE_BLOCK   _IOW(RC522_IOC_MAGIC, 6, struct rc522_block_rw)

int rc522_ioctl_get_uid(int fd, uint8_t *uid_buf);
int rc522_ioctl_set_antenna(int fd, int on);
int rc522_ioctl_get_version(int fd, uint8_t *ver);

/*
 * MIFARE read/write block - must authenticate first
 *
 * Correct flow (driver no longer auto-searches cards, to preserve auth state):
 *   1. ioctl(fd, RC522_IOC_GET_UID, uid)      - select card + activate card
 *   2. ioctl(fd, RC522_IOC_AUTH, &auth)       - authenticate target sector
 *   3. ioctl(fd, RC522_IOC_READ_BLOCK, &rw)   - read block
 *
 * All three steps require the card not to move; otherwise the auth state is lost and you must restart from step 1.
 */
int rc522_ioctl_read_block(int fd, uint8_t block, uint8_t *data);
int rc522_ioctl_write_block(int fd, uint8_t block, const uint8_t *data);

/* PWM pin configurable via LED_PWM_CHIP / LED_PWM_CHANNEL in hardware_api.c */
#define LED_PWM_BRIGHTNESS_MAX 255

int led_open(void);
int led_set(int fd, int brightness);          /* 0~255 -> 0%~100% duty cycle */
int led_get(int fd, int *brightness);
void led_close(int fd);

/*
 * 28BYJ-48 + ULN2003 via /dev/motor_dev: write(int steps) (non-blocking), ioctl for dir/speed/mode.
 * 4096 steps/rev (half-step), 2048 (full-step), recommended ~15 RPM.
 */
#define MOTOR_IOC_MAGIC     'M'                              /* ioctl magic */
#define MOTOR_IOC_SET_DIR   _IOW(MOTOR_IOC_MAGIC, 1, int)    /* 0=CW, 1=CCW */
#define MOTOR_IOC_SET_SPEED _IOW(MOTOR_IOC_MAGIC, 2, int)    /* step interval in us */
#define MOTOR_IOC_SET_MODE  _IOW(MOTOR_IOC_MAGIC, 3, int)    /* 0=half-step, 1=full-step */
#define MOTOR_IOC_GET_DIR   _IOR(MOTOR_IOC_MAGIC, 4, int)    /* current direction */
#define MOTOR_IOC_GET_SPEED _IOR(MOTOR_IOC_MAGIC, 5, int)    /* current step interval (us) */
#define MOTOR_IOC_GET_MODE  _IOR(MOTOR_IOC_MAGIC, 6, int)    /* current drive mode */
#define MOTOR_IOC_STOP      _IO(MOTOR_IOC_MAGIC, 7)          /* stop immediately */
#define MOTOR_IOC_GET_POS   _IOR(MOTOR_IOC_MAGIC, 8, int)    /* accumulated steps */
#define MOTOR_IOC_RESET_POS _IO(MOTOR_IOC_MAGIC, 9)          /* reset accumulated steps to zero */
/* Direction constants */
#define MOTOR_DIR_CW    0   /* Forward */
#define MOTOR_DIR_CCW   1   /* Reverse */

/* Drive mode */
#define MOTOR_MODE_HALF 0   /* Half-step (8 phases, 4096 steps/rev) */
#define MOTOR_MODE_FULL 1   /* Full-step (4 phases, 2048 steps/rev) */

/* Step count constants (half-step mode) */
#define MOTOR_STEPS_PER_REV  4096  /* 28BYJ-48 half-step steps per revolution */
#define MOTOR_STEPS_QUARTER  1024  /* 90 degrees */
#define MOTOR_STEPS_HALF_REV 2048  /* 180 degrees */

/* Default step interval (microseconds) */
#define MOTOR_DEFAULT_INTERVAL_US 1200  /* ~15 RPM */

int  motor_open(void);
int  motor_step(int fd, int steps);            /* Rotate by specified steps, non-blocking */
int  motor_rotate(int fd, int duration_ms);    /* Rotate for specified milliseconds (legacy API) */
int  motor_stop(int fd);                       /* Stop immediately */
int  motor_set_direction(int fd, int dir);     /* Set direction */
int  motor_set_speed(int fd, int interval_us); /* Set step interval */
int  motor_set_mode(int fd, int mode);         /* Set drive mode */
int  motor_get_status(int fd, int *status);    /* 0=idle, 1=running */
int  motor_get_position(int fd, int *pos);     /* accumulated steps */
int  motor_reset_position(int fd);             /* reset accumulated steps to zero */
void motor_close(int fd);

/*
 * PWM stepper driver (motor_drv_pwm.c) via /dev/stepper_pwm:
 * write(int steps) (non-blocking), ioctl for speed/direction/position.
 * Unlike motor_dev: A4988/DRV8825 (PWM pulse + DIR GPIO) instead of GPIO phase switching.
 */
#define STEPPER_IOC_MAGIC      'S'
#define STEPPER_IOC_SET_SPEED  _IOW(STEPPER_IOC_MAGIC, 1, int)   /* step frequency (Hz) */
#define STEPPER_IOC_GET_SPEED  _IOR(STEPPER_IOC_MAGIC, 2, int)   /* current step frequency (Hz) */
#define STEPPER_IOC_SET_DIR    _IOW(STEPPER_IOC_MAGIC, 3, int)   /* 0=CW, 1=CCW */
#define STEPPER_IOC_GET_DIR    _IOR(STEPPER_IOC_MAGIC, 4, int)   /* current direction */
#define STEPPER_IOC_STOP       _IO(STEPPER_IOC_MAGIC, 5)         /* stop immediately */
#define STEPPER_IOC_GET_POS    _IOR(STEPPER_IOC_MAGIC, 6, int)   /* accumulated steps */
#define STEPPER_IOC_RESET_POS  _IO(STEPPER_IOC_MAGIC, 7)         /* reset accumulated steps to zero */

int  stepper_pwm_open(void);
int  stepper_pwm_step(int fd, int steps);
int  stepper_pwm_stop(int fd);
int  stepper_pwm_set_speed(int fd, int speed_hz);
int  stepper_pwm_set_dir(int fd, int dir);
int  stepper_pwm_get_status(int fd, int *status);
int  stepper_pwm_get_position(int fd, int *pos);
void stepper_pwm_close(int fd);

/* DMA SPI driver (dma_spi_drv.c): large-block SPI transfers via /dev/dma_spi */
#define DMA_SPI_IOC_MAGIC       'D'
#define DMA_SPI_IOC_GET_STATS   _IOR(DMA_SPI_IOC_MAGIC, 1, struct dma_spi_stats)
#define DMA_SPI_IOC_RESET_STATS _IO(DMA_SPI_IOC_MAGIC, 2)
#define DMA_SPI_IOC_SET_SPEED   _IOW(DMA_SPI_IOC_MAGIC, 3, uint32_t)
#define DMA_SPI_IOC_GET_SPEED   _IOR(DMA_SPI_IOC_MAGIC, 4, uint32_t)

struct dma_spi_stats {
    uint32_t transfer_count;      /* Total transfer count */
    uint32_t bytes_transferred;   /* Total bytes transferred */
    uint32_t dma_errors;          /* DMA error count */
    uint32_t avg_transfer_us;     /* Average transfer time (us) */
};

int  dma_spi_open(void);
int  dma_spi_write(int fd, const uint8_t *data, int len);
int  dma_spi_read(int fd, uint8_t *buf, int len);
int  dma_spi_ioctl_get_stats(int fd, struct dma_spi_stats *stats);
int  dma_spi_ioctl_reset_stats(int fd);
int  dma_spi_ioctl_set_speed(int fd, uint32_t speed_hz);
void dma_spi_close(int fd);

#ifdef __cplusplus
}
#endif

#endif
