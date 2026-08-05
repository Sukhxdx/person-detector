#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "utils/logger.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

static const char *level_strings[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

static log_level_t g_min_level = LOG_LEVEL_INFO;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_fp = NULL;

void logger_init(log_level_t min_level)
{
    g_min_level = min_level;
    g_log_fp = stderr;
}

void logger_set_level(log_level_t min_level)
{
    g_min_level = min_level;
}

void logger_shutdown(void)
{
    pthread_mutex_lock(&g_log_mutex);
    g_log_fp = NULL;
    pthread_mutex_unlock(&g_log_mutex);
}

void log_write(log_level_t level, const char *file, int line, const char *fmt, ...)
{
    if (level < g_min_level) {
        return;
    }

    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_buf;
    struct tm *tm_info = localtime_r(&ts.tv_sec, &tm_buf);
    char time_str[32];
    if (tm_info != NULL) {
        (void)strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        (void)snprintf(time_str, sizeof(time_str), "unknown");
    }

    const char *base = strrchr(file, '/');
    base = (base != NULL) ? base + 1 : file;

    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fp != NULL) {
        (void)fprintf(g_log_fp, "[%s.%03ld] [%s] %s:%d: ",
                      time_str,
                      ts.tv_nsec / 1000000L,
                      level_strings[level],
                      base,
                      line);
        va_list args;
        va_start(args, fmt);
        (void)vfprintf(g_log_fp, fmt, args);
        va_end(args);
        (void)fputc('\n', g_log_fp);
        (void)fflush(g_log_fp);
    }
    pthread_mutex_unlock(&g_log_mutex);
}
