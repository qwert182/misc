#include "log.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    log_level_t level = LOG_LEVEL_INFO;
    if (argc > 1) {
        log_level_t l = log_level_from_str(argv[1]);
        if (l != LOG_LEVEL_INVALID)
            level = l;
        else
            fprintf(stderr, "WARN: can't parse log level: '%s'\n", argv[1]);
    }
    log_init(level);
    for (int i = 0; i < 3; ++i) {
        LOG_INFO("test");
        LOG_INFO("argc = %d", argc);
        LOG_INFO("argv[0:2] = {'%s', '%s', '%s'}", argv[0], argv[1], argv[2]);
        LOG_INFO("test");
        LOG_INFO("test");
    }
}
