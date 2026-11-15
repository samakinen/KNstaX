/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2025-2026 Sami Mäkinen */

/* newlib-nano system calls for the size gate. The only one that matters is
 * _sbrk: it records the heap high-water mark, which with the static
 * .data/.bss size is the RAM figure the gate checks. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

extern char _heap_start, _heap_limit;

static char* g_brk = &_heap_start;

/* Highest heap address handed out, as an offset from the start of the heap. */
size_t g_heap_high_water = 0;

#if defined(KNX_SIZE_GATE_SEMIHOST)
/* On the emulator, say why the run is about to die: a heap that cannot grow
 * ends in abort() and a silent spin otherwise. */
static void semihost_write0(const char* text) {
    register int r0 __asm__("r0") = 0x04; /* SYS_WRITE0 */
    register const char* r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
}

/* Allocation profile: how many malloc() calls of each size the run made, so a
 * heap figure can be traced to the structures behind it. Built with
 * -Wl,--wrap=malloc; the table is small and anything past it is lumped under
 * size 0xFFFF, which is itself a finding. */
#define PROFILE_SLOTS 96
static struct { uint32_t size; uint32_t count; } g_profile[PROFILE_SLOTS];
extern void* __real_malloc(size_t size);

void* __wrap_malloc(size_t size) {
    uint32_t key = size > 0xFFFEu ? 0xFFFFu : (uint32_t)size;
    for (int i = 0; i < PROFILE_SLOTS; ++i) {
        if (g_profile[i].count == 0u) { g_profile[i].size = key; g_profile[i].count = 1u; break; }
        if (g_profile[i].size == key) { ++g_profile[i].count; break; }
    }
    return __real_malloc(size);
}

static void write_decimal(const char* label, unsigned value, const char* suffix) {
    char text[64];
    int length = 0;
    for (const char* c = label; *c != 0; ++c) text[length++] = *c;
    char digits[12];
    int count = 0;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; } while (value != 0u);
    while (count > 0) text[length++] = digits[--count];
    for (const char* c = suffix; *c != 0; ++c) text[length++] = *c;
    text[length] = (char)0;
    semihost_write0(text);
}

/* Prints the allocation sizes that account for the most bytes, largest first. */
void knx_size_gate_print_profile(void) {
    uint32_t printed[12] = {0};
    for (int rank = 0; rank < 12; ++rank) {
        int best = -1;
        uint32_t best_bytes = 0;
        for (int i = 0; i < PROFILE_SLOTS; ++i) {
            if (g_profile[i].count == 0u) break;
            int taken = 0;
            for (int r = 0; r < rank; ++r) { if (printed[r] == g_profile[i].size + 1u) taken = 1; }
            uint32_t bytes = g_profile[i].size * g_profile[i].count;
            if (!taken && bytes > best_bytes) { best_bytes = bytes; best = i; }
        }
        if (best < 0) break;
        printed[rank] = g_profile[best].size + 1u;
        write_decimal("alloc ", g_profile[best].count, "");
        write_decimal(" x ", g_profile[best].size, "");
        write_decimal(" = ", best_bytes, "\n");
    }
}

static void report_heap_exhausted(ptrdiff_t increment) {
    char text[64] = "heap exhausted: sbrk(";
    char digits[12];
    int count = 0, length = 21;
    unsigned value = (unsigned)increment;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; } while (value != 0u);
    while (count > 0) { text[length++] = digits[--count]; }
    text[length++] = ')'; text[length++] = '\n'; text[length] = '\0';
    semihost_write0(text);
}
#endif

void* _sbrk(ptrdiff_t increment) {
    char* previous = g_brk;
    if (g_brk + increment > &_heap_limit) {
#if defined(KNX_SIZE_GATE_SEMIHOST)
        report_heap_exhausted(increment);
#endif
        errno = ENOMEM;
        return (void*)-1;
    }
    g_brk += increment;
    if ((size_t)(g_brk - &_heap_start) > g_heap_high_water) {
        g_heap_high_water = (size_t)(g_brk - &_heap_start);
    }
    return previous;
}

/* Needed by __cxa_atexit registrations; there is no dynamic unloading here. */
void* __dso_handle = 0;

int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat* st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
off_t _lseek(int fd, off_t offset, int whence) { (void)fd; (void)offset; (void)whence; return 0; }
int _read(int fd, char* buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _write(int fd, const char* buf, int len) { (void)fd; (void)buf; return len; }
void _exit(int status) { (void)status; for (;;) {} }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }
