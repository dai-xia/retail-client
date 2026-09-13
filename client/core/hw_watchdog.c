/*
 * hw_watchdog.c - Linux hardware watchdog (/dev/watchdog)
 */

#include "hw_watchdog.h"
#include "logger.h"
#include <errno.h>

static hw_watchdog_t *g_hw_watchdog = NULL;

static void *hw_watchdog_thread(void *arg)
{
    hw_watchdog_t *hw = (hw_watchdog_t *)arg;

    while (hw->running) {
        if (write(hw->fd, "", 1) != 1) {
            LOGE("Hardware watchdog feed failed: %s", strerror(errno));
        }
        sleep(hw->interval / 2);
    }

    return NULL;
}

hw_watchdog_t* hw_watchdog_create(int interval_sec)
{
    hw_watchdog_t *hw = calloc(1, sizeof(hw_watchdog_t));
    if (!hw) return NULL;

    hw->fd = open(HW_WATCHDOG_DEVICE, O_RDWR);
    if (hw->fd < 0) {
        LOGW("Cannot open hardware watchdog %s: %s (device may not exist, skipping)",
             HW_WATCHDOG_DEVICE, strerror(errno));
        free(hw);
        return NULL;
    }

    hw->interval = interval_sec > 0 ? interval_sec : HW_WATCHDOG_INTERVAL;
    ioctl(hw->fd, WDIOC_SETTIMEOUT, &hw->interval);

    g_hw_watchdog = hw;
    LOGI("Hardware watchdog opened, timeout=%ds", hw->interval);
    return hw;
}

int hw_watchdog_start(hw_watchdog_t *hw)
{
    if (!hw || hw->running) return -1;

    /* The kernel starts the countdown when open is called; feed the dog once first to avoid immediate timeout */
    write(hw->fd, "", 1);

    hw->running = 1;
    if (pthread_create(&hw->thread, NULL, hw_watchdog_thread, hw) != 0) {
        hw->running = 0;
        return -1;
    }

    LOGI("Hardware watchdog started");
    return 0;
}

/* Normal stop: write "V" (notify kernel of clean exit) + close + join thread */
void hw_watchdog_stop(hw_watchdog_t *hw)
{
    if (!hw || !hw->running) return;

    hw->running = 0;

    if (hw->fd >= 0) {
        write(hw->fd, "V", 1);
        close(hw->fd);
        hw->fd = -1;
    }

    pthread_join(hw->thread, NULL);
}

void hw_watchdog_destroy(hw_watchdog_t *hw)
{
    if (!hw) return;
    hw_watchdog_stop(hw);
    if (g_hw_watchdog == hw) g_hw_watchdog = NULL;
    free(hw);
}

/*
 * Emergency disable: called in signal handler / before _exit
 * Cannot call pthread_join (not async-signal-safe),
 * directly write V + close + set flag; thread is reclaimed by _exit
 */
void hw_watchdog_emergency_disable(void)
{
    hw_watchdog_t *hw = g_hw_watchdog;
    if (!hw || hw->fd < 0) return;

    hw->running = 0;
    write(hw->fd, "V", 1);
    close(hw->fd);
    hw->fd = -1;
}
