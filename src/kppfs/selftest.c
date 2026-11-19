/* kppfs-selftest: functional test of libkppfs.so semantics, run in the emulator as
 *   cp kppfs-selftest /tmp/KPPMainApp-selftest   (the library only activates in a process whose exe contains "KPPMainApp")
 *   KPPFS_STAT=1 KPPFS_NEG=1 KPPFS_CLOCK=1 KPPFS_TTL_MS=500 KPPFS_STATS=1 LD_PRELOAD=libkppfs.so /tmp/KPPMainApp-selftest DIR
 * DIR must be under KPPFS_PREFIX (default /mnt/us/). Prints PASS/FAIL lines; exit code = number of failures.
 * Built without crt (own _start) against the device glibc 2.20. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
static int ex(const char *p) { struct stat st; return __xstat(3, p, &st) == 0; }
static int lex(const char *p) { struct stat st; return __lxstat(3, p, &st) == 0; }
static long sz(const char *p) { struct stat st; return __xstat(3, p, &st) == 0 ? (long)st.st_size : -1; }
static void msleep(int ms) { struct timespec d = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&d, NULL); }
/* "another process" changes a file: raw syscalls bypass the library = what the scanner would do */
static void ext_create(const char *p, const char *txt)
{
    int fd = syscall(SYS_open, p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { syscall(SYS_write, fd, txt, strlen(txt)); syscall(SYS_close, fd); }
}
static int64_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static int64_t rawmono(void) { struct timespec t; syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static volatile int stop_threads; static volatile long thr_bad;
static void *clk_thr(void *x)
{
    int64_t last = 0;
    while (!stop_threads) { int64_t v = mono(); if (v < last) __sync_fetch_and_add(&thr_bad, 1); last = v; }
    return x;
}
static char *g_dir;
static void *stat_thr(void *x)
{
    char p[256]; snprintf(p, sizeof p, "%s/thr.txt", g_dir);
    while (!stop_threads) { struct stat st; if (__xstat(3, p, &st) == 0 && st.st_size != 5) __sync_fetch_and_add(&thr_bad, 1); }
    return x;
}

int real_main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/mnt/us/kppfs-test";
    g_dir = (char *)dir;
    char a[256], b[256], d2[256];
    mkdir(dir, 0755);
    snprintf(a, sizeof a, "%s/a.txt", dir); snprintf(b, sizeof b, "%s/b.txt", dir); snprintf(d2, sizeof d2, "%s/sub", dir);
    unlink(a); unlink(b); rmdir(d2);
    int ttl = getenv("KPPFS_TTL_MS") ? atoi(getenv("KPPFS_TTL_MS")) : 1000;

    /* 1. negative result, then own create -> visible at once */
    CHECK(!ex(a) && !ex(a), "ENOENT before create");
    FILE *f = fopen(a, "w"); fputs("hello", f); fclose(f);
    CHECK(ex(a) && sz(a) == 5, "own fopen(w)+fclose visible immediately (size %ld)", sz(a));
    /* 2. own append through open(O_APPEND): size seen at once after write+close and also while open */
    int fd = open(a, O_WRONLY | O_APPEND); write(fd, "12345", 5);
    CHECK(sz(a) == 10, "size while open for writing = %ld (memo bypassed)", sz(a));
    close(fd);
    CHECK(sz(a) == 10, "size after close = %ld", sz(a));
    /* 3. rename / unlink */
    rename(a, b);
    CHECK(!ex(a) && ex(b) && lex(b), "own rename visible");
    unlink(b);
    CHECK(!ex(b) && access(b, F_OK) != 0, "own unlink visible (stat+access)");
    /* 4. mkdir / rmdir */
    CHECK(access(d2, F_OK) != 0, "dir absent"); mkdir(d2, 0755);
    CHECK(access(d2, F_OK) == 0 && ex(d2), "own mkdir visible"); rmdir(d2);
    CHECK(!ex(d2), "own rmdir visible");
    /* 5. other process creates a file: visible after <= TTL */
    CHECK(!ex(a), "absent (cached negative)");
    ext_create(a, "xyz");
    int64_t t0 = rawmono(); while (!ex(a) && rawmono() - t0 < 5000000000LL) msleep(10);
    int64_t lag = (rawmono() - t0) / 1000000;
    CHECK(ex(a) && lag <= ttl + 50, "external create seen after %lld ms (TTL %d)", (long long)lag, ttl);
    /* 6. other process changes size: visible after <= TTL */
    long s0 = sz(a); ext_create(a, "abcdefgh"); t0 = rawmono();
    while (sz(a) == s0 && rawmono() - t0 < 5000000000LL) msleep(10);
    lag = (rawmono() - t0) / 1000000;
    CHECK(sz(a) == 8 && lag <= ttl + 50, "external size change seen after %lld ms", (long long)lag);
    unlink(a);
    /* 7. ENOENT memo for config path */
    int o1 = open("/var/local/rendering/smartinvert.conf", O_RDONLY), e1 = errno;
    int o2 = open("/var/local/rendering/smartinvert.conf", O_RDONLY), e2 = errno;
    CHECK(o1 < 0 && o2 < 0 && e1 == ENOENT && e2 == ENOENT, "smartinvert.conf ENOENT x2 (errno %d %d)", e1, e2);
    /* 8. clock: monotonic, close to the kernel, multithreaded never backwards */
    int64_t k0 = rawmono(), m0 = mono(), k1 = rawmono();
    CHECK(m0 >= k0 - 1000000 && m0 <= k1 + 1000000, "CLOCK_MONOTONIC within 1 ms of kernel (%lld us)", (long long)((m0 - k0) / 1000));
    int64_t maxdev = 0;
    for (int i = 0; i < 200; i++) { int64_t r1 = rawmono(), m = mono(), r2 = rawmono(); int64_t dv = m < r1 ? r1 - m : (m > r2 ? m - r2 : 0); if (dv > maxdev) maxdev = dv; msleep(1); }
    CHECK(maxdev < 2000000, "200 samples: max deviation from kernel %lld us", (long long)(maxdev / 1000));
    /* 9. threads: clock monotonic + stat of a stable file always consistent */
    char tp[256]; snprintf(tp, sizeof tp, "%s/thr.txt", dir); f = fopen(tp, "w"); fputs("12345", f); fclose(f);
    pthread_t th[6]; for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, clk_thr, NULL);
    for (int i = 3; i < 6; i++) pthread_create(&th[i], NULL, stat_thr, NULL);
    msleep(1500); stop_threads = 1; for (int i = 0; i < 6; i++) pthread_join(th[i], NULL);
    CHECK(thr_bad == 0, "3 clock + 3 stat threads, 1.5 s: %ld inconsistencies", thr_bad);
    unlink(tp); rmdir(dir);
    struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt); struct timespec rr; syscall(SYS_clock_gettime, CLOCK_REALTIME, &rr);
    CHECK(llabs((rt.tv_sec - rr.tv_sec) * 1000000000LL + rt.tv_nsec - rr.tv_nsec) < 5000000, "CLOCK_REALTIME sane");
    printf("selftest: %d failure(s)\n", fails);
    fflush(stdout);
    if (getenv("KPPFS_STATS")) { FILE *q = fopen("/tmp/kppfs.req", "w"); if (q) fclose(q); msleep(1200);
        FILE *s = fopen("/tmp/kppfs.stats", "r"); char l[256]; while (s && fgets(l, sizeof l, s)) fputs(l, stdout); if (s) fclose(s); }
    return fails;
}

/* no crt: the device glibc initialises itself from ld.so; argv from the initial stack */
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
