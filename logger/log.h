#ifndef LOG_H
#define LOG_H

enum log_level {
    LOG_LEVEL_INVALID = -1,
    LOG_LEVEL_FATAL,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_TRACE,
};

typedef int log_level_t;

void log_init(log_level_t level);
log_level_t log_level_from_str(const char *str);
void log_fmt(log_level_t level, const char *scope, const char *fmt, ...);

#define CONCAT(a, b) a ## b

#if defined(__i386__) || defined(__x86_64__)
#define LOG_(id, level, scope, fmt, ...) \
    do { \
        asm goto( \
            "1: jmp %l0\n" \
            ".pushsection log_entries,\"a\",@progbits\n" \
            ".p2align 2\n" \
            ".4byte 1b-.\n" \
            ".popsection" \
            :::: CONCAT(do_log, id) \
        ); \
        if (__builtin_expect(0, 1)) { \
        CONCAT(do_log, id): \
            log_fmt(level, scope, fmt, ## __VA_ARGS__); \
            asm(""); /* avoid tail call optimization */ \
        } \
    } while (0)
#else
#error Unknown target architecture
#endif

#define LOG_FATAL(fmt, ...) \
    LOG_(__COUNTER__, LOG_LEVEL_FATAL, __FUNCTION__, fmt, ## __VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    LOG_(__COUNTER__, LOG_LEVEL_ERROR, __FUNCTION__, fmt, ## __VA_ARGS__)
#define LOG_WARN(fmt, ...) \
    LOG_(__COUNTER__, LOG_LEVEL_WARN, __FUNCTION__, fmt, ## __VA_ARGS__)
#define LOG_INFO(fmt, ...) \
    LOG_(__COUNTER__, LOG_LEVEL_INFO, __FUNCTION__, fmt, ## __VA_ARGS__)
#define LOG_DEBUG(fmt, ...) \
    LOG_(__COUNTER__, LOG_LEVEL_DEBUG, __FUNCTION__, fmt, ## __VA_ARGS__)
#define LOG_TRACE(fmt, ...) \
    LOG_(__COUNTER__, LOG_LEVEL_TRACE, __FUNCTION__, fmt, ## __VA_ARGS__)

#endif // LOG_H
