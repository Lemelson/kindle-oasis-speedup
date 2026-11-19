/* libkppmalloc.so - per-thread cache in front of glibc 2.20 malloc (which has no tcache; that came in 2.26).
 * LD_PRELOAD for Amazon's KPPMainApp (Kindle Oasis 3, FW 5.18.2, armv7 hf).
 *
 * Design: the heap stays glibc's. Every pointer the app ever sees is a normal glibc chunk obtained from
 * __libc_malloc; we only keep recently freed small chunks (chunk size 16..1024 bytes) on per-thread LIFO lists
 * instead of handing them back, and serve later requests of the same chunk size from there. So realloc,
 * memalign, malloc_usable_size, mallinfo, free() of a pointer allocated before we were active, etc. all keep
 * working unchanged, and the library can be inert at any moment (pure pass-through) without any hand-over.
 * No locks, no syscalls on the fast path. Thread exit: pthread key destructor returns the cache to glibc.
 *
 * Switches: KPPMALLOC=1 enables (default off = pass-through). /mnt/us/DISABLE_KPPFAST -> inert.
 *           Only active when /proc/self/exe contains "KPPMainApp" (children inherit LD_PRELOAD, stay stock).
 *           KPPMALLOC_STATS=1 -> counters written to /tmp/kppmalloc.<pid>.out when /tmp/kppmalloc.req is touched
 *           (checked every 4096 cached frees) and at exit.
 *           KPPMALLOC_LIMITS=a,b,c -> per-bin chunk limits for chunk size <=128 / <=512 / <=1024 (default 32,16,8).
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

extern void *__libc_malloc(size_t);
extern void __libc_free(void *);
extern void *__libc_calloc(size_t, size_t);

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* glibc 2.20, 32-bit: chunk header = 4-byte size word right before the user pointer; low 3 bits are flags
 * (1 PREV_INUSE, 2 IS_MMAPPED, 4 NON_MAIN_ARENA); alignment 8, minimum chunk 16; request2size(n) = max(16,(n+4+7)&~7). */
#define MAXCS 1024u
#define NX (MAXCS / 8 + 1) /* bin index = chunk size / 8: 2..128 = chunk sizes 16,24,...,1024 (0 and 1 unused) */
#define MAXREQ (MAXCS - 4)
/* bin of a request: request2size(n) / 8 = (max(n,5) + 11) >> 3 (n <= 12 -> 16-byte chunk -> 2) */
#define REQIDX(n) ((((n) < 5 ? 5 : (n)) + 11) >> 3)

enum { T_NEW = 0, T_READY = 1, T_DEAD = 2, T_INIT = 3 };
typedef struct {
    void *head[NX];
    uint16_t cnt[NX];
    uint8_t state;
} tc_t;

static __thread tc_t tc __attribute__((tls_model("initial-exec")));
static int on;       /* 0 until the constructor decided; pass-through while 0 */
static int stats_on;
static pthread_key_t key;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int key_ok;
static unsigned long st_hit, st_miss, st_big, st_fcache, st_ffull, st_fpass, st_calloc_hit, st_calloc_miss, st_flush; /* racy, stats only */
static uint16_t lim[NX]; /* per-bin limit, filled by init() (all 0 while off -> nothing is ever cached) */

static void flush(tc_t *t) {
    unsigned i;
    for (i = 0; i < NX; i++) {
        void *p = t->head[i];
        t->head[i] = 0; t->cnt[i] = 0;
        while (p) { void *n = *(void **)p; __libc_free(p); p = n; st_flush++; }
    }
}
static void dtor(void *v) {
    tc_t *t = v;
    t->state = T_DEAD; /* later frees in this thread (other key destructors) go straight to glibc */
    flush(t);
}
static void mkkey(void) { key_ok = pthread_key_create(&key, dtor) == 0; }

static int thread_init(tc_t *t) {
    t->state = T_INIT; /* pass-through while we set up (pthread_setspecific may allocate) */
    pthread_once(&once, mkkey);
    if (key_ok && pthread_setspecific(key, t) == 0) { t->state = T_READY; return 1; }
    t->state = T_DEAD;
    return 0;
}

static void dump(void);
static __attribute__((noinline)) void stats_tick(void) {
    if (access("/tmp/kppmalloc.req", F_OK) == 0) { unlink("/tmp/kppmalloc.req"); dump(); }
}

void *malloc(size_t n) {
    if (likely(n <= MAXREQ)) { /* bins are empty unless enabled, so no separate 'on' test is needed here */
        unsigned idx = REQIDX(n);
        tc_t *t = &tc;
        void *p = t->head[idx];
        if (p) {
            t->head[idx] = *(void **)p;
            t->cnt[idx]--;
            if (unlikely(stats_on)) st_hit++;
            return p;
        }
        if (unlikely(stats_on)) st_miss++;
    } else if (unlikely(stats_on)) st_big++;
    return __libc_malloc(n);
}

/* everything but "thread ready and bin not full": first free of a thread, bin full, stats tick */
static __attribute__((noinline)) void free_slow(tc_t *t, void *p, unsigned idx) {
    if (t->state == T_NEW && thread_init(t) && t->cnt[idx] < lim[idx]) {
        *(void **)p = t->head[idx];
        t->head[idx] = p;
        t->cnt[idx]++;
        if (unlikely(stats_on)) st_fcache++;
        return;
    }
    if (unlikely(stats_on)) { if (t->state == T_READY) st_ffull++; else st_fpass++; }
    __libc_free(p);
}

void free(void *p) {
    if (unlikely(!p)) return;
    if (likely(on)) {
        size_t h = ((size_t *)p)[-1];
        size_t cs = h & ~(size_t)7;
        if (!(h & 2) && cs - 16 <= MAXCS - 16) {
            unsigned idx = cs >> 3;
            tc_t *t = &tc;
            unsigned c = t->cnt[idx];
            if (likely(t->state == T_READY) && likely(c < lim[idx])) {
                *(void **)p = t->head[idx];
                t->head[idx] = p;
                t->cnt[idx] = c + 1;
                if (unlikely(stats_on)) { if ((++st_fcache & 0xfff) == 0) stats_tick(); }
                return;
            }
            free_slow(t, p, idx);
            return;
        }
    }
    if (unlikely(stats_on)) st_fpass++;
    __libc_free(p);
}

void *calloc(size_t a, size_t b) {
    size_t n;
    if (likely(!__builtin_mul_overflow(a, b, &n)) && n <= MAXREQ) {
        unsigned idx = REQIDX(n);
        tc_t *t = &tc;
        void *p = t->head[idx];
        if (p) {
            t->head[idx] = *(void **)p;
            t->cnt[idx]--;
            if (unlikely(stats_on)) st_calloc_hit++;
            return memset(p, 0, n);
        }
        if (unlikely(stats_on)) st_calloc_miss++;
    }
    return __libc_calloc(a, b);
}

static void dump(void) {
    char path[64], buf[512];
    int n, fd;
    snprintf(path, sizeof path, "/tmp/kppmalloc.%d.out", (int)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    n = snprintf(buf, sizeof buf,
                 "malloc_hit %lu\nmalloc_miss %lu\nmalloc_big %lu\ncalloc_hit %lu\ncalloc_miss %lu\nfree_cached %lu\nfree_binfull %lu\nfree_pass %lu\nflushed %lu\nlimits %u,%u,%u\n",
                 st_hit, st_miss, st_big, st_calloc_hit, st_calloc_miss, st_fcache, st_ffull, st_fpass, st_flush,
                 lim[2], lim[256 / 8], lim[NX - 1]);
    if (write(fd, buf, n) < 0) {}
    close(fd);
}

/* exit dump: a destructor instead of atexit() (atexit lives in libc_nonshared.a, which -nostdlib does not link) */
__attribute__((destructor)) static void fini(void) { if (stats_on) dump(); }

__attribute__((constructor)) static void init(void) {
    char exe[256];
    const char *e = getenv("KPPMALLOC");
    ssize_t k;
    if (!e || e[0] != '1') return;
    if (access("/mnt/us/DISABLE_KPPFAST", F_OK) == 0) return;
    k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return;
    exe[k] = 0;
    if (!strstr(exe, "KPPMainApp") && !getenv("KPPMALLOC_ANYEXE")) return;
    {
        unsigned l[3] = {32, 16, 8}, i;
        e = getenv("KPPMALLOC_LIMITS");
        for (i = 0; e && *e && i < 3; i++) { /* "a,b,c"; no sscanf (would need GLIBC_2.7 __isoc99_sscanf) */
            unsigned v = 0;
            while (*e >= '0' && *e <= '9') v = v * 10 + (unsigned)(*e++ - '0');
            l[i] = v;
            if (*e == ',') e++; else break;
        }
        for (i = 0; i < 3; i++) if (l[i] > 4096) l[i] = 4096;
        for (i = 2; i < NX; i++) lim[i] = i <= 128 / 8 ? l[0] : i <= 512 / 8 ? l[1] : l[2];
    }
    e = getenv("KPPMALLOC_STATS");
    if (e && e[0] == '1') stats_on = 1;
    on = 1;
}
