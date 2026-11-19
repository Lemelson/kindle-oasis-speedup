/* mallocbench - single-thread malloc/free micro-benchmark, same binary stock vs LD_PRELOAD=libkppmalloc.so.
 * usage: mallocbench [reps=5]   prints per pattern the median ns/op over reps (each rep re-seeds the same PRNG).
 * a) live set of 2000 blocks, random slot replaced (free + malloc), sizes 16..64 60 %, 64..256 30 %, 256..1024 8 %,
 *    1024..4096 2 %
 * b) burst: malloc 200 blocks (same size mix), free all 200, repeat
 * c) string/map churn: build "strings" (malloc len+13 header, memcpy), append (realloc), map-node mallocs of 24/40
 *    bytes kept in a 512-entry window, free in FIFO order */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

extern char **environ;
static uint32_t S;
static inline uint32_t rnd(void) { S ^= S << 13; S ^= S >> 17; S ^= S << 5; return S; }
static inline size_t rsize(void) {
    uint32_t r = rnd() % 100, x = rnd();
    if (r < 60) return 16 + x % 49;
    if (r < 90) return 64 + x % 193;
    if (r < 98) return 256 + x % 769;
    return 1024 + x % 3073;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static volatile uintptr_t sink;

static double bench_a(long ops) {
    static void *v[2000]; long i; double t0, t1; int k;
    S = 12345;
    for (k = 0; k < 2000; k++) { v[k] = malloc(rsize()); *(char *)v[k] = 1; }
    t0 = now();
    for (i = 0; i < ops; i++) {
        k = rnd() % 2000;
        free(v[k]); v[k] = malloc(rsize()); *(volatile char *)v[k] = 1;
    }
    t1 = now();
    for (k = 0; k < 2000; k++) free(v[k]);
    return (t1 - t0) / ops; /* one op = free + malloc */
}
static double bench_b(long rounds) {
    static void *v[200]; long i; double t0, t1; int k;
    S = 777;
    t0 = now();
    for (i = 0; i < rounds; i++) {
        for (k = 0; k < 200; k++) { v[k] = malloc(rsize()); *(volatile char *)v[k] = 1; }
        for (k = 0; k < 200; k++) free(v[k]);
    }
    t1 = now();
    return (t1 - t0) / (rounds * 200.0); /* one op = malloc + free */
}
static double bench_c(long ops) {
    static char *win[512]; long i, n = 0; double t0, t1; int k; const char txt[] = "The quick brown fox jumps over the lazy dog 0123456789 abcdefghijklmnopqrstuvwxyz";
    S = 4242; memset(win, 0, sizeof win);
    t0 = now();
    for (i = 0; i < ops; i++) {
        uint32_t len = 4 + rnd() % 48;
        char *s = malloc(len + 13); memcpy(s + 12, txt, len); s[12 + len] = 0;           /* string rep */
        if (rnd() % 4 == 0) { uint32_t l2 = len + 8 + rnd() % 40; s = realloc(s, l2 + 13); memset(s + 12 + len, 'x', l2 - len); s[12 + l2] = 0; }
        char *node = malloc(rnd() % 2 ? 24 : 40); *(volatile char *)node = 1;            /* map node */
        sink += (uintptr_t)s[12];
        k = n++ % 512;
        if (win[k]) { free(win[k]); }
        win[k] = s; free(node);
        if (rnd() % 3 == 0) { char *t = malloc(len + 13); memcpy(t, s, len); sink += (uintptr_t)t[0]; free(t); } /* temp copy */
    }
    t1 = now();
    for (k = 0; k < 512; k++) free(win[k]);
    return (t1 - t0) / ops; /* one op = ~3.3 mallocs + frees (+0.25 realloc) */
}
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int real_main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 5, r;
    double ra[16], rb[16], rc[16];
    if (reps > 16) reps = 16;
    bench_a(20000); bench_b(100); bench_c(20000); /* warm-up */
    for (r = 0; r < reps; r++) { ra[r] = bench_a(400000); rb[r] = bench_b(2000); rc[r] = bench_c(200000); }
    qsort(ra, reps, sizeof(double), cmpd); qsort(rb, reps, sizeof(double), cmpd); qsort(rc, reps, sizeof(double), cmpd);
    printf("a_liveset_ns_per_op %.1f (min %.1f max %.1f)\n", ra[reps / 2], ra[0], ra[reps - 1]);
    printf("b_burst_ns_per_op %.1f (min %.1f max %.1f)\n", rb[reps / 2], rb[0], rb[reps - 1]);
    printf("c_strmap_ns_per_op %.1f (min %.1f max %.1f)\n", rc[reps / 2], rc[0], rc[reps - 1]);
    return 0;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("mov r0, sp\n\tmov r1, sp\n\tbic r1, r1, #7\n\tmov sp, r1\n\tbl start_c\n");
}
__attribute__((used, noreturn)) void start_c(long *sp)
{
    int argc = (int)sp[0]; char **argv = (char **)(sp + 1);
    environ = argv + argc + 1;
    exit(real_main(argc, argv));
}
