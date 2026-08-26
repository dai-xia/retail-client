#ifndef __WATCHDOG_H
#define __WATCHDOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WATCHDOG_TIMEOUT_SEC   30
#define WATCHDOG_FEED_INTERVAL 5
#define WATCHDOG_DUMP_PATH_MAX 256

#define WATCHDOG_CRASH_WINDOW_SEC  300
#define WATCHDOG_MAX_CRASH_COUNT   5

// ==================== Watchdog core struct ====================
/**
 * @brief Watchdog instance
 * Each program can create one watchdog instance holding all its state.
 */
typedef struct {
    int timeout_sec;        // timeout threshold (s); reboot if not fed within this window
    int feed_interval;      // auto-feed interval (s)
    int running;            // 1=running, 0=stopped
    int crash_count;        // crash count (atomic, thread-safe)
    time_t last_feed;       // last feed timestamp (for timeout check)

    char dump_dir[WATCHDOG_DUMP_PATH_MAX];  // crash dump dir
    pthread_t watchdog_tid;                 // monitor thread id
    pthread_mutex_t mtx;                    // protects cross-thread access
} watchdog_t;


// ==================== Public API ====================
watchdog_t* watchdog_create(int timeout_sec, int feed_interval);
void watchdog_destroy(watchdog_t *wd);
int watchdog_start(watchdog_t *wd);
void watchdog_stop(watchdog_t *wd);

void watchdog_feed(watchdog_t *wd);

void watchdog_set_dump_dir(watchdog_t *wd, const char *dir);

int watchdog_install_signal_handlers(watchdog_t *wd);

int watchdog_get_crash_count(watchdog_t *wd);

int watchdog_check_startup_safety(const char *dump_dir);

int watchdog_count_crashes_since(const char *dump_dir, time_t since);

#ifdef __cplusplus
}
#endif

#endif
