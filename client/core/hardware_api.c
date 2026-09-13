#include "hardware_api.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>

/*
 * PWM via /sys/class/pwm sysfs, no custom kernel driver.
 * RK3568 default pins: LED PWM1_CH0 -> pwmchip1 (GPIO4_C6), Motor PWM2_CH0 -> pwmchip2 (GPIO4_C2).
 */

#define LED_PWM_CHIP      0      /* PWM1 -> pwmchip1 */
#define LED_PWM_CHANNEL   0      /* channel 0 -> pwm1 */
#define LED_PWM_PERIOD_NS 1000000 /* 1ms period -> 1kHz */

/* Returns: 0 success, -1 failure */
static int pwm_export(int chip, int channel)
{
    char path[128];
    char val[16];
    int fd, len;

    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/export", chip);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        /* May already be exported; not a fatal error */
        return 0;
    }

    len = snprintf(val, sizeof(val), "%d", channel);
    if (write(fd, val, (size_t)len) != len) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void pwm_unexport(int chip, int channel)
{
    char path[128];
    char val[16];
    int fd, len;

    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/unexport", chip);
    fd = open(path, O_WRONLY);
    if (fd < 0) return;

    len = snprintf(val, sizeof(val), "%d", channel);
    write(fd, val, (size_t)len);
    close(fd);
}

static int pwm_open_attr(int chip, int channel, const char *attr)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/pwm%d/%s",
             chip, channel, attr);
    return open(path, O_RDWR);
}

int bh1750_open(void)
{
    int fd = open("/dev/bh1750", O_RDWR);
    if (fd < 0)
        perror("BH1750 open failed");
    return fd;
}

int bh1750_read_light(int fd)
{
    int light_val = 0;
    if (read(fd, &light_val, sizeof(light_val)) != sizeof(light_val))
        return -1;
    return light_val;
}

void bh1750_close(int fd)
{
    if (fd > 0)
        close(fd);
}

int bh1750_ioctl_set_mode(int fd, uint8_t mode)
{
    return ioctl(fd, BH1750_IOC_SET_MODE, mode);
}

int bh1750_ioctl_set_mtreg(int fd, uint8_t mtreg)
{
    return ioctl(fd, BH1750_IOC_SET_MTREG, mtreg);
}

int bh1750_ioctl_get_lux(int fd, int *lux)
{
    return ioctl(fd, BH1750_IOC_GET_LUX, lux);
}

int rc522_open(void)
{
    int fd = open("/dev/rc522_dev", O_RDWR | O_NONBLOCK);
    if (fd < 0)
        perror("RC522 open failed");
    return fd;
}

int rc522_get_uid(int fd, uint8_t *uid_buf)
{
    if (read(fd, uid_buf, 4) != 4)
        return -1;
    return 0;
}

void rc522_close(int fd)
{
    if (fd > 0)
        close(fd);
}

int rc522_ioctl_get_uid(int fd, uint8_t *uid_buf)
{
    return ioctl(fd, RC522_IOC_GET_UID, uid_buf);
}

int rc522_ioctl_set_antenna(int fd, int on)
{
    return ioctl(fd, RC522_IOC_SET_ANTENNA, (unsigned long)on);
}

int rc522_ioctl_get_version(int fd, uint8_t *ver)
{
    return ioctl(fd, RC522_IOC_GET_VERSION, ver);
}

int rc522_ioctl_read_block(int fd, uint8_t block, uint8_t *data)
{
    struct rc522_block_rw rw;
    rw.block = block;
    if (ioctl(fd, RC522_IOC_READ_BLOCK, &rw) != 0)
        return -1;
    for (int i = 0; i < 16; i++)
        data[i] = rw.data[i];
    return 0;
}

int rc522_ioctl_write_block(int fd, uint8_t block, const uint8_t *data)
{
    struct rc522_block_rw rw;
    rw.block = block;
    for (int i = 0; i < 16; i++)
        rw.data[i] = data[i];
    return ioctl(fd, RC522_IOC_WRITE_BLOCK, &rw);
}

/*
 * LED brightness (0~255) via /sys/class/pwm duty_cycle.
 * led_open() returns the duty_cycle file descriptor.
 */

int led_open(void)
{
    int duty_fd;

    if (pwm_export(LED_PWM_CHIP, LED_PWM_CHANNEL) != 0) {
        perror("PWM export (LED) failed");
        return -1;
    }

    usleep(1000);

    {
        int en_fd = pwm_open_attr(LED_PWM_CHIP, LED_PWM_CHANNEL, "enable");
        if (en_fd >= 0) {
            write(en_fd, "0", 1);
            close(en_fd);
        }
    }

    {
        int period_fd = pwm_open_attr(LED_PWM_CHIP, LED_PWM_CHANNEL, "period");
        if (period_fd >= 0) {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "%d", LED_PWM_PERIOD_NS);
            write(period_fd, buf, (size_t)len);
            close(period_fd);
        }
    }

    duty_fd = pwm_open_attr(LED_PWM_CHIP, LED_PWM_CHANNEL, "duty_cycle");
    if (duty_fd < 0) {
        perror("LED duty_cycle open failed");
        return -1;
    }

    return duty_fd;
}

/* brightness 0~255; 0 disables PWM, N -> duty = N * period / 255 */
int led_set(int fd, int brightness)
{
    char buf[32];
    int duty_ns, len;
    int en_fd;

    if (fd < 0) return -1;
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;

    if (brightness == 0) {
        en_fd = pwm_open_attr(LED_PWM_CHIP, LED_PWM_CHANNEL, "enable");
        if (en_fd >= 0) {
            write(en_fd, "0", 1);
            close(en_fd);
        }
        return 0;
    }

    duty_ns = brightness * LED_PWM_PERIOD_NS / 255;
    len = snprintf(buf, sizeof(buf), "%d", duty_ns);
    if (write(fd, buf, (size_t)len) != len)
        return -1;

    en_fd = pwm_open_attr(LED_PWM_CHIP, LED_PWM_CHANNEL, "enable");
    if (en_fd >= 0) {
        write(en_fd, "1", 1);
        close(en_fd);
    }

    return 0;
}

int led_get(int fd, int *brightness)
{
    char buf[32];
    ssize_t n;
    int duty_ns;

    if (fd < 0 || !brightness) return -1;

    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';

    duty_ns = atoi(buf);
    *brightness = duty_ns * 255 / LED_PWM_PERIOD_NS;
    return 0;
}

void led_close(int fd)
{
    if (fd > 0) {
        /* Disable PWM first */
        int en_fd = pwm_open_attr(LED_PWM_CHIP, LED_PWM_CHANNEL, "enable");
        if (en_fd >= 0) {
            write(en_fd, "0", 1);
            close(en_fd);
        }
        close(fd);
    }
    pwm_unexport(LED_PWM_CHIP, LED_PWM_CHANNEL);
}

/*
 * 28BYJ-48 + ULN2003 via character device /dev/motor_dev.
 * 4096 steps/rev (half-step), default 1200us/step ~ 15 RPM;
 * driver auto powers off after the stepping completes.
 */

int motor_open(void)
{
    int fd = open("/dev/motor_dev", O_RDWR);
    if (fd < 0)
        perror("Motor open failed");
    return fd;
}

/* steps > 0 forward, < 0 reverse, = 0 stop immediately (non-blocking) */
int motor_step(int fd, int steps)
{
    if (fd < 0) return -1;

    if (write(fd, &steps, sizeof(steps)) != sizeof(steps))
        return -1;

    return 0;
}

/* Legacy API: steps = duration_ms * 1000 / interval_us (default 1200us -> ~0.833 steps/ms) */
int motor_rotate(int fd, int duration_ms)
{
    int steps;

    if (fd < 0) return -1;
    if (duration_ms < 0) duration_ms = 0;
    if (duration_ms > 30000) duration_ms = 30000;

    /* Duration to steps: default 1200us/step */
    steps = (int)((long long)duration_ms * 1000 / MOTOR_DEFAULT_INTERVAL_US);
    if (steps == 0 && duration_ms > 0)
        steps = 1;  /* At least one step */

    return motor_step(fd, steps);
}

int motor_stop(int fd)
{
    if (fd < 0) return -1;
    return ioctl(fd, MOTOR_IOC_STOP);
}

/* dir: MOTOR_DIR_CW(0)=forward, MOTOR_DIR_CCW(1)=reverse */
int motor_set_direction(int fd, int dir)
{
    if (fd < 0) return -1;
    return ioctl(fd, MOTOR_IOC_SET_DIR, dir);
}

/* interval_us: microseconds/step, range 500~10000 (default 1200 = ~15 RPM) */
int motor_set_speed(int fd, int interval_us)
{
    if (fd < 0) return -1;
    return ioctl(fd, MOTOR_IOC_SET_SPEED, interval_us);
}

/* mode: MOTOR_MODE_HALF(0), MOTOR_MODE_FULL(1) */
int motor_set_mode(int fd, int mode)
{
    if (fd < 0) return -1;
    return ioctl(fd, MOTOR_IOC_SET_MODE, mode);
}

int motor_get_status(int fd, int *status)
{
    if (fd < 0 || !status) return -1;

    if (read(fd, status, sizeof(int)) != sizeof(int))
        return -1;

    return 0;
}

int motor_get_position(int fd, int *pos)
{
    if (fd < 0 || !pos) return -1;
    return ioctl(fd, MOTOR_IOC_GET_POS, pos);
}

int motor_reset_position(int fd)
{
    if (fd < 0) return -1;
    return ioctl(fd, MOTOR_IOC_RESET_POS);
}

void motor_close(int fd)
{
    if (fd > 0) {
        motor_stop(fd);
        close(fd);
    }
}

/* A4988/DRV8825 via /dev/stepper_pwm; step speed via PWM pulse frequency (unlike GPIO phase switching in motor_drv.c) */

int stepper_pwm_open(void)
{
    int fd = open("/dev/stepper_pwm", O_RDWR);
    if (fd < 0)
        perror("Stepper PWM open failed");
    return fd;
}

int stepper_pwm_step(int fd, int steps)
{
    if (fd < 0) return -1;
    if (write(fd, &steps, sizeof(steps)) != sizeof(steps))
        return -1;
    return 0;
}

int stepper_pwm_stop(int fd)
{
    if (fd < 0) return -1;
    return ioctl(fd, STEPPER_IOC_STOP);
}

int stepper_pwm_set_speed(int fd, int speed_hz)
{
    if (fd < 0) return -1;
    return ioctl(fd, STEPPER_IOC_SET_SPEED, speed_hz);
}

int stepper_pwm_set_dir(int fd, int dir)
{
    if (fd < 0) return -1;
    return ioctl(fd, STEPPER_IOC_SET_DIR, dir);
}

int stepper_pwm_get_status(int fd, int *status)
{
    if (fd < 0 || !status) return -1;
    if (read(fd, status, sizeof(int)) != sizeof(int))
        return -1;
    return 0;
}

int stepper_pwm_get_position(int fd, int *pos)
{
    if (fd < 0 || !pos) return -1;
    return ioctl(fd, STEPPER_IOC_GET_POS, pos);
}

void stepper_pwm_close(int fd)
{
    if (fd > 0) {
        stepper_pwm_stop(fd);
        close(fd);
    }
}

int dma_spi_open(void)
{
    int fd = open("/dev/dma_spi", O_RDWR);
    if (fd < 0)
        perror("DMA SPI open failed");
    return fd;
}

int dma_spi_write(int fd, const uint8_t *data, int len)
{
    ssize_t ret;
    if (fd < 0 || !data || len <= 0) return -1;
    ret = write(fd, data, (size_t)len);
    return (ret >= 0) ? (int)ret : -1;
}

int dma_spi_read(int fd, uint8_t *buf, int len)
{
    ssize_t ret;
    if (fd < 0 || !buf || len <= 0) return -1;
    ret = read(fd, buf, (size_t)len);
    return (ret >= 0) ? (int)ret : -1;
}

int dma_spi_ioctl_get_stats(int fd, struct dma_spi_stats *stats)
{
    if (fd < 0 || !stats) return -1;
    return ioctl(fd, DMA_SPI_IOC_GET_STATS, stats);
}

int dma_spi_ioctl_reset_stats(int fd)
{
    if (fd < 0) return -1;
    return ioctl(fd, DMA_SPI_IOC_RESET_STATS);
}

int dma_spi_ioctl_set_speed(int fd, uint32_t speed_hz)
{
    if (fd < 0) return -1;
    return ioctl(fd, DMA_SPI_IOC_SET_SPEED, speed_hz);
}

void dma_spi_close(int fd)
{
    if (fd > 0)
        close(fd);
}
