#ifndef __LOGGER_H
#define __LOGGER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_BUF_SIZE        4096
#define LOG_RING_SIZE       8192
#define LOG_MAX_FILE_SIZE   (10 * 1024 * 1024)
#define LOG_FLUSH_INTERVAL  2
#define LOG_PATH_MAX        256

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_NONE  = 4
} log_level_t;

typedef struct {
    int level;
    char msg[LOG_BUF_SIZE];
    int len;
} log_entry_t;

typedef struct {
    log_entry_t *ring;
    int *ready;
    int head;
    int tail;
    int size;
    int mask;
    pthread_mutex_t pop_mtx;
} log_ring_t;

typedef void (*log_remote_cb_t)(int level, const char *msg, int len);

typedef struct {
    char log_dir[LOG_PATH_MAX];
    char log_file[LOG_PATH_MAX];
    char current_path[LOG_PATH_MAX];
    log_level_t min_level;
    int max_file_size;
    int flush_interval;
    int use_syslog;
    int use_console;
    int running;

    log_ring_t ring;
    pthread_t flush_tid;

    pthread_mutex_t file_mtx;
    FILE *fp;
    time_t today_start;
    size_t bytes_written;

    log_remote_cb_t remote_cb;
} logger_t;

logger_t* logger_create(const char *log_dir, const char *log_file);
void logger_destroy(logger_t *log);
int logger_start(logger_t *log);
void logger_stop(logger_t *log);

void logger_set_level(logger_t *log, log_level_t level);
void logger_set_console(logger_t *log, int enable);
void logger_set_syslog(logger_t *log, int enable);
void logger_set_remote_cb(logger_t *log, log_remote_cb_t cb);

void logger_write(logger_t *log, int level, const char *file, int line,
                  const char *func, const char *fmt, ...)
                  __attribute__((format(printf, 6, 7)));

void logger_flush(logger_t *log);

logger_t* logger_get_default(void);
void logger_set_default(logger_t *log);

#define LOG_DEBUG(log, fmt, ...) \
    logger_write(log, LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_INFO(log, fmt, ...) \
    logger_write(log, LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_WARN(log, fmt, ...) \
    logger_write(log, LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(log, fmt, ...) \
    logger_write(log, LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOGD(fmt, ...)  LOG_DEBUG(logger_get_default(), fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...)  LOG_INFO(logger_get_default(), fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...)  LOG_WARN(logger_get_default(), fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...)  LOG_ERROR(logger_get_default(), fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif

