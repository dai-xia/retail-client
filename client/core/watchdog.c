#include "watchdog.h"
#include "logger.h"
#include "hw_watchdog.h"
#include <sys/stat.h>
#include <execinfo.h>
#include <fcntl.h>
#include <errno.h>

// Global watchdog instance: crash handlers cannot take user arguments.
static watchdog_t *g_active_watchdog = NULL;

static void crash_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    watchdog_t *wd = g_active_watchdog;

    if (wd) {
        __sync_fetch_and_add(&wd->crash_count, 1);

        char path[WATCHDOG_DUMP_PATH_MAX];
        snprintf(path, sizeof(path), "%s/crash_%d_%ld.dump",
                 wd->dump_dir, getpid(), (long)time(NULL));

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char header[512];
            int n = snprintf(header, sizeof(header),
                             "=== CRASH DUMP ===\n"
                             "Signal: %d (%s)\n"
                             "PID: %d\n"
                             "Address: %p\n"
                             "Time: %ld\n"
                             "CrashCount: %d\n"
                             "=== BACKTRACE ===\n",
                             sig, strsignal(sig), getpid(),
                             si ? si->si_addr : NULL,
                             (long)time(NULL), wd->crash_count);
            write(fd, header, n);

            void *frames[64];
            int frame_count = backtrace(frames, 64);
            backtrace_symbols_fd(frames, frame_count, fd);

            char footer[] = "\n=== END ===\n";
            write(fd, footer, sizeof(footer) - 1);
            close(fd);
        }

        snprintf(path, sizeof(path), "%s/crash_history", wd->dump_dir);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%ld\n", (long)time(NULL));
            write(fd, buf, n);
            close(fd);
        }

        logger_flush(logger_get_default());
    }

    hw_watchdog_emergency_disable();
    _exit(128 + sig);
}



int watchdog_install_signal_handlers(watchdog_t *wd)
{
    if (!wd) return -1;
    g_active_watchdog = wd;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;  // detailed info + reset default after first trigger
    sigemptyset(&sa.sa_mask);

    int crash_signals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL, SIGTRAP};
    for (size_t i = 0; i < sizeof(crash_signals) / sizeof(crash_signals[0]); i++) {
        if (sigaction(crash_signals[i], &sa, NULL))
            return -1;
    }

    signal(SIGPIPE, SIG_IGN);  // ignore broken pipe so writing to a closed socket won't crash
    return 0;
}


int watchdog_count_crashes_since(const char *dump_dir, time_t since)
{
    if (!dump_dir) return 0;

    char path[WATCHDOG_DUMP_PATH_MAX];
    snprintf(path, sizeof(path), "%s/crash_history", dump_dir);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    buf[n] = '\0';

    int count = 0;
    char *line = strtok(buf, "\n");
    while (line) {
        time_t t = (time_t)atol(line);
        if (t > since) {
            count++;
        }
        line = strtok(NULL, "\n");
    }

    return count;
}

static void *watchdog_thread(void *arg)
{
    watchdog_t *wd = (watchdog_t *)arg;

    while (wd->running) {
        sleep(wd->feed_interval);

        pthread_mutex_lock(&wd->mtx);
        time_t now = time(NULL);
        int elapsed = (int)(now - wd->last_feed);
        pthread_mutex_unlock(&wd->mtx);

        if (elapsed > wd->timeout_sec) {
            LOGE("watchdog timeout: main thread did not feed for %d s, exiting", elapsed);

            logger_flush(logger_get_default());
            hw_watchdog_emergency_disable();
            _exit(1);
        }
    }

    return NULL;
}

watchdog_t* watchdog_create(int timeout_sec, int feed_interval)
{
    watchdog_t *wd = calloc(1, sizeof(watchdog_t));
    if (!wd) return NULL;

    wd->timeout_sec = timeout_sec > 0 ? timeout_sec : WATCHDOG_TIMEOUT_SEC;
    wd->feed_interval = feed_interval > 0 ? feed_interval : WATCHDOG_FEED_INTERVAL;
    wd->last_feed = time(NULL);

    strncpy(wd->dump_dir, "/tmp", sizeof(wd->dump_dir) - 1);
    mkdir(wd->dump_dir, 0755);

    pthread_mutex_init(&wd->mtx, NULL);

    return wd;
}

void watchdog_destroy(watchdog_t *wd)
{
    if (!wd) return;
    watchdog_stop(wd);
    pthread_mutex_destroy(&wd->mtx);
    if (g_active_watchdog == wd) g_active_watchdog = NULL;
    free(wd);
}

int watchdog_start(watchdog_t *wd)
{
    if (!wd || wd->running) return -1;

    wd->running = 1;
    wd->last_feed = time(NULL);

    if (pthread_create(&wd->watchdog_tid, NULL, watchdog_thread, wd) != 0) {
        wd->running = 0;
        return -1;
    }

    return 0;
}

void watchdog_stop(watchdog_t *wd)
{
    if (!wd || !wd->running) return;
    wd->running = 0;
    pthread_join(wd->watchdog_tid, NULL);
}

void watchdog_feed(watchdog_t *wd)
{
    if (!wd) return;
    pthread_mutex_lock(&wd->mtx);
    wd->last_feed = time(NULL);
    pthread_mutex_unlock(&wd->mtx);
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[WATCHDOG_DUMP_PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

void watchdog_set_dump_dir(watchdog_t *wd, const char *dir)
{
    if (wd && dir) {
        strncpy(wd->dump_dir, dir, sizeof(wd->dump_dir) - 1);
        wd->dump_dir[sizeof(wd->dump_dir) - 1] = '\0';
        if (mkdir_p(wd->dump_dir, 0755) != 0) {
            LOGE("cannot create dump dir %s: %s", wd->dump_dir, strerror(errno));
        }
    }
}

int watchdog_get_crash_count(watchdog_t *wd)
{
    return wd ? wd->crash_count : 0;
}


int watchdog_check_startup_safety(const char *dump_dir)
{
    if (!dump_dir) return 0;

    char path[WATCHDOG_DUMP_PATH_MAX];
    snprintf(path, sizeof(path), "%s/crash_history", dump_dir);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    buf[n] = '\0';

    time_t now = time(NULL);
    time_t cutoff = now - WATCHDOG_CRASH_WINDOW_SEC;  // window start; older crashes ignored
    int recent_crashes = 0;
    time_t latest = 0;

    char *line = strtok(buf, "\n");
    while (line) {
        time_t t = (time_t)atol(line);
        if (t > cutoff) {
            recent_crashes++;
            if (t > latest) latest = t;
        }
        line = strtok(NULL, "\n");
    }

    if (recent_crashes >= WATCHDOG_MAX_CRASH_COUNT) {
        LOGE("startup fuse: %d crashes within %d s (threshold %d), refusing to start",
             recent_crashes, WATCHDOG_CRASH_WINDOW_SEC, WATCHDOG_MAX_CRASH_COUNT);
        return -1;
    }

    if (recent_crashes > 0) {
        LOGW("crash warning: %d crashes within %d s, last one %ld s ago",
             recent_crashes, WATCHDOG_CRASH_WINDOW_SEC, (long)(now - latest));
    }

    return 0;
}
