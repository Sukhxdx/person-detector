#ifndef PERSON_DETECTOR_LOGGER_H
#define PERSON_DETECTOR_LOGGER_H

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3
} log_level_t;

void logger_init(log_level_t min_level);
void logger_set_level(log_level_t min_level);
void logger_shutdown(void);

void log_write(log_level_t level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Variadic form keeps the format string inside __VA_ARGS__ so no GNU comma-paste
   extension is needed; this stays valid under -Wpedantic -Werror. */
#define LOG_DEBUG(...) log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif /* PERSON_DETECTOR_LOGGER_H */
