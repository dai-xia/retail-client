#ifndef __HW_WATCHDOG_H
#define __HW_WATCHDOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HW_WATCHDOG_DEVICE   "/dev/watchdog"
#define HW_WATCHDOG_INTERVAL 60

typedef struct hw_watchdog_s {
    int fd;
    int running;
    int interval;
    pthread_t thread;
} hw_watchdog_t;

hw_watchdog_t* hw_watchdog_create(int interval_sec);
void hw_watchdog_destroy(hw_watchdog_t *hw);
int hw_watchdog_start(hw_watchdog_t *hw);
void hw_watchdog_stop(hw_watchdog_t *hw);

/* Emergency disable of the hardware watchdog; call before _exit */
void hw_watchdog_emergency_disable(void);

#ifdef __cplusplus
}
#endif

#endif
