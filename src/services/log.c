#include "services/log.h"

#include "services/timebase.h"
#include "pico/mutex.h"
#include "pico/stdlib.h"

#include <stdio.h>

static log_level_t s_log_level = LOG_LEVEL_INFO;
auto_init_mutex(s_log_output_mutex);

void log_set_level(log_level_t level) {
    s_log_level = level;
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_vwrite(level, tag, fmt, args);
    va_end(args);
}

void log_vwrite(log_level_t level, const char *tag, const char *fmt, va_list args) {
    if (level > s_log_level) {
        return;
    }
    log_output_lock();
    static const char *const names[] = {"E", "W", "I", "D"};
    const char *name = "?";
    if ((unsigned)level < (sizeof(names) / sizeof(names[0]))) {
        name = names[level];
    }
    printf("[%08lu][c%u][%s][%s] ",
           (unsigned long)time_ms(),
           (unsigned)get_core_num(),
           name,
           tag ? tag : "-");
    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);
    log_output_unlock();
}

void log_output_lock(void) {
    mutex_enter_blocking(&s_log_output_mutex);
}

void log_output_unlock(void) {
    mutex_exit(&s_log_output_mutex);
}
