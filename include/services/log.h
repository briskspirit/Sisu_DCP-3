#ifndef LOG_H
#define LOG_H

#include <stdarg.h>

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3,
} log_level_t;

void log_set_level(log_level_t level);
void log_write(log_level_t level, const char *tag, const char *fmt, ...);
void log_vwrite(log_level_t level, const char *tag, const char *fmt, va_list args);

/* Cross-core serialization for the process-wide stdio driver list. Normal log
 * writes take this internally; a lifecycle owner that adds/removes a stdio
 * driver must hold it around that mutation. Core-0-only direct console output
 * cannot overlap a lifecycle call because both run in the main loop. */
void log_output_lock(void);
void log_output_unlock(void);

#define LOGE(tag, fmt, ...) log_write(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) log_write(LOG_LEVEL_WARN, tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) log_write(LOG_LEVEL_INFO, tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) log_write(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)

#endif
