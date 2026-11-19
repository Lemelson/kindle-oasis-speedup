/*
 * libkppfs.so — LD_PRELOAD accelerator for Amazon's KPPMainApp (Kindle Oasis 3, FW 5.18.2, armv7 hf, glibc 2.20).
 * Cuts syscall overhead without changing what the app sees (beyond a bounded staleness for OTHER processes' writes).
 * Any doubt -> the real call. All features are switched by environment variables (launch.sh reads them from
 * /var/local/kppfast/env); nothing is active unless asked for. Global kill switch: /mnt/us/DISABLE_KPPFAST.
 * Only active inside KPPMainApp (checked via /proc/self/exe); children (sh, lipc tools) are pass-through.
 *
 *  KPPFS_STAT=1         memo of stat/lstat/stat64/lstat64 (__xstat*, __lxstat*) and access()/faccessat(AT_FDCWD)
 *                       for absolute paths under KPPFS_PREFIX (default "/mnt/us/", the FUSE userstore). Caches
 *                       success AND ENOENT. Entry lifetime KPPFS_TTL_MS (default 1000, max 10000).
 *                       Own writes: any mutating call of THIS process on a path in scope (open/fopen with
 *                       O_WRONLY/O_RDWR/O_CREAT/O_TRUNC/"w"/"a"/"+", creat, rename(at), unlink(at), mkdir(at), rmdir,
 *                       link, symlink, utime(s), utimensat, truncate, chmod, chown, lchown) or with a relative path
 *                       bumps a global epoch = every cached entry is dropped. While a file in scope is open for
 *                       writing, the memo is bypassed for that file; its close()/fclose() bumps the epoch again.
 *                       So the app always sees its own writes immediately. Changes made by OTHER processes
 *                       (scanner, thumbnail generator, USB host) become visible at most TTL later.
 *  KPPFS_NEG=1          ENOENT memo (KPPFS_NEG_TTL_MS, default 3000) for fixed paths that do not exist on the device:
 *                       /var/local/rendering/smartinvert.conf, /var/local/token/activeprofile.txt and debug-symbol
 *                       lookups ("*.dbg" under /app, /usr/lib, /proc/<n>/root). Applies to open/open64/fopen/fopen64
 *                       (read-only opens), stat family and access. A create of such a path by this process bumps
 *                       the epoch as above. Positive results are never memoised here.
 *  KPPFS_CLOCK=1        userspace CLOCK_MONOTONIC / CLOCK_REALTIME / gettimeofday from the ARM generic timer
 *                       (CNTVCT) when the kernel lets user space read it (probed with a SIGILL guard at start).
 *                       value = kernel value at the last calibration + CNTVCT delta * 1e9 / CNTFRQ; recalibrated
 *                       by the real syscall when the delta exceeds KPPFS_CLOCK_WIN_MS (default 50); per-clock
 *                       MONOTONIC never goes backwards. Other clock ids -> real call. Probe fails -> real calls.
 *  KPPFS_CLOCK_RT=1     also CLOCK_REALTIME + gettimeofday (off by default: if CNTVCT stops in suspend, the wall
 *                       clock could lag by the suspend length for up to one window after resume).
 *  KPPFS_LOG=<n>        drop syslog() messages with level > n (0 EMERG .. 7 DEBUG). Unset = keep all.
 *  KPPFS_STATS=1        counters dumped to /tmp/kppfs.stats on `touch /tmp/kppfs.req` (diagnostic thread).
 *  KPPFS_PREFIX=<p>     scope of the stat memo (one absolute prefix, default /mnt/us/).
 *
 * Thread safety: the memo is a fixed table of seqlocked slots. Readers never block (copy + re-check sequence; a
 * torn read is detected and falls back to the real call). Writers take a slot with one CAS; if it is busy the
 * result is simply not cached. No malloc, no locks on the hot path.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <syslog.h>
#include <dlfcn.h>
#include <pthread.h>
#include <utime.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1l)
#endif
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

static int g_on, g_stat, g_neg, g_clock, g_clock_rt, g_logmax = 99, g_stats;
static int64_t g_ttl_ns = 1000000000LL, g_neg_ttl_ns = 3000000000LL;
static char g_prefix[64] = "/mnt/us/"; static size_t g_prefix_len = 8;
static volatile unsigned g_epoch = 1;          /* bumped by every own mutation in scope */
static volatile int g_writers;                 /* files in scope currently open for writing by this process */

/* counters (diagnostic only; relaxed atomics) */
enum { C_STAT, C_STAT_HIT, C_NEG, C_NEG_HIT, C_EPOCH, C_CLK, C_CLK_FAST, C_CLK_CAL, C_LOG_DROP, C_ACC, C_ACC_HIT, NC };
static volatile unsigned long g_cnt[NC];
#define CNT(i) do { if (g_stats) __sync_fetch_and_add(&g_cnt[i], 1); } while (0)

/* ---------------- real symbols ---------------- */
#define REAL(name, ret, ...) typedef ret (*name##_t)(__VA_ARGS__); static name##_t r_##name
REAL(__xstat, int, int, const char *, struct stat *);
REAL(__lxstat, int, int, const char *, struct stat *);
REAL(__xstat64, int, int, const char *, struct stat64 *);
REAL(__lxstat64, int, int, const char *, struct stat64 *);
REAL(access, int, const char *, int);
REAL(faccessat, int, int, const char *, int, int);
REAL(open, int, const char *, int, ...);
REAL(open64, int, const char *, int, ...);
REAL(openat, int, int, const char *, int, ...);
REAL(openat64, int, int, const char *, int, ...);
REAL(creat, int, const char *, mode_t);
REAL(fopen, FILE *, const char *, const char *);
REAL(fopen64, FILE *, const char *, const char *);
REAL(close, int, int);
REAL(fclose, int, FILE *);
REAL(rename, int, const char *, const char *);
REAL(renameat, int, int, const char *, int, const char *);
REAL(unlink, int, const char *);
REAL(unlinkat, int, int, const char *, int);
REAL(mkdir, int, const char *, mode_t);
REAL(mkdirat, int, int, const char *, mode_t);
REAL(rmdir, int, const char *);
REAL(link, int, const char *, const char *);
REAL(symlink, int, const char *, const char *);
REAL(utime, int, const char *, const struct utimbuf *);
REAL(utimes, int, const char *, const struct timeval *);
REAL(utimensat, int, int, const char *, const struct timespec *, int);
REAL(truncate, int, const char *, off_t);
REAL(truncate64, int, const char *, off64_t);
REAL(chmod, int, const char *, mode_t);
REAL(chown, int, const char *, uid_t, gid_t);
REAL(lchown, int, const char *, uid_t, gid_t);
REAL(clock_gettime, int, clockid_t, struct timespec *);
REAL(gettimeofday, int, struct timeval *, void *);
REAL(vsyslog, void, int, const char *, va_list);
REAL(__vsyslog_chk, void, int, int, const char *, va_list);
#define RS(name) (r_##name ? r_##name : (r_##name = (name##_t)dlsym(RTLD_NEXT, #name)))

static int raw_mono(struct timespec *ts) { return syscall(SYS_clock_gettime, CLOCK_MONOTONIC, ts); }

/* ---------------- userspace clock (ARM generic timer) ---------------- */
static uint32_t g_cntfrq; static uint64_t g_win_ticks; static uint32_t g_mult; /* ns = ticks*mult >> 24 */
struct clkbase { volatile unsigned seq; uint64_t cnt; int64_t ns; };
static struct clkbase g_cb[2];                     /* [0] REALTIME, [1] MONOTONIC */
static volatile int64_t g_last[2];
static inline uint64_t cntvct(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline uint32_t cntfrq(void) { uint32_t f; __asm__ __volatile__("mrc p15, 0, %0, c14, c0, 0" : "=r"(f)); return f; }
static sigjmp_buf g_probe_jb;
static void probe_sig(int s) { (void)s; siglongjmp(g_probe_jb, 1); }
static int clock_probe(void)
{
    struct sigaction sa, o1, o2; memset(&sa, 0, sizeof sa); sa.sa_handler = probe_sig; sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, &o1); sigaction(SIGSEGV, &sa, &o2);
    volatile int ok = 0; volatile uint32_t f = 0; volatile uint64_t a = 0, b = 0;
    if (sigsetjmp(g_probe_jb, 1) == 0) { f = cntfrq(); a = cntvct(); ok = 1; }
    sigaction(SIGILL, &o1, NULL); sigaction(SIGSEGV, &o2, NULL);
    if (!ok || f < 1000000 || f > 1000000000 || a == 0) return 0;
    /* sanity: counter must advance at CNTFRQ against the kernel clock (5 ms window, 2 % tolerance) */
    struct timespec t0, t1, d = {0, 5000000}; raw_mono(&t0); a = cntvct(); nanosleep(&d, NULL); b = cntvct(); raw_mono(&t1);
    double kns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec), cns = (double)(b - a) * 1e9 / f;
    if (b <= a || cns < kns * 0.90 || cns > kns * 1.02 + 200000) return 0;
    g_cntfrq = f; g_mult = (uint32_t)(((uint64_t)1000000000 << 24) / f);
    return 1;
}
static int64_t ts2ns(const struct timespec *t) { return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec; }
static void ns2ts(int64_t ns, struct timespec *t) { t->tv_sec = (time_t)(ns / 1000000000LL); t->tv_nsec = (long)(ns % 1000000000LL); }
static void clamp_store(int k, int64_t ns) /* remember the largest value handed out for clock k */
{
    int64_t o = __atomic_load_n(&g_last[k], __ATOMIC_ACQUIRE);
    while (ns > o && !__atomic_compare_exchange_n(&g_last[k], &o, ns, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
}
/* MONOTONIC never goes backwards. REALTIME: tiny backward steps (< 1 ms, extrapolation vs kernel rate) are hidden,
   a real step back of the wall clock (settimeofday / time sync) is passed through and becomes the new floor. */
static int64_t clampv(int k, int64_t ns)
{
    int64_t last = __atomic_load_n(&g_last[k], __ATOMIC_ACQUIRE);
    if (ns >= last) { clamp_store(k, ns); return ns; }
    if (k == 1 || last - ns < 1000000) return last;
    __atomic_store_n(&g_last[k], ns, __ATOMIC_RELEASE);
    return ns;
}
/* returns 0 and fills *out, or -1 = use the real syscall */
static int fast_clock(int k, int64_t *out)
{
    struct clkbase *b = &g_cb[k];
    unsigned s1 = b->seq; __sync_synchronize();
    if (s1 & 1) return -1;
    uint64_t bc = b->cnt; int64_t bn = b->ns;
    __sync_synchronize(); if (b->seq != s1 || bc == 0) return -1;
    uint64_t c = cntvct();
    if (c < bc || c - bc > g_win_ticks) return -1;
    int64_t ns = bn + (int64_t)(((c - bc) * (uint64_t)g_mult) >> 24);
    *out = clampv(k, ns); return 0;
}
static int calibrate(int k, clockid_t id, struct timespec *ts)
{
    uint64_t c = cntvct();
    int r = syscall(SYS_clock_gettime, id, ts);
    if (r) return r;
    int64_t ns = ts2ns(ts), v = clampv(k, ns);
    if (v != ns) { ns2ts(v, ts); ns = v; }
    struct clkbase *b = &g_cb[k]; unsigned s = b->seq;
    if (!(s & 1) && __sync_bool_compare_and_swap(&b->seq, s, s + 1)) {
        b->cnt = c; b->ns = ns; __sync_synchronize(); b->seq = s + 2;
    }
    CNT(C_CLK_CAL);
    return 0;
}
int clock_gettime(clockid_t id, struct timespec *ts)
{
    if (LIKELY(g_clock) && (id == CLOCK_MONOTONIC || (id == CLOCK_REALTIME && g_clock_rt)) && ts) {
        int k = id == CLOCK_MONOTONIC; int64_t ns;
        CNT(C_CLK);
        if (fast_clock(k, &ns) == 0) { ns2ts(ns, ts); CNT(C_CLK_FAST); return 0; }
        return calibrate(k, id, ts);
    }
    return RS(clock_gettime)(id, ts);
}
int gettimeofday(struct timeval *tv, void *tz)
{
    if (LIKELY(g_clock_rt) && tv && !tz) {
        struct timespec ts; int64_t ns;
        CNT(C_CLK);
        if (fast_clock(0, &ns) == 0) { CNT(C_CLK_FAST); ns2ts(ns, &ts); }
        else if (calibrate(0, CLOCK_REALTIME, &ts)) return RS(gettimeofday)(tv, tz);
        tv->tv_sec = ts.tv_sec; tv->tv_usec = ts.tv_nsec / 1000; return 0;
    }
    return RS(gettimeofday)(tv, tz);
}

/* time base for memo TTLs: the fast clock if available, else one raw CLOCK_MONOTONIC_COARSE syscall
   (still far cheaper than a FUSE round trip) */
static int64_t now_ns(void)
{
    int64_t ns;
    if (g_clock && fast_clock(1, &ns) == 0) return ns;
    struct timespec ts;
    if (g_clock) { calibrate(1, CLOCK_MONOTONIC, &ts); return ts2ns(&ts); }
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC_COARSE, &ts);
    return ts2ns(&ts);
}

/* ---------------- path classification ---------------- */
static inline int in_scope(const char *p)
{
    return g_stat && p && p[0] == '/' && strncmp(p, g_prefix, g_prefix_len) == 0;
}
static int ends_with(const char *p, size_t n, const char *suf) { size_t m = strlen(suf); return n >= m && memcmp(p + n - m, suf, m) == 0; }
static int neg_path(const char *p)
{
    if (!g_neg || !p || p[0] != '/') return 0;
    if (strcmp(p, "/var/local/rendering/smartinvert.conf") == 0) return 1;
    if (strcmp(p, "/var/local/token/activeprofile.txt") == 0) return 1;
    size_t n = strlen(p);
    if (ends_with(p, n, ".dbg") && (strncmp(p, "/app/", 5) == 0 || strncmp(p, "/usr/lib/", 9) == 0 || strncmp(p, "/proc/", 6) == 0))
        return 1;
    return 0;
}
/* a mutating call on path p: drop the memo if p could be something we cache */
static void mutated(const char *p)
{
    if (!g_stat && !g_neg) return;
    if (!p || p[0] != '/' || in_scope(p) || neg_path(p) ||
        (g_neg && (strncmp(p, "/var/local/rendering", 20) == 0 || strncmp(p, "/var/local/token", 16) == 0))) {
        __sync_fetch_and_add(&g_epoch, 1); CNT(C_EPOCH);
    }
}
static void mutated_at(int dfd, const char *p) { if (dfd != AT_FDCWD && p && p[0] != '/') mutated(NULL); else mutated(p); }

/* ---------------- the memo table ---------------- */
#define NSLOT 1024
#define PMAX 152
struct slot {
    volatile unsigned seq;
    unsigned epoch, hash; int kind; int err; int64_t t; int64_t ttl;
    char path[PMAX];
    struct stat64 st;      /* also holds struct stat for the 32-bit variants (smaller) */
};
static struct slot g_tab[NSLOT];
enum { K_XSTAT, K_LXSTAT, K_XSTAT64, K_LXSTAT64, K_ACCESS /* + mode */ = 8 };
static unsigned hash_path(const char *p, size_t *len)
{
    unsigned h = 2166136261u; const char *c = p;
    for (; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    *len = (size_t)(c - p); return h;
}
/* 1 = hit (buf/err filled), 0 = miss */
static int memo_get(const char *p, unsigned h, size_t n, int kind, void *buf, size_t sz, int *err)
{
    struct slot *s = &g_tab[(h ^ (unsigned)kind * 0x9e3779b1u) % NSLOT];
    unsigned s1 = s->seq; __sync_synchronize();
    if (s1 & 1 || s->hash != h || s->kind != kind || s->epoch != g_epoch) return 0;
    int64_t t = s->t, ttl = s->ttl; int e = s->err;
    if (memcmp(s->path, p, n + 1) != 0) return 0;
    if (buf && !e) memcpy(buf, &s->st, sz);
    __sync_synchronize();
    if (s->seq != s1) return 0;
    int64_t now = now_ns();
    if (now - t >= ttl || now < t) return 0;
    *err = e; return 1;
}
static void memo_put(const char *p, unsigned h, size_t n, int kind, const void *buf, size_t sz, int err, unsigned epoch, int64_t t, int64_t ttl)
{
    if (n >= PMAX || sz > sizeof(struct stat64)) return;
    struct slot *s = &g_tab[(h ^ (unsigned)kind * 0x9e3779b1u) % NSLOT];
    unsigned q = s->seq;
    if ((q & 1) || !__sync_bool_compare_and_swap(&s->seq, q, q + 1)) return;   /* busy: just don't cache */
    s->hash = h; s->kind = kind; s->err = err; s->t = t; s->ttl = ttl; s->epoch = epoch;
    memcpy(s->path, p, n + 1);
    if (buf && !err) memcpy(&s->st, buf, sz);
    __sync_synchronize(); s->seq = q + 2;
}

static int being_written(unsigned h);
/* generic memoised call: fn(...) is the real call; kind selects the slot key */
typedef int (*realcall)(void *ctx);
static int memo_call(const char *p, int kind, void *buf, size_t sz, realcall fn, void *ctx)
{
    int scope = in_scope(p), neg = !scope && neg_path(p);
    if (!scope && !neg) return fn(ctx);
    size_t n; unsigned h = hash_path(p, &n);
    if (n >= PMAX) return fn(ctx);
    if (scope && g_writers && being_written(h)) return fn(ctx);  /* own write in progress: no memo for it */
    int e;
    CNT(scope ? C_STAT : C_NEG);
    if (memo_get(p, h, n, kind, buf, sz, &e)) {
        CNT(scope ? C_STAT_HIT : C_NEG_HIT);
        if (e) { errno = e; return -1; }
        return 0;
    }
    unsigned ep = g_epoch; int64_t t = now_ns();
    int r = fn(ctx), sv = errno;
    if (r == 0 && scope) memo_put(p, h, n, kind, buf, sz, 0, ep, t, g_ttl_ns);
    else if (r < 0 && sv == ENOENT) memo_put(p, h, n, kind, NULL, 0, ENOENT, ep, t, scope ? g_ttl_ns : g_neg_ttl_ns);
    errno = sv; return r;
}

struct sctx { int ver; const char *p; void *buf; int (*f)(int, const char *, void *); int mode; int dfd; int flags; };
static int c_stat(void *c) { struct sctx *x = c; return x->f(x->ver, x->p, x->buf); }
#define STATFN(name, T, K) \
int name(int ver, const char *p, T *buf) \
{ \
    if (!RS(name)) { errno = ENOSYS; return -1; } \
    if (!g_on || !buf) return r_##name(ver, p, buf); \
    struct sctx x = { ver, p, buf, (int (*)(int, const char *, void *))r_##name, 0, 0, 0 }; \
    return memo_call(p, K + ver * 16, buf, sizeof *buf, c_stat, &x); \
}
STATFN(__xstat, struct stat, K_XSTAT)
STATFN(__lxstat, struct stat, K_LXSTAT)
STATFN(__xstat64, struct stat64, K_XSTAT64)
STATFN(__lxstat64, struct stat64, K_LXSTAT64)

static int c_access(void *c) { struct sctx *x = c; return RS(access)(x->p, x->mode); }
static int c_faccessat(void *c) { struct sctx *x = c; return RS(faccessat)(x->dfd, x->p, x->mode, x->flags); }
int access(const char *p, int mode)
{
    if (!g_on || (mode & ~7)) return RS(access)(p, mode);
    struct sctx x = { 0, p, NULL, NULL, mode, AT_FDCWD, 0 };
    CNT(C_ACC);
    return memo_call(p, K_ACCESS + mode, NULL, 0, c_access, &x);
}
int faccessat(int dfd, const char *p, int mode, int flags)
{
    if (!g_on || (mode & ~7) || flags || !p || (dfd != AT_FDCWD && p[0] != '/')) return RS(faccessat)(dfd, p, mode, flags);
    struct sctx x = { 0, p, NULL, NULL, mode, dfd, flags };
    CNT(C_ACC);
    return memo_call(p, K_ACCESS + mode, NULL, 0, c_faccessat, &x);
}

/* ---------------- opens ---------------- */
#define WFLAGS (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)
/* writer set: files in scope that this process holds open for writing. A memo lookup of such a path goes to the
   kernel. Entries leave the set on close()/fclose() of their fd, or lazily when fcntl(fd) says the fd is gone
   (close paths we cannot see, e.g. libc-internal ones). Full set -> memo bypassed for every path (safe). */
#define NW 32
static struct { volatile int fd; volatile unsigned hash; } g_w[NW];
static volatile int g_wfull;
static void track_wfd(int fd, const char *p)
{
    char buf[PMAX + 8];
    if (fd < 0) return;
    if (!p || p[0] != '/') {               /* relative: resolve through /proc/self/fd */
        char l[32]; snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(l, buf, sizeof buf - 1);
        if (n <= 0) { g_wfull = 1; return; }
        buf[n] = 0; p = buf;
        if (!in_scope(p)) return;
    }
    size_t n; unsigned h = hash_path(p, &n);
    for (int i = 0; i < NW; i++)
        if (__sync_bool_compare_and_swap(&g_w[i].fd, 0, fd + 1)) { g_w[i].hash = h; __sync_fetch_and_add(&g_writers, 1); return; }
    g_wfull = 1;
}
static void untrack_wfd(int fd)
{
    for (int i = 0; i < NW; i++)
        if (g_w[i].fd == fd + 1 && __sync_bool_compare_and_swap(&g_w[i].fd, fd + 1, 0)) { __sync_fetch_and_sub(&g_writers, 1); mutated(NULL); }
}
/* 1 = path h is (possibly) open for writing by us */
static int being_written(unsigned h)
{
    if (g_wfull) return 1;
    for (int i = 0; i < NW; i++) {
        int f = g_w[i].fd;
        if (f && g_w[i].hash == h) {
            if (fcntl(f - 1, F_GETFD) < 0 && errno == EBADF) {          /* closed behind our back */
                if (__sync_bool_compare_and_swap(&g_w[i].fd, f, 0)) { __sync_fetch_and_sub(&g_writers, 1); mutated(NULL); }
                continue;
            }
            return 1;
        }
    }
    return 0;
}
static int open_common(const char *p, int fl, int dfd, int *handled)
{
    *handled = 0;
    if (!g_on || !p) return 0;
    if (fl & WFLAGS) { mutated_at(dfd, p); return 0; }
    if (dfd == AT_FDCWD && neg_path(p)) {
        size_t n; unsigned h = hash_path(p, &n); int e;
        CNT(C_NEG);
        if (n < PMAX && memo_get(p, h, n, 7, NULL, 0, &e) && e) { CNT(C_NEG_HIT); errno = e; *handled = 1; return -1; }
    }
    return 0;
}
static void open_done(const char *p, int fl, int dfd, int fd, int sv, unsigned ep, int64_t t)
{
    if (!g_on || !p) return;
    if (fl & WFLAGS) {
        if (fd >= 0 && g_stat && (dfd != AT_FDCWD || p[0] != '/' || in_scope(p))) track_wfd(fd, (dfd == AT_FDCWD || p[0] == '/') ? p : NULL);
        mutated_at(dfd, p);
        return;
    }
    if (fd < 0 && sv == ENOENT && dfd == AT_FDCWD && neg_path(p)) {
        size_t n; unsigned h = hash_path(p, &n);
        if (n < PMAX) memo_put(p, h, n, 7, NULL, 0, ENOENT, ep, t, g_neg_ttl_ns);
    }
}
#define GETMODE mode_t mode = 0; \
    if (fl & (O_CREAT | 020000000 /* O_TMPFILE */)) { va_list ap; va_start(ap, fl); mode = va_arg(ap, mode_t); va_end(ap); }
#define PRE(DFD) int h; int r = open_common(p, fl, DFD, &h); if (h) return r; \
    unsigned ep = g_epoch; int64_t t = (g_on && g_neg && !(fl & WFLAGS)) ? now_ns() : 0;
#define POST(DFD) int sv = errno; open_done(p, fl, DFD, fd, sv, ep, t); errno = sv; return fd;
int open(const char *p, int fl, ...)
{ GETMODE if (!RS(open)) { errno = ENOSYS; return -1; } PRE(AT_FDCWD) int fd = r_open(p, fl, mode); POST(AT_FDCWD) }
int open64(const char *p, int fl, ...)
{ GETMODE if (!RS(open64)) { errno = ENOSYS; return -1; } PRE(AT_FDCWD) int fd = r_open64(p, fl, mode); POST(AT_FDCWD) }
int openat(int dfd, const char *p, int fl, ...)
{ GETMODE if (!RS(openat)) { errno = ENOSYS; return -1; } PRE(dfd) int fd = r_openat(dfd, p, fl, mode); POST(dfd) }
int openat64(int dfd, const char *p, int fl, ...)
{ GETMODE if (!RS(openat64)) { errno = ENOSYS; return -1; } PRE(dfd) int fd = r_openat64(dfd, p, fl, mode); POST(dfd) }
int creat(const char *p, mode_t m)
{ int fl = O_CREAT | O_WRONLY | O_TRUNC; if (!RS(creat)) { errno = ENOSYS; return -1; } PRE(AT_FDCWD) int fd = r_creat(p, m); POST(AT_FDCWD) }
static int mode_flags(const char *m)
{
    if (!m) return 0;
    int fl = 0;
    for (const char *c = m; *c; c++) { if (*c == 'w' || *c == 'a') fl |= O_WRONLY | O_CREAT; if (*c == '+') fl |= O_RDWR; }
    return fl;
}
#define FOPENFN(name) \
FILE *name(const char *p, const char *m) \
{ \
    if (!RS(name)) { errno = ENOSYS; return NULL; } \
    int fl = mode_flags(m), h; open_common(p, fl, AT_FDCWD, &h); if (h) return NULL; \
    unsigned ep = g_epoch; int64_t t = (g_on && g_neg && !(fl & WFLAGS)) ? now_ns() : 0; \
    FILE *f = r_##name(p, m); int sv = errno; \
    open_done(p, fl, AT_FDCWD, f ? fileno(f) : -1, sv, ep, t); errno = sv; return f; \
}
FOPENFN(fopen)
FOPENFN(fopen64)
int close(int fd)
{
    if (UNLIKELY(g_writers) && fd >= 0) { int r = RS(close)(fd); int sv = errno; untrack_wfd(fd); errno = sv; return r; }
    return RS(close)(fd);
}
int fclose(FILE *f)
{
    if (UNLIKELY(g_writers) && f) { int fd = fileno(f); int r = RS(fclose)(f); int sv = errno; untrack_wfd(fd); errno = sv; return r; }
    return RS(fclose)(f);
}

/* ---------------- other mutating calls: drop the memo ---------------- */
#define MUT1(name, ret, T1, ...) ret name(const char *p, T1 a) { if (g_on) mutated(p); ret r = RS(name)(p, a); if (g_on) { int sv = errno; mutated(p); errno = sv; } return r; }
MUT1(mkdir, int, mode_t)
MUT1(chmod, int, mode_t)
MUT1(utime, int, const struct utimbuf *)
MUT1(utimes, int, const struct timeval *)
MUT1(truncate, int, off_t)
MUT1(truncate64, int, off64_t)
#define MUT0(name) int name(const char *p) { if (g_on) mutated(p); int r = RS(name)(p); if (g_on) { int sv = errno; mutated(p); errno = sv; } return r; }
MUT0(unlink)
MUT0(rmdir)
#define MUT2P(name) int name(const char *a, const char *b) { if (g_on) { mutated(a); mutated(b); } int r = RS(name)(a, b); if (g_on) { int sv = errno; mutated(a); mutated(b); errno = sv; } return r; }
MUT2P(rename)
MUT2P(link)
MUT2P(symlink)
int chown(const char *p, uid_t u, gid_t g) { if (g_on) mutated(p); int r = RS(chown)(p, u, g); if (g_on) { int sv = errno; mutated(p); errno = sv; } return r; }
int lchown(const char *p, uid_t u, gid_t g) { if (g_on) mutated(p); int r = RS(lchown)(p, u, g); if (g_on) { int sv = errno; mutated(p); errno = sv; } return r; }
int renameat(int d1, const char *a, int d2, const char *b)
{ if (g_on) { mutated_at(d1, a); mutated_at(d2, b); } int r = RS(renameat)(d1, a, d2, b); if (g_on) { int sv = errno; mutated_at(d1, a); mutated_at(d2, b); errno = sv; } return r; }
int unlinkat(int d, const char *p, int fl)
{ if (g_on) mutated_at(d, p); int r = RS(unlinkat)(d, p, fl); if (g_on) { int sv = errno; mutated_at(d, p); errno = sv; } return r; }
int mkdirat(int d, const char *p, mode_t m)
{ if (g_on) mutated_at(d, p); int r = RS(mkdirat)(d, p, m); if (g_on) { int sv = errno; mutated_at(d, p); errno = sv; } return r; }
int utimensat(int d, const char *p, const struct timespec *t, int fl)
{ if (g_on) { if (p) mutated_at(d, p); else mutated(NULL); } int r = RS(utimensat)(d, p, t, fl); if (g_on) { int sv = errno; if (p) mutated_at(d, p); else mutated(NULL); errno = sv; } return r; }

/* ---------------- syslog filter ---------------- */
void vsyslog(int pri, const char *fmt, va_list ap)
{
    if (g_on && (pri & 7) > g_logmax) { CNT(C_LOG_DROP); return; }
    RS(vsyslog)(pri, fmt, ap);
}
void syslog(int pri, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsyslog(pri, fmt, ap); va_end(ap); }
void __vsyslog_chk(int pri, int flag, const char *fmt, va_list ap)
{
    if (g_on && (pri & 7) > g_logmax) { CNT(C_LOG_DROP); return; }
    RS(__vsyslog_chk)(pri, flag, fmt, ap);
}
void __syslog_chk(int pri, int flag, const char *fmt, ...) { va_list ap; va_start(ap, fmt); __vsyslog_chk(pri, flag, fmt, ap); va_end(ap); }

/* ---------------- stats thread (diagnostic) ---------------- */
static void dump(void)
{
    static const char *nm[NC] = { "stat_scope", "stat_hit", "neg_lookups", "neg_hit", "epoch_bumps", "clock_calls",
                                  "clock_fast", "clock_calibrations", "syslog_dropped", "access_calls", "access_hit" };
    FILE *o = RS(fopen)("/tmp/kppfs.stats.tmp", "w"); if (!o) return;
    for (int i = 0; i < NC; i++) fprintf(o, "%s %lu\n", nm[i], g_cnt[i]);
    fprintf(o, "clock_rt %d\n", g_clock_rt);
    fprintf(o, "wfull %d\n", g_wfull);
    fprintf(o, "writers %d epoch %u stat %d neg %d clock %d cntfrq %u ttl_ms %lld logmax %d prefix %s\n", g_writers, g_epoch,
            g_stat, g_neg, g_clock, g_cntfrq, (long long)(g_ttl_ns / 1000000), g_logmax, g_prefix);
    RS(fclose)(o); RS(rename)("/tmp/kppfs.stats.tmp", "/tmp/kppfs.stats");
}
static void *dumper(void *x)
{
    (void)x;
    for (;;) {
        struct timespec d = {0, 500000000}; nanosleep(&d, NULL);
        if (RS(access)("/tmp/kppfs.req", F_OK) == 0) { RS(unlink)("/tmp/kppfs.req"); dump(); }
    }
    return NULL;
}

static long envl(const char *n, long def, long lo, long hi)
{
    const char *v = getenv(n); if (!v || !*v) return def;
    char *e; long x = strtol(v, &e, 10); if (*e || x < lo || x > hi) return def; return x;
}
__attribute__((constructor)) static void kppfs_init(void)
{
    char exe[256]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    if (!strstr(exe, "KPPMainApp")) return;                 /* only Amazon's KPPMainApp(V2) */
    if (syscall(SYS_access, "/mnt/us/DISABLE_KPPFAST", F_OK) == 0) return;
    g_stat = envl("KPPFS_STAT", 0, 0, 1);
    g_neg = envl("KPPFS_NEG", 0, 0, 1);
    g_ttl_ns = envl("KPPFS_TTL_MS", 1000, 1, 10000) * 1000000LL;
    g_neg_ttl_ns = envl("KPPFS_NEG_TTL_MS", 3000, 1, 30000) * 1000000LL;
    g_logmax = (int)envl("KPPFS_LOG", 99, 0, 7);
    g_stats = envl("KPPFS_STATS", 0, 0, 1);
    const char *pf = getenv("KPPFS_PREFIX");
    if (pf && pf[0] == '/' && strlen(pf) < sizeof g_prefix - 1 && strlen(pf) > 1) { strcpy(g_prefix, pf); g_prefix_len = strlen(pf); }
    if (envl("KPPFS_CLOCK", 0, 0, 1) && clock_probe()) {
        g_win_ticks = (uint64_t)g_cntfrq * envl("KPPFS_CLOCK_WIN_MS", 50, 1, 1000) / 1000;
        g_clock = 1;
        g_clock_rt = envl("KPPFS_CLOCK_RT", 0, 0, 1);
    }
    g_on = 1;
    if (g_stats) { pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &a, dumper, NULL); }
    syslog(LOG_NOTICE, "kppfs: active stat=%d neg=%d clock=%d (cntfrq %u) ttl=%lldms log<=%d", g_stat, g_neg, g_clock, g_cntfrq,
           (long long)(g_ttl_ns / 1000000), g_logmax);
}
