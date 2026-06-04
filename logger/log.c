#define _GNU_SOURCE
#include "log.h"
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

static log_level_t g_log_level;

void log_init(log_level_t level)
{
    g_log_level = level;
}

static const char *g_level_names[] = {
    "INVALID",
    "FATAL",
    "ERROR",
    "WARN",
    "INFO",
    "DEBUG",
    "TRACE",
};

log_level_t log_level_from_str(const char *str)
{
    for (unsigned i = 0; i < sizeof g_level_names / sizeof *g_level_names; ++i)
        if (strcasecmp(str, g_level_names[i]) == 0)
            return (log_level_t)i + LOG_LEVEL_INVALID;
    return LOG_LEVEL_INVALID;
}

#if defined(__i386__) || defined(__x86_64__)
#define PAGESIZE 4096
#endif

// SELinux exec* policies:
//   https://web.archive.org/web/20100527212415/http://people.redhat.com/drepper/selinux-mem.html
//   https://bugzilla.redhat.com/show_bug.cgi?id=2031507
//   https://nullprogram.com/blog/2018/11/15/
//   https://lists.nongnu.org/archive/html/tinycc-devel/2014-01/msg00068.html
//   https://stackoverflow.com/q/75277872
//   https://cachecoherence.medium.com/execute-jitted-code-in-selinux-with-android-5-0-f5b76e2abb47
//   https://ankitpatel.org/macos/apple/arm/apple-silicon/pac/pointer-authentication/secureboot/iboot/2020/09/15/apple-silicon-macs.html
//   https://www.outflank.nl/blog/2026/02/19/macos-jit-memory/
//   https://github.com/zherczeg/sljit/issues/99
//   https://github.com/dosbox-staging/dosbox-staging/pull/1031
//   https://kazlauskas.me/entries/fast-page-maps-for-jit
//   https://github.com/upx/upx/discussions/703
//   https://github.com/systemd/systemd/issues/11473
// Different levels of W^X protection can be enabled on a system:
//   0. execheap and execstack have no effect on the naive approach:
//     a. mprotect(RWX), change, mprotect(RX)
//   1. execmem (no W+X simultaneously):
//     a. mprotect(RW), change, mprotect(RX)
//   2. execmem+execmod (no W+X, but also no X for dirty pages after copy-on-write):
//     a. change, save into memfd/file, then re-mmap(RX,FIXED|PRIVATE)
//     b. map (/dev/zero) twice: mmap(RX,FIXED|SHARED)+mmap(SHARED,RW), change RW, unmap it

__attribute((noinline, section("log_protected"), aligned(PAGESIZE)))
static int write_code_protected(void *dst, const void *src, size_t size, void *dst_pages, size_t dst_pages_size)
{
    void (*abort)(void) __attribute((noreturn)) = dlsym(RTLD_DEFAULT, "abort");
    void (*perror)(const char *) = dlsym(RTLD_DEFAULT, "perror");
    int (*close)(int) = dlsym(RTLD_DEFAULT, "close");
    void *(*mmap)(void *, size_t, int, int, int, off_t) = dlsym(RTLD_DEFAULT, "mmap");
    ssize_t (*write)(int, const void *, size_t) = dlsym(RTLD_DEFAULT, "write");
    int (*memfd_create)(const char *, unsigned) = dlsym(RTLD_DEFAULT, "memfd_create");
    int (*memcpy)(void *, const void *, size_t) = dlsym(RTLD_DEFAULT, "memcpy");
    int (*mprotect)(void *, size_t, int) = dlsym(RTLD_DEFAULT, "mprotect");
    volatile int* errno_ptr = &errno;

    // ensure compiler doesn't emit 'call __errno_location@plt' after mprotect(RW) as it may become inaccessible
    asm volatile("" : "+r" (errno_ptr));

    // TODO: fallback
    if (!memfd_create)
        return 1;

    if (mprotect(dst_pages, dst_pages_size, PROT_READ | PROT_WRITE)) {
        perror("DEBUG:log: mprotect(RW)");
        return 1;
    }
    memcpy(dst, src, size);
    if (mprotect(dst_pages, dst_pages_size, PROT_READ | PROT_EXEC)) {
        if (*errno_ptr != EACCES) {
            perror("DEBUG:log: mprotect(RX)");
            abort();
        }
    } else {
        return 0;
    }

    if (memfd_create) {
        // buf: "0x" + number + "\0"
        char buf[2 + sizeof(size_t) / 2 + 1];
        size_t n = (size_t)dst_pages, i = sizeof buf - 1;
        buf[i] = '\0';
        do {
            unsigned d = n % 16;
            buf[--i] = (char)(d < 10 ? d + '0' : d - 10 + 'a');
        } while (n /= 16);
        buf[--i] = 'x', buf[--i] = '0';
        int fd = memfd_create(&buf[i], MFD_CLOEXEC);
        if (fd < 0) {
            perror("DEBUG:log: memfd_create");
        } else {
            *errno_ptr = 0;
            if ((size_t)write(fd, dst_pages, dst_pages_size) != dst_pages_size) {
                perror("DEBUG:log: write");
            } else {
                void *new_dst_pages = mmap(dst_pages, dst_pages_size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, 0);
                if (new_dst_pages != dst_pages) {
                    perror("DEBUG:log: mmap");
                } else {
                    return close(fd);
                }
            }
            close(fd);
        }
    }
    abort();
}

static int write_code(void *dst, const void *src, size_t size)
{
    void *dst_pages = (void *)((size_t)dst / PAGESIZE * PAGESIZE);
    size_t dst_page_end = ((size_t)dst + size + PAGESIZE - 1) / PAGESIZE * PAGESIZE;
    size_t dst_pages_size = dst_page_end - (size_t)dst_pages;
    fprintf(stderr, "DEBUG:log: dst_pages = {%p, %zu}\n", dst_pages, dst_pages_size);
    if (mprotect(dst_pages, dst_pages_size, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        if (errno != EACCES) {
            perror("DEBUG:log: mprotect(RWX)");
            return 1;
        }
        return write_code_protected(dst, src, size, dst_pages, dst_pages_size);
    }
    memcpy(dst, src, size);
    if (mprotect(dst_pages, dst_pages_size, PROT_READ | PROT_EXEC))
        perror("DEBUG:log: mprotect(RX)");
    return 0;
}

// https://stackoverflow.com/questions/16552710/how-do-you-get-the-start-and-end-addresses-of-a-custom-elf-section
// https://mgalgs.io/2013/05/10/hacking-your-ELF-for-fun-and-profit.html
// https://stackoverflow.com/questions/3808053/how-to-get-a-pointer-to-a-binary-section-in-msvc
// https://stackoverflow.com/questions/17669593/how-to-get-a-pointer-to-a-binary-section-in-mac-os-x
__attribute((visibility("hidden")))
extern int32_t start_log_entries[] asm("__start_log_entries");
__attribute((visibility("hidden")))
extern int32_t stop_log_entries[] asm("__stop_log_entries");

static int g_log_disable_caller = 1;

static void log_disable_caller(void *return_address)
{
    typedef int32_t __attribute((aligned(1), may_alias)) unaligned_int32_t;
    fprintf(stderr, "DEBUG:log: start = %p, stop = %p, ra = %p\n",
        start_log_entries, stop_log_entries, return_address);

    size_t nearest_off = SIZE_MAX;
    unsigned char *nearest_caller = NULL;
    const unsigned char *nearest_nop = NULL;
    unsigned nearest_nop_size = 0;

#if defined(__i386__) || defined(__x86_64__)
    // xchg %ax,%ax
    static const unsigned char nop2[2] = {0x66, 0x90};
    // nopl 0x0(%rax,%rax,1)
    static const unsigned char nop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
#endif

    for (int32_t *log_entry = start_log_entries; log_entry != stop_log_entries; ++log_entry) {
        unsigned char *caller = (unsigned char *)log_entry + *log_entry;
        void *target_addr = NULL;
        const unsigned char *nop = NULL;
        unsigned nop_size = 0;

#if defined(__i386__) || defined(__x86_64__)
        if (*caller == 0xeb) {
            nop = nop2;
            nop_size = 2;
            target_addr = caller + (int8_t)caller[1] + 2;
        } else if (*caller == 0xe9) {
            nop = nop5;
            nop_size = 5;
            target_addr = caller + *(unaligned_int32_t *)&caller[1] + 5;
        } else if (*caller == nop2[0] || *caller == nop5[0]) {
            continue;
        } else {
            fprintf(stderr, "WARN:log: unexpected code byte 0x%02x at %p\n", *caller, caller);
            return;
        }
#endif

        if (target_addr <= return_address) {
            if ((size_t)return_address - (size_t)target_addr < nearest_off) {
                nearest_off = (size_t)return_address - (size_t)target_addr;
                nearest_caller = caller;
                nearest_nop = nop;
                nearest_nop_size = nop_size;
            }
        }
    }

    fprintf(stderr, "DEBUG:log: nearest_caller: %p, %u\n", nearest_caller, nearest_nop_size);
    if (write_code(nearest_caller, nearest_nop, nearest_nop_size)) {
        g_log_disable_caller = 0;
        fprintf(stderr, "INFO:log: can't write to %p, disabled code overwriting\n", nearest_caller);
    }
}

void log_fmt(log_level_t level, const char *scope, const char *fmt, ...)
{
    if (level <= g_log_level) {
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "%s:%s: ", g_level_names[level - LOG_LEVEL_INVALID], scope);
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
    } else if (g_log_disable_caller) {
        // https://gcc.gnu.org/onlinedocs/gcc/Return-Address.html
        void *return_address = __builtin_extract_return_addr(__builtin_return_address(0));
        log_disable_caller(return_address);
    }
}
