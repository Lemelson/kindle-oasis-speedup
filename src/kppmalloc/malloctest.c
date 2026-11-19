/* malloctest - correctness stress for libkppmalloc.so on the device glibc 2.20 (armv7 hf). Built without crt.
 * Run stock and with LD_PRELOAD=libkppmalloc.so KPPMALLOC=1 KPPMALLOC_ANYEXE=1; lines starting with "R " must be
 * identical between the two runs (deterministic), lines starting with "I " are informational (RSS, timing).
 * 1. chunk-header assumptions (glibc 2.20 32-bit) for n = 0..2000, twice (2nd round is served from the cache)
 * 2. 8 threads x OPS random ops: malloc/calloc/realloc/memalign/posix_memalign/strdup, sizes 0..4000, every block
 *    pattern-filled and verified before free/realloc, calloc checked zero, ~1/3 of frees go through a shared queue
 *    to another thread
 * 3. thread churn: CHURN create/join cycles of a thread that allocates, frees (fills its cache) and exits; RSS check */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

extern char **environ;
static volatile int fails;
#define FAIL(...) do { fails++; if (fails < 20) printf("FAIL " __VA_ARGS__); } while (0)

static size_t req2size(size_t n) { size_t r = (n + 4 + 7) & ~(size_t)7; return r < 16 ? 16 : r; }

static long rss_kb(void) {
    char l[128]; long v = -1; FILE *f = fopen("/proc/self/status", "r");
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) v = atol(l + 6);
    if (f) fclose(f);
    return v;
}

static void test_headers(void) {
    static void *p[2001];
    int round, n, bad = 0, mm = 0;
    for (round = 0; round < 2; round++) {
        for (n = 0; n <= 2000; n++) {
            size_t h, cs;
            p[n] = malloc(n);
            h = ((size_t *)p[n])[-1]; cs = h & ~(size_t)7;
            if ((uintptr_t)p[n] & 7) { bad++; FAIL("align n=%d\n", n); }
            if (h & 2) { mm++; continue; }
            if (cs < req2size(n)) { bad++; FAIL("n=%d chunksize %zu < request2size %zu\n", n, cs, req2size(n)); }
            if (malloc_usable_size(p[n]) != cs - 4) { bad++; FAIL("n=%d usable %zu != cs-4 %zu\n", n, malloc_usable_size(p[n]), cs - 4); }
            memset(p[n], 0xa5, n);
        }
        for (n = 0; n <= 2000; n++) free(p[n]);
    }
    /* mmapped chunk */
    { void *q = malloc(1 << 20); size_t h = ((size_t *)q)[-1];
      if (!(h & 2)) { bad++; FAIL("1 MB block not IS_MMAPPED (h=%zx)\n", h); }
      memset(q, 1, 1 << 20); free(q); }
    /* calloc overflow must fail */
    { void *q = calloc((size_t)1 << 17, (size_t)1 << 16); if (q) { bad++; FAIL("calloc overflow returned %p\n", q); free(q); } }
    { volatile size_t big = SIZE_MAX / 2; void *q = calloc(big, 3); if (q) { bad++; FAIL("calloc overflow2 returned %p\n", q); free(q); } }
    printf("R headers: n=0..2000 x2 checked, mmapped-in-range=%d, bad=%d\n", mm, bad);
}

/* ---- multi-thread stress ---- */
#define NT 8
#define SLOTS 256
#define QN 4096
typedef struct { unsigned char *p; uint32_t n, seed; } blk_t;
static blk_t q[QN]; static int qh, qt;
static pthread_mutex_t qm = PTHREAD_MUTEX_INITIALIZER;
static long OPS = 60000;

static uint32_t rnd(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static void fill(blk_t *b) { uint32_t i, x = b->seed; for (i = 0; i < b->n; i++) { b->p[i] = (unsigned char)(x >> 24); x = x * 1103515245u + 12345u; } }
static int check(const blk_t *b, uint32_t upto) {
    uint32_t i, x = b->seed;
    for (i = 0; i < upto; i++) { if (b->p[i] != (unsigned char)(x >> 24)) return 0; x = x * 1103515245u + 12345u; }
    return 1;
}
static void verify_free(blk_t *b, const char *who) {
    if (!b->p) return;
    if (!check(b, b->n)) FAIL("%s: corrupted block %p n=%u\n", who, (void *)b->p, b->n);
    free(b->p); b->p = 0;
}
static int qpush(blk_t *b) { int ok = 0; pthread_mutex_lock(&qm); if ((qt + 1) % QN != qh) { q[qt] = *b; qt = (qt + 1) % QN; ok = 1; } pthread_mutex_unlock(&qm); return ok; }
static int qpop(blk_t *b) { int ok = 0; pthread_mutex_lock(&qm); if (qh != qt) { *b = q[qh]; qh = (qh + 1) % QN; ok = 1; } pthread_mutex_unlock(&qm); return ok; }

typedef struct { int id; long ops[7]; } targ_t;
static void *stress(void *a) {
    targ_t *ta = a; uint32_t s = 0x9e3779b9u * (ta->id + 1);
    blk_t sl[SLOTS]; long i; int k;
    memset(sl, 0, sizeof sl);
    for (i = 0; i < OPS; i++) {
        uint32_t r = rnd(&s), op = r % 100, k2 = (r >> 8) % SLOTS;
        uint32_t n = (rnd(&s) % 10 < 7) ? rnd(&s) % 1025 : rnd(&s) % 4001;
        blk_t *b = &sl[k2];
        blk_t qb;
        if (op < 10 && qpop(&qb)) { verify_free(&qb, "qfree"); ta->ops[6]++; continue; }
        if (b->p && op < 25) { /* realloc */
            unsigned char *np = realloc(b->p, n);
            if (!np && n) { FAIL("realloc NULL\n"); continue; }
            if (n || np) {
                blk_t t = *b; t.p = np;
                if (!check(&t, n < b->n ? n : b->n)) FAIL("realloc lost data n=%u->%u\n", b->n, n);
                b->p = np; b->n = n; b->seed = rnd(&s); fill(b);
            } else b->p = 0;
            ta->ops[1]++; continue;
        }
        if (b->p) { if (op < 45 && qpush(b)) b->p = 0; else verify_free(b, "free"); }
        b->seed = rnd(&s); b->n = n;
        switch (op % 6) {
        case 0: case 1: b->p = malloc(n); ta->ops[0]++; break;
        case 2: b->p = calloc(1, n); ta->ops[2]++;
                for (k = 0; k < (int)n; k++) if (b->p[k]) { FAIL("calloc not zero n=%u at %d\n", n, k); break; } break;
        case 3: b->p = memalign(8u << (r % 5), n); ta->ops[3]++;
                if ((uintptr_t)b->p & ((8u << (r % 5)) - 1)) FAIL("memalign misaligned\n"); break;
        case 4: { void *pp = 0; if (posix_memalign(&pp, 64, n)) FAIL("posix_memalign\n"); b->p = pp; ta->ops[4]++;
                  if ((uintptr_t)pp & 63) FAIL("posix_memalign misaligned\n"); break; }
        case 5: { char tmp[64]; uint32_t m = n % 60; memset(tmp, 'a' + (r % 26), m); tmp[m] = 0;
                  b->p = (unsigned char *)strdup(tmp); b->n = m + 1; ta->ops[5]++;
                  if (strlen((char *)b->p) != m) FAIL("strdup\n"); break; }
        }
        if (!b->p) { FAIL("alloc NULL n=%u\n", n); continue; }
        if ((uintptr_t)b->p & 7) FAIL("misaligned %p\n", (void *)b->p);
        fill(b);
    }
    for (k = 0; k < SLOTS; k++) verify_free(&sl[k], "final");
    return 0;
}

static void test_stress(void) {
    pthread_t th[NT]; targ_t ta[NT]; int i, j; blk_t b; long tot[7] = {0};
    for (i = 0; i < NT; i++) { memset(&ta[i], 0, sizeof ta[i]); ta[i].id = i; pthread_create(&th[i], 0, stress, &ta[i]); }
    for (i = 0; i < NT; i++) pthread_join(th[i], 0);
    while (qpop(&b)) verify_free(&b, "drain");
    for (i = 0; i < NT; i++) for (j = 0; j < 6; j++) tot[j] += ta[i].ops[j];
    /* thread-local op counts are deterministic only when no queue pop happened at the 10 % slots, so print the
     * per-type totals of the deterministic allocation kinds as info and only the failure count as R */
    printf("I stress: %d threads x %ld ops: malloc=%ld realloc=%ld calloc=%ld memalign=%ld posix_memalign=%ld strdup=%ld\n",
           NT, OPS, tot[0], tot[1], tot[2], tot[3], tot[4], tot[5]);
    printf("R stress: %d threads x %ld ops done, fails=%d\n", NT, OPS, fails);
}

/* ---- thread churn ---- */
static void *churn(void *a) {
    void *v[300]; int i; uint32_t s = (uint32_t)(uintptr_t)a * 2654435761u + 1;
    void *shared = a; /* block from the parent: freed here (cross-thread) */
    for (i = 0; i < 300; i++) { v[i] = malloc(rnd(&s) % 1021); memset(v[i], i, 1); }
    for (i = 0; i < 300; i++) free(v[i]);
    for (i = 0; i < 100; i++) { v[i] = malloc(rnd(&s) % 1021); }
    for (i = 0; i < 100; i++) free(v[i]);
    free(shared);
    return malloc(100); /* freed by the parent */
}
static long CHURN = 2000;
static void test_churn(void) {
    long i, r100 = 0, rend; pthread_t t; void *ret; struct mallinfo m100 = {0}, mend;
    for (i = 0; i < CHURN; i++) {
        void *sh = malloc(48 + i % 500);
        if (pthread_create(&t, 0, churn, sh)) { FAIL("pthread_create %ld\n", i); break; }
        pthread_join(t, &ret); free(ret);
        if (i == 99) { r100 = rss_kb(); m100 = mallinfo(); }
    }
    rend = rss_kb(); mend = mallinfo();
    /* VmRSS under qemu-user includes qemu's own per-thread memory (grows the same stock and preloaded), so the
     * pass criterion is glibc's view: bytes in use (uordblks, all arenas) and arena size must not grow */
    printf("I churn: VmRSS after 100 cycles %ld kB, after %ld cycles %ld kB (growth %ld kB)\n", r100, CHURN, rend, rend - r100);
    printf("I churn: mallinfo in-use %d -> %d B, arena %d -> %d B, mmapped %d -> %d B\n",
           m100.uordblks, mend.uordblks, m100.arena, mend.arena, m100.hblkhd, mend.hblkhd);
    if (mend.uordblks - m100.uordblks > 65536) FAIL("in-use bytes grew by %d over thread churn\n", mend.uordblks - m100.uordblks);
    if (mend.arena - m100.arena > 1 << 20) FAIL("arena grew by %d over thread churn\n", mend.arena - m100.arena);
    printf("R churn: %ld create/join cycles, inuse_growth_ok=%d arena_growth_ok=%d\n", CHURN,
           mend.uordblks - m100.uordblks <= 65536, mend.arena - m100.arena <= 1 << 20);
}

int real_main(int argc, char **argv) {
    struct timespec a, b;
    if (getenv("OPS")) OPS = atol(getenv("OPS"));
    if (getenv("CHURN")) CHURN = atol(getenv("CHURN"));
    clock_gettime(CLOCK_MONOTONIC, &a);
    setvbuf(stdout, 0, _IOLBF, 0);
    test_headers();
    test_stress();
    test_churn();
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("I time %.1f s, LD_PRELOAD=%s\n", (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9, getenv("LD_PRELOAD") ? getenv("LD_PRELOAD") : "-");
    printf("R result: %s\n", fails ? "FAIL" : "PASS");
    if (getenv("KPPMALLOC_STATS")) { /* no crt -> no exit-time destructors; ask for a dump (checked every 65536 cached frees) */
        long k; FILE *rq = fopen("/tmp/kppmalloc.req", "w"); if (rq) fclose(rq); else printf("I cannot create /tmp/kppmalloc.req\n");
        for (k = 0; k < 70000; k++) free(malloc(32));
    }
    return fails != 0;
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
