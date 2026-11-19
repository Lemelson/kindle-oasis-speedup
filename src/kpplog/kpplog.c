/* libkpplog.so (kpp5): ONE switch for KPPMainApp's own log spam.
 *
 *   KPPLOG=1 and no file /var/local/kppfast/verbose-logs  -> spam dropped (this process only)
 *   KPPLOG=2: as 1, plus the freeze-detection status lines (CRITICAL level, ~60 per page turn; the alarm itself passes)
 *   file /var/local/kppfast/verbose-logs present            -> everything is logged exactly as stock
 * The file is looked at when the process starts and again every 256 log calls, so `touch`/`rm` takes effect within a
 * page turn or two, without a restart.
 *
 * What is dropped (KPPMainApp process only; other processes - the reader's cvm, rsd, ... - are never touched, the
 * library is inert outside KPPMainApp):
 *   - kndk::log::log(level, tag, fmt, ...) with level >= WARNING (3 WARNING, 4.. INFO/DEBUG/...): returns before any
 *     formatting (the stock function formats twice and then calls syslog). CRITICAL (1), ERROR (2) and level 0 pass.
 *   - syslog/vsyslog/__syslog_chk/__vsyslog_chk with priority >= LOG_WARNING (4): WARNING, NOTICE, INFO, DEBUG.
 *     EMERG..ERR pass.
 *   - at any level: the known per-item noise lines (weblab trigger records, isColorEnabledFailed, LIPC conn counters).
 * Never dropped: anything whose tag/format/first string argument mentions ReadingTimerController, OpenBook, CloseBook
 * (reading statistics parse those lines) or starts with "kpp" (our own libraries' status lines).
 * KPPLOG_STATS=<file>: counters written at exit (diagnostic).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VERBOSE_FILE "/var/local/kppfast/verbose-logs"
#define KNDK_LOG "_ZN4kndk3log3logENS0_8LogLevelEPKcS3_z"

static int g_on;              /* KPPLOG=1|2 in KPPMainApp */
static int g_fd;              /* KPPLOG=2: also drop the freeze-detection status lines (logged as CRITICAL on every UI event) */
static volatile unsigned g_drop_fd;
static volatile int g_quiet;  /* g_on and no verbose file */
static volatile unsigned g_calls, g_drop_kndk, g_drop_sys, g_pass;
static const char *g_stats;
static void *g_real_kndk;
typedef void (*vs_fn)(int, const char *, va_list);
typedef void (*vsc_fn)(int, int, const char *, va_list);
static vs_fn g_real_vs;
static vsc_fn g_real_vsc;

static void recheck(void) { g_quiet = g_on && access(VERBOSE_FILE, F_OK) != 0; }
static inline void tick(void) { if (g_on && (__sync_add_and_fetch(&g_calls, 1) & 255) == 0) recheck(); }

static int keep_text(const char *s) {
    if (!s) return 0;
    if (s[0] == 'k' && s[1] == 'p' && s[2] == 'p') return 1;
    return strstr(s, "ReadingTimerController") || strstr(s, "OpenBook") || strstr(s, "CloseBook");
}
static int noise_text(const char *s) {
    return s && (strstr(s, "record_weblab_trigger") || strstr(s, "Sent trigger request for experiment") ||
                 strstr(s, "isColorEnabledFailed") || strstr(s, "LIPC conn") || strstr(s, "Having high DBUS connections"));
}

__attribute__((constructor)) static void kpplog_init(void) {
    char exe[200]; const char *e = getenv("KPPLOG");
    g_real_vs = (vs_fn)dlsym(RTLD_NEXT, "vsyslog");
    g_real_vsc = (vsc_fn)dlsym(RTLD_NEXT, "__vsyslog_chk");
    if (!e || (strcmp(e, "1") && strcmp(e, "2"))) return;
    g_fd = e[0] == '2';
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    const char *bn = strrchr(exe, '/'); bn = bn ? bn + 1 : exe;
    if (strncmp(bn, "KPPMainApp", 10)) return;
    if (access("/mnt/us/DISABLE_KPPFAST", F_OK) == 0) return;
    g_stats = getenv("KPPLOG_STATS");
    g_on = 1; recheck();
}

__attribute__((destructor)) static void kpplog_fini(void) {
    if (!g_on || !g_stats) return;
    int fd = open(g_stats, O_WRONLY | O_CREAT | O_APPEND, 0644); char b[160];
    if (fd < 0) return;
    int n = snprintf(b, sizeof b, "pid %d quiet=%d dropped kndk=%u syslog=%u freezedet=%u passed=%u\n", (int)getpid(), g_quiet, g_drop_kndk, g_drop_sys, g_drop_fd, g_pass);
    if (write(fd, b, n) < 0) {}
    close(fd);
}

/* ---- kndk::log::log(LogLevel, const char *tag, const char *fmt, ...): variadic, so an ARM-mode trampoline asks the
 * C helper and either returns (drop) or tail-calls the next definition with r0-r3, lr and the stack untouched. ---- */
__attribute__((used, visibility("hidden"))) void *kpplog_decide(int level, const char *tag, const char *fmt) {
    if (!g_real_kndk) g_real_kndk = dlsym(RTLD_NEXT, KNDK_LOG);
    tick();
    if (g_quiet && level >= 3 && !keep_text(tag) && !keep_text(fmt)) { __sync_fetch_and_add(&g_drop_kndk, 1); return 0; }
    return g_real_kndk; /* NULL only if the logging library is not loaded: then there is nothing to call */
}
void kpplog_tramp(void) __asm__(KNDK_LOG);
__attribute__((naked, target("arm"))) void kpplog_tramp(void) {
    __asm__ volatile(
        "push {r0, r1, r2, r3, r4, lr}\n\t"
        "bl kpplog_decide\n\t"
        "mov r12, r0\n\t"
        "pop {r0, r1, r2, r3, r4, lr}\n\t"
        "cmp r12, #0\n\t"
        "bxeq lr\n\t"
        "bx r12\n\t");
}

/* ---- syslog family ---- */
static int drop_sys(int pri, const char *fmt, va_list ap) {
    const char *m = fmt;
    tick();
    if (!g_quiet || !fmt) return 0;
    if (fmt[0] == '%' && fmt[1] == 's' && fmt[2] == 0) { va_list c; va_copy(c, ap); m = va_arg(c, const char *); va_end(c); }
    if (keep_text(fmt) || keep_text(m)) return 0;
    if ((pri & 7) >= 4 || noise_text(m)) { __sync_fetch_and_add(&g_drop_sys, 1); return 1; }
    if (g_fd) {
        /* "NativeThreadFreezeDetection ... Native thread ping back", "TaskQueue ... Most recent tasks", "TaskThread ...
           Starting task from NativeQueue: freezeDetection": ~60 CRITICAL lines per page turn while nothing is wrong.
           The real alarm ("FREEZE DETECTED ... KILLING PROCESS") and anything else always passes. */
        char b[400]; va_list c; va_copy(c, ap); vsnprintf(b, sizeof b, fmt, c); va_end(c);
        if ((strstr(b, "NativeThreadFreezeDetection") || strstr(b, "TaskQueue") || strstr(b, "TaskThread")) &&
            !strstr(b, "FREEZE") && !strstr(b, "KILL") && !strstr(b, "exceed") && !strstr(b, "not respond")) {
            __sync_fetch_and_add(&g_drop_fd, 1); return 1;
        }
    }
    return 0;
}
void vsyslog(int pri, const char *fmt, va_list ap) {
    if (!g_real_vs) g_real_vs = (vs_fn)dlsym(RTLD_NEXT, "vsyslog");
    if (drop_sys(pri, fmt, ap)) return;
    g_pass++; g_real_vs(pri, fmt, ap);
}
void syslog(int pri, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsyslog(pri, fmt, ap); va_end(ap); }
void __vsyslog_chk(int pri, int flag, const char *fmt, va_list ap) {
    if (!g_real_vsc) g_real_vsc = (vsc_fn)dlsym(RTLD_NEXT, "__vsyslog_chk");
    if (drop_sys(pri, fmt, ap)) return;
    g_pass++; g_real_vsc(pri, flag, fmt, ap);
}
void __syslog_chk(int pri, int flag, const char *fmt, ...) { va_list ap; va_start(ap, fmt); __vsyslog_chk(pri, flag, fmt, ap); va_end(ap); }
