/* libkppside.so (kpp4/js): per-turn native side effects of KPPMainApp.
 * KPPSIDE_STATS=1: counts + wall/cpu time of liblipc / devcap / syslog calls (by function and thread class), written
 * appended to /tmp/kppside.log every 2 s (cumulative, "T <ms>" line per block). Observation only.
 * Other switches are added below as candidates; all default off.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <syslog.h>
#include <sys/syscall.h>

enum { F_LipcOpen, F_LipcOpenEx, F_LipcOpenNoName, F_LipcClose, F_LipcAccessHasharrayProperty,
       F_LipcAccessHasharrayPropertyWithSender, F_LipcSetStringProperty, F_LipcSetIntProperty, F_LipcGetStringProperty,
       F_LipcGetIntProperty, F_LipcSendEvent, F_LipcAccessPropertyAsync, F_devcap_get_string, F_devcap_get_int,
       F_devcap_is_available, F_devcap_initialize, F_vsyslog, NF };
static const char *names[NF] = {"LipcOpen", "LipcOpenEx", "LipcOpenNoName", "LipcClose", "LipcAccessHasharrayProperty",
    "LipcAccessHasharrayPropertyWithSender", "LipcSetStringProperty", "LipcSetIntProperty", "LipcGetStringProperty",
    "LipcGetIntProperty", "LipcSendEvent", "LipcAccessPropertyAsync", "devcap_get_string", "devcap_get_int",
    "devcap_is_available", "devcap_initialize", "vsyslog"};
enum { T_MAIN, T_JS, T_OTHER, NT };
static unsigned cnt[NF][NT], wall_us[NF][NT], cpu_us[NF][NT];
static unsigned long long now_us(clockid_t c);
static int g_stats, g_trace, g_weblab, g_devcap, g_logdrop;
static unsigned g_dropped, g_reused;
static int g_ntrace;
static char tbuf[400][96];
static void trace(const char *fn, long b, long c) {   /* KPPSIDE_TRACE=1: names of the first 400 calls (ring, no IO here) */
    if (!g_trace) return;
    int i = __sync_fetch_and_add(&g_ntrace, 1) % 400;   /* ring: keeps the last 400 */
    snprintf(tbuf[i], sizeof tbuf[i], "%llu %ld %.24s %.40s %.40s", now_us(CLOCK_MONOTONIC) / 1000, (long)syscall(SYS_gettid), fn,
             b > 65536 ? (const char *)b : "-", c > 65536 ? (const char *)c : "-");
}

static int tclass(void) {
    static __thread int c = -1;
    if (c < 0) {
        char n[17] = {0}; prctl(PR_GET_NAME, n, 0, 0, 0);
        c = syscall(SYS_gettid) == getpid() ? T_MAIN : !strcmp(n, "JSQueue") ? T_JS : T_OTHER;
    }
    return c;
}
static unsigned long long now_us(clockid_t c) {
    struct timespec t; clock_gettime(c, &t); return t.tv_sec * 1000000ULL + t.tv_nsec / 1000;
}

static void *dumper(void *a) {
    prctl(PR_SET_NAME, "kppsidestat", 0, 0, 0);
    for (;;) {
        sleep(2);
        FILE *f = fopen("/tmp/kppside.log", "a");
        if (!f) continue;
        fprintf(f, "T %llu\n", now_us(CLOCK_MONOTONIC) / 1000);
        fprintf(f, "weblab_dropped all %u 0 0\nweblab_pool_reuse all %u 0 0\n", g_dropped, g_reused);
        for (int i = 0; i < NF; i++)
            for (int t = 0; t < NT; t++)
                if (cnt[i][t]) fprintf(f, "%s %s %u %u %u\n", names[i], t == T_MAIN ? "main" : t == T_JS ? "js" : "other",
                                       cnt[i][t], wall_us[i][t], cpu_us[i][t]);
        fclose(f);
        if (g_trace && (f = fopen("/tmp/kppside.trace", "w"))) {
            int n = g_ntrace, s0 = n < 400 ? 0 : n % 400, m = n < 400 ? n : 400;
            for (int i = 0; i < m; i++) fprintf(f, "%s\n", tbuf[(s0 + i) % 400]);
            fclose(f);
        }
    }
    return a;
}

__attribute__((constructor)) static void init(void) {
    const char *e = getenv("KPPSIDE_STATS");
    g_stats = e && !strcmp(e, "1");
    e = getenv("KPPSIDE_TRACE"); g_trace = e && !strcmp(e, "1");
    e = getenv("KPPSIDE_WEBLAB"); g_weblab = e && !strcmp(e, "1");
    e = getenv("KPPSIDE_DEVCAP"); g_devcap = e && !strcmp(e, "1");
    e = getenv("KPPSIDE_LOGDROP"); g_logdrop = e && !strcmp(e, "1");
    if (g_stats) { pthread_t t; pthread_create(&t, 0, dumper, 0); pthread_detach(t); }
}

#define BEGIN unsigned long long w0 = 0, c0 = 0; if (g_stats) { w0 = now_us(CLOCK_MONOTONIC); c0 = now_us(CLOCK_THREAD_CPUTIME_ID); }
#define END(id) if (g_stats) { int t = tclass(); __sync_fetch_and_add(&cnt[id][t], 1); \
    __sync_fetch_and_add(&wall_us[id][t], (unsigned)(now_us(CLOCK_MONOTONIC) - w0)); \
    __sync_fetch_and_add(&cpu_us[id][t], (unsigned)(now_us(CLOCK_THREAD_CPUTIME_ID) - c0)); }

static int trk_get(long h); static void trk_set(long h, int st);
/* generic pass-through for functions with up to 12 word-sized arguments (AAPCS: extra args are harmless) */
typedef long (*fn6)(long, long, long, long, long, long, long, long, long, long, long, long);
#define WRAP(name)                                                                     \
    long name(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j, long k, long l) { \
        static fn6 real; if (!real) real = (fn6)dlsym(RTLD_NEXT, #name);               \
        if (F_##name == F_LipcAccessHasharrayProperty || F_##name == F_LipcSetStringProperty \
            || F_##name == F_LipcSetIntProperty || F_##name == F_LipcGetStringProperty || F_##name == F_LipcGetIntProperty) trace(#name, b, c); \
        if (F_##name == F_LipcAccessPropertyAsync) trace(#name, c, d); \
        if (F_##name == F_LipcOpenNoName || F_##name == F_LipcClose || F_##name == F_devcap_get_string) trace(#name, F_##name == F_devcap_get_string ? a : 0, F_##name == F_LipcClose ? 0 : 0); \
        if (g_weblab && a > 65536 && trk_get(a)) trk_set(a, 2);                       \
        BEGIN long r = real(a, b, c, d, e, f, g, h, i, j, k, l); END(F_##name) return r; }
WRAP(LipcOpen) WRAP(LipcOpenEx)
WRAP(LipcAccessHasharrayPropertyWithSender) WRAP(LipcSetStringProperty) WRAP(LipcSetIntProperty)
WRAP(LipcGetStringProperty) WRAP(LipcGetIntProperty) WRAP(LipcSendEvent) WRAP(LipcAccessPropertyAsync)
WRAP(devcap_get_string) WRAP(devcap_get_int) WRAP(devcap_is_available) WRAP(devcap_initialize)

typedef void (*vs_fn)(int, const char *, va_list);
/* KPPSIDE_LOGDROP=1: drop the per-turn noise lines produced by the paths this library short-circuits */
static int noise(const char *fmt, va_list ap) {
    const char *m = fmt;
    if (!fmt) return 0;
    if (fmt[0] == '%' && fmt[1] == 's' && fmt[2] == 0) { va_list c; va_copy(c, ap); m = va_arg(c, const char *); va_end(c); }
    if (!m) return 0;
    return strstr(m, "record_weblab_trigger") || strstr(m, "Sent trigger request for experiment") ||
           strstr(m, "isColorEnabledFailed") || strstr(m, "LIPC conn") || strstr(m, "Having high DBUS connections");
}
void vsyslog(int pri, const char *fmt, va_list ap) {
    static vs_fn real; if (!real) real = (vs_fn)dlsym(RTLD_NEXT, "vsyslog");
    if (g_logdrop && noise(fmt, ap)) return;
    BEGIN real(pri, fmt, ap); END(F_vsyslog)
}
void syslog(int pri, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsyslog(pri, fmt, ap); va_end(ap); }
typedef void (*vsc_fn)(int, int, const char *, va_list);
void __vsyslog_chk(int pri, int flag, const char *fmt, va_list ap) {
    static vsc_fn real; if (!real) real = (vsc_fn)dlsym(RTLD_NEXT, "__vsyslog_chk");
    if (g_logdrop && noise(fmt, ap)) return;
    BEGIN real(pri, flag, fmt, ap); END(F_vsyslog)
}
void __syslog_chk(int pri, int flag, const char *fmt, ...) { va_list ap; va_start(ap, fmt); __vsyslog_chk(pri, flag, fmt, ap); va_end(ap); }

/* ---- KPPSIDE_WEBLAB=1: weblab exposure records (com.lab126.mobile.weblabTreatments / recordtriggers) are not sent.
 * The answer looks like a successful empty reply (LIPC_OK + a fresh empty hasharray, freed by the caller).
 * Connections opened only for such records go to a small pool instead of being closed, and LipcOpenNoName hands
 * them out again: an idle connection on which nothing was ever sent. Treatment lookups (getexperiments, addweblabs)
 * and everything else are untouched. */
#define WSVC "com.lab126.mobile.weblabTreatments"
#define POOLN 2
#define TRK 64
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static long g_pool[POOLN]; static int g_npool;
static long trk_h[TRK]; static int trk_state[TRK];   /* 1 = fresh, 3 = only dropped records, 2 = used */
static void trk_set(long h, int st) {
    int i, fr = -1;
    pthread_mutex_lock(&g_mx);
    for (i = 0; i < TRK; i++) { if (trk_h[i] == h) break; if (fr < 0 && !trk_h[i]) fr = i; }
    if (i == TRK) i = fr;
    if (i >= 0) { trk_h[i] = h; trk_state[i] = st; }
    pthread_mutex_unlock(&g_mx);
}
static int trk_take(long h) {   /* returns state and forgets the handle */
    int st = 0;
    pthread_mutex_lock(&g_mx);
    for (int i = 0; i < TRK; i++) if (trk_h[i] == h) { st = trk_state[i]; trk_h[i] = 0; break; }
    pthread_mutex_unlock(&g_mx);
    return st;
}
static int trk_get(long h) {
    int st = 0;
    pthread_mutex_lock(&g_mx);
    for (int i = 0; i < TRK; i++) if (trk_h[i] == h) { st = trk_state[i]; break; }
    pthread_mutex_unlock(&g_mx);
    return st;
}

long LipcOpenNoName(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j, long k, long l) {
    static fn6 real; if (!real) real = (fn6)dlsym(RTLD_NEXT, "LipcOpenNoName");
    trace("LipcOpenNoName", 0, 0);
    if (g_weblab) {
        long ph = 0;
        pthread_mutex_lock(&g_mx);
        if (g_npool) ph = g_pool[--g_npool];
        pthread_mutex_unlock(&g_mx);
        if (ph) { __sync_fetch_and_add(&g_reused, 1); trk_set(ph, 1); return ph; }
    }
    BEGIN long r = real(a, b, c, d, e, f, g, h, i, j, k, l); END(F_LipcOpenNoName)
    if (g_weblab && r) trk_set(r, 1);
    return r;
}
long LipcClose(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j, long k, long l) {
    static fn6 real; if (!real) real = (fn6)dlsym(RTLD_NEXT, "LipcClose");
    trace("LipcClose", 0, 0);
    if (g_weblab && a) {
        int st = trk_take(a);
        if (st == 3) {   /* pool only connections that carried nothing but dropped records */
            int kept = 0;
            pthread_mutex_lock(&g_mx);
            if (g_npool < POOLN) { g_pool[g_npool++] = a; kept = 1; }
            pthread_mutex_unlock(&g_mx);
            if (kept) return 0;
        }
    }
    BEGIN long r = real(a, b, c, d, e, f, g, h, i, j, k, l); END(F_LipcClose) return r;
}
long LipcAccessHasharrayProperty(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j, long k, long l) {
    static fn6 real; if (!real) real = (fn6)dlsym(RTLD_NEXT, "LipcAccessHasharrayProperty");
    static long (*hnew)(long);
    trace("LipcAccessHasharrayProperty", b, c);
    if (g_weblab && b && c && !strcmp((const char *)b, WSVC) && !strcmp((const char *)c, "recordtriggers")) {
        if (!hnew) hnew = (long (*)(long))dlsym(RTLD_DEFAULT, "LipcHasharrayNew");
        long out = hnew ? hnew(a) : 0;
        if (out) {
            if (e) *(long *)e = out;
            __sync_fetch_and_add(&g_dropped, 1);
            { int st = trk_get(a); trk_set(a, st == 1 || st == 3 ? 3 : 2); }   /* 3 = used only for dropped records */
            return 0;
        }
    }
    if (g_weblab && a) trk_set(a, 2);
    BEGIN long r = real(a, b, c, d, e, f, g, h, i, j, k, l); END(F_LipcAccessHasharrayProperty) return r;
}

/* ---- KPPSIDE_DEVCAP=1: isColorEnabled() asked once per process, then the same answer without devcap lookup/log */
typedef int (*ice_fn)(void);
int isColorEnabled(void) {
    static ice_fn real; static int cached = -1;
    if (!real) real = (ice_fn)dlsym(RTLD_NEXT, "isColorEnabled");
    if (g_devcap && cached >= 0) return cached;
    int r = real();
    if (g_devcap) cached = r;
    return r;
}
