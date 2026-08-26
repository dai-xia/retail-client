#include "logger.h"
#include <sys/stat.h>
#include <syslog.h>
#include <errno.h>

static logger_t *g_default_logger = NULL;

static int ring_init(log_ring_t *r, int size)
{
    int real_size = 1;
    while (real_size < size) real_size <<= 1;

    r->ring = calloc(real_size, sizeof(log_entry_t));
    if (!r->ring) return -1;

    r->ready = calloc(real_size, sizeof(int));
    if (!r->ready) {
        free(r->ring);
        r->ring = NULL;
        return -1;
    }

    r->size = real_size;
    r->mask = real_size - 1;
    r->head = 0;
    r->tail = 0;
    pthread_mutex_init(&r->pop_mtx, NULL);
    return 0;
}

static void ring_destroy(log_ring_t *r)
{
    free(r->ring);
    r->ring = NULL;
    free(r->ready);
    r->ready = NULL;
    pthread_mutex_destroy(&r->pop_mtx);
}

static int ring_push(log_ring_t *r, const log_entry_t *entry)
{
    int head, next;

    do {
        head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        next = (head + 1) & r->mask;
        if (next == __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE))
            return -1;
    } while (!__atomic_compare_exchange_n(&r->head, &head, next,
                                          0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));

    memcpy(&r->ring[head], entry, sizeof(log_entry_t));
    __atomic_store_n(&r->ready[head], 1, __ATOMIC_RELEASE);
    return 0;
}


static int ring_pop(log_ring_t *r, log_entry_t *entry)
{
    pthread_mutex_lock(&r->pop_mtx);

    int tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if (tail == __atomic_load_n(&r->head, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&r->pop_mtx);
        return -1;
    }

    if (__atomic_load_n(&r->ready[tail], __ATOMIC_ACQUIRE) != 1) {
        pthread_mutex_unlock(&r->pop_mtx);
        return -1;
    }

    memcpy(entry, &r->ring[tail], sizeof(log_entry_t));
    __atomic_store_n(&r->ready[tail], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&r->tail, (tail + 1) & r->mask, __ATOMIC_RELEASE);

    pthread_mutex_unlock(&r->pop_mtx);
    return 0;
}


static int ring_empty(const log_ring_t *r)
{
    return __atomic_load_n(&r->tail, __ATOMIC_RELAXED)
        == __atomic_load_n(&r->head, __ATOMIC_RELAXED);
}

static const char *level_str(int level)
{
    switch (level) {
    case LOG_LEVEL_DEBUG: return "DEBUG";
    case LOG_LEVEL_INFO:  return "INFO ";
    case LOG_LEVEL_WARN:  return "WARN ";
    case LOG_LEVEL_ERROR: return "ERROR";
    default:              return "?????";
    }
}

static int syslog_level(int level)
{
    switch (level) {
    case LOG_LEVEL_DEBUG: return LOG_DEBUG;
    case LOG_LEVEL_INFO:  return LOG_INFO;
    case LOG_LEVEL_WARN:  return LOG_WARNING;
    case LOG_LEVEL_ERROR: return LOG_ERR;
    default:              return LOG_INFO;
    }
}

static int rotate_needed(logger_t *log)
{
    if (log->bytes_written >= (size_t)log->max_file_size) return 1;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    tm_now.tm_hour = 0;
    tm_now.tm_min = 0;
    tm_now.tm_sec = 0;
    time_t today = mktime(&tm_now);

    if (today != log->today_start) return 1;
    return 0;
}

static int open_log_file(logger_t *log)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    tm_now.tm_hour = 0;
    tm_now.tm_min = 0;
    tm_now.tm_sec = 0;
    log->today_start = mktime(&tm_now);

    char path[LOG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s_%04d%02d%02d.log",
             log->log_dir, log->log_file,
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);

    if (log->fp) fclose(log->fp);

    log->fp = fopen(path, "a");
    if (!log->fp) {
        fprintf(stderr, "logger: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    strncpy(log->current_path, path, sizeof(log->current_path) - 1);
    log->bytes_written = 0;

    setvbuf(log->fp, NULL, _IOFBF, 65536);
    return 0;
}

static void write_entry_to_file(logger_t *log, const log_entry_t *entry)
{
    if (!log->fp || rotate_needed(log)) {
        if (open_log_file(log) != 0) return;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_val;
    localtime_r(&tv.tv_sec, &tm_val);

    char time_buf[32];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d.%03ld",
             tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec,
             tv.tv_usec / 1000);

    fprintf(log->fp, "[%s][%s][%d] %s\n",
            time_buf, level_str(entry->level), getpid(), entry->msg);
    log->bytes_written += strlen(entry->msg) + 64;
}

static void *flush_thread(void *arg)
{
    logger_t *log = (logger_t *)arg;
    log_entry_t entry;

    while (__atomic_load_n(&log->running, __ATOMIC_ACQUIRE)) {
        int flushed = 0;
        for (int i = 0; i < 256 && !ring_empty(&log->ring); i++) {
            if (ring_pop(&log->ring, &entry) == 0) {
                pthread_mutex_lock(&log->file_mtx);
                write_entry_to_file(log, &entry);
                pthread_mutex_unlock(&log->file_mtx);
                flushed++;
            }
        }

        if (flushed == 0) {
            struct timespec ts;
            ts.tv_sec = log->flush_interval;
            ts.tv_nsec = 0;
            nanosleep(&ts, NULL);
        }
    }

    while (!ring_empty(&log->ring)) {
        if (ring_pop(&log->ring, &entry) == 0) {
            pthread_mutex_lock(&log->file_mtx);
            write_entry_to_file(log, &entry);
            pthread_mutex_unlock(&log->file_mtx);
        }
    }

    return NULL;
}

logger_t* logger_create(const char *log_dir, const char *log_file)
{
    logger_t *log = calloc(1, sizeof(logger_t));
    if (!log) return NULL;

    strncpy(log->log_dir, log_dir, sizeof(log->log_dir) - 1);
    strncpy(log->log_file, log_file, sizeof(log->log_file) - 1);
    log->min_level = LOG_LEVEL_DEBUG;
    log->max_file_size = LOG_MAX_FILE_SIZE;
    log->flush_interval = LOG_FLUSH_INTERVAL;
    log->use_console = 1;
    log->use_syslog = 0;

    mkdir(log_dir, 0755);

    if (ring_init(&log->ring, LOG_RING_SIZE) != 0) {
        free(log);
        return NULL;
    }

    pthread_mutex_init(&log->file_mtx, NULL);

    if (open_log_file(log) != 0) {
        ring_destroy(&log->ring);
        pthread_mutex_destroy(&log->file_mtx);
        free(log);
        return NULL;
    }

    return log;
}

void logger_destroy(logger_t *log)
{
    if (!log) return;
    logger_stop(log);
    ring_destroy(&log->ring);
    pthread_mutex_destroy(&log->file_mtx);
    if (log->fp) fclose(log->fp);
    if (log->use_syslog) closelog();
    free(log);
}

int logger_start(logger_t *log)
{
    if (!log) return -1;
    if (log->running) return 0;

    log->running = 1;

    if (log->use_syslog) {
        openlog(log->log_file, LOG_PID | LOG_CONS, LOG_USER);
    }

    if (pthread_create(&log->flush_tid, NULL, flush_thread, log) != 0) {
        log->running = 0;
        return -1;
    }

    return 0;
}

void logger_stop(logger_t *log)
{
    if (!log || !log->running) return;
    log->running = 0;
    pthread_join(log->flush_tid, NULL);
}

void logger_set_level(logger_t *log, log_level_t level)
{
    if (log) log->min_level = level;
}

void logger_set_console(logger_t *log, int enable)
{
    if (log) log->use_console = enable;
}

void logger_set_syslog(logger_t *log, int enable)
{
    if (log) log->use_syslog = enable;
}

void logger_set_remote_cb(logger_t *log, log_remote_cb_t cb)
{
    if (log) log->remote_cb = cb;
}

void logger_write(logger_t *log, int level, const char *file, int line,
                  const char *func, const char *fmt, ...)
{
    if (!log || level < log->min_level) return;

    const char *filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    char body[LOG_BUF_SIZE - 128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    log_entry_t entry;
    entry.level = level;
    entry.len = snprintf(entry.msg, sizeof(entry.msg),
                         "%s:%d %s() %s", filename, line, func, body);

    if (log->use_console) {
        fprintf(stderr, "[%s][%s] %s\n", level_str(level), filename, body);
        fflush(stderr);
    }

    if (log->use_syslog) {
        syslog(syslog_level(level), "%s:%d %s", filename, line, body);
    }

    if (level >= LOG_LEVEL_ERROR && log->remote_cb) {
        log->remote_cb(level, entry.msg, entry.len);
    }

    ring_push(&log->ring, &entry);
}

void logger_flush(logger_t *log)
{
    if (!log) return;

    log_entry_t entry;
    /* Use trylock to avoid deadlock in signal handlers: if a crash happens
     * while logger_write holds file_mtx, lock would deadlock; trylock skips
     * it and pops remaining data directly. */
    int locked = (pthread_mutex_trylock(&log->file_mtx) == 0);
    while (ring_pop(&log->ring, &entry) == 0) {
        write_entry_to_file(log, &entry);
    }
    if (log->fp) fflush(log->fp);
    if (locked) pthread_mutex_unlock(&log->file_mtx);
}

logger_t* logger_get_default(void)
{
    return __atomic_load_n(&g_default_logger, __ATOMIC_ACQUIRE);
}

void logger_set_default(logger_t *log)
{
    __atomic_store_n(&g_default_logger, log, __ATOMIC_RELEASE);
}
