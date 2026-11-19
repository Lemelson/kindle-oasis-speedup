/* libkppio.so (kpp7): where does KPPMainApp WAIT for files?  (audit 2: R02, R04, R16, Q07, L12, Q02, A02, A07)
 *
 * The sampling profiler (libkppprof, SIGPROF = CPU time) cannot see a thread that sleeps in read() on the FUSE mount
 * /mnt/us or in a flash write. This library times exactly those calls and prints one line per burst of file activity,
 * with CLOCK_MONOTONIC stamps (same clock as rdprobe2 t0mono_us), so "book open spent N ms blocked in fread of the
 * KFX on thread T" becomes a number instead of a guess.
 *
 *   KPPIO=1            master switch (default off: every hook is a plain tail call to the next definition)
 *   KPPIO_STAT=1       account open/fopen/close/fclose/read, pread, fread, fgets, getc, fseek(o), lseek, write, fsync, mmap of
 *                      files in the classes below; a stdio call that is served from the FILE buffer is only counted
 *   KPPIO_TRACE=n      additionally one line per call on book files (class book/sdr), at most n lines
 *   KPPIO_LOG=file     default /tmp/kppio.log, at most KPPIO_LOG_MAX_KB (default 512) per process, then it stops
 *   KPPIO_GAP_MS=ms    a burst ends after this many ms without a tracked call (default 250)
 * Two read helpers for book files opened read-only with fopen (libYJSDK: FileWrapper = FILE*, every
 * MultiAccessFile::readBytesAt is fseeko + fread; glibc refills the 4 KiB stdio buffer after each seek outside it):
 *   KPPIO_BUF_KB=n     stdio buffer of n KiB (power of two, 8..1024) instead of st_blksize
 *   KPPIO_RA_KB=n      posix_fadvise(WILLNEED) for the first n KiB of the file, issued by a helper thread on a
 *   KPPIO_RA_TAIL_KB=n duplicate descriptor (and for the last n KiB); purely advisory, bytes still come from read()
 * Both are OFF by default and only make sense if the STAT lines show the KFX reads blocking (cold page cache).
 * Returned bytes, counts, errno and EOF behaviour are those of stdio either way (unit test iotest.sh).
 *
 * Classes (absolute path at open time): book = /mnt/us/... with a book suffix (.kfx .azw .azw3 .azw8 .mobi .prc .pdf
 * .epub .kfx-zip .txt), sdr = /mnt/us/....sdr/..., thumb = /mnt/us/system/thumbnails/, us = other /mnt/us,
 * kfxcache = /mnt/kfxcache/, varlocal = /var/local/, rootfs = /usr /opt /app /lib /etc. Everything else (sockets,
 * pipes, /proc, /dev, /tmp, descriptors opened before this library started) is never timed.
 * Only in a process whose executable name starts with KPPMainApp, and not when /mnt/us/DISABLE_KPPFAST exists.
 * Put it FIRST in preload.list: then the times include what the other kpp libs add in the same calls (e.g. the
 * synchronous mirror copy of libkppcc inside fclose). Not seen: page faults of mmap'ed files (the mapping itself is
 * listed), I/O that libc does internally for calls not hooked here (ifstream, opendir, dlopen).
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#undef getc


enum { C_NONE, C_BOOK, C_SDR, C_THUMB, C_US, C_KFXCACHE, C_VARLOCAL, C_ROOTFS, NCLS };
static const char *const g_cname[NCLS] = {"-", "book", "sdr", "thumb", "us", "kfxcache", "varlocal", "rootfs"};
enum { OP_OPEN, OP_CLOSE, OP_READ, OP_SEEK, OP_WRITE, OP_SYNC, OP_MMAP, NOP };
static const char *const g_oname[NOP] = {"open", "close", "read", "seek", "write", "sync", "mmap"};

#define NFD 4096
static unsigned char g_cls[NFD];             /* class of an open descriptor, 0 = not tracked */
static int g_on, g_stat, g_buf, g_ra, g_ra_tail, g_gap_us = 250000, g_trace_left, g_pid;
static long g_log_left = 512 * 1024;
static char g_logpath[200] = "/tmp/kppio.log";
static int g_logfd = -1;

/* ---- next definitions (resolved on first use: a hook can run before our constructor) ---- */
static int (*r_open)(const char *, int, ...), (*r_open64)(const char *, int, ...), (*r_close)(int), (*r_fclose)(FILE *);
static FILE *(*r_fopen)(const char *, const char *), *(*r_fopen64)(const char *, const char *);
static ssize_t (*r_read)(int, void *, size_t), (*r_pread)(int, void *, size_t, off_t), (*r_pread64)(int, void *, size_t, off64_t);
static ssize_t (*r_write)(int, const void *, size_t);
static size_t (*r_fread)(void *, size_t, size_t, FILE *);
static char *(*r_fgets)(char *, int, FILE *);
static int (*r_getc)(FILE *), (*r_fgetc)(FILE *);
static int (*r_fseek)(FILE *, long, int), (*r_fseeko)(FILE *, off_t, int), (*r_fseeko64)(FILE *, off64_t, int);
static off_t (*r_lseek)(int, off_t, int);
static off64_t (*r_lseek64)(int, off64_t, int);
static int (*r_fsync)(int), (*r_fdatasync)(int);
static void *(*r_mmap)(void *, size_t, int, int, int, off_t), *(*r_mmap64)(void *, size_t, int, int, int, off64_t);
#define NEXT(p, name) do { if (__builtin_expect(!(p), 0)) (p) = dlsym(RTLD_NEXT, name); } while (0)

static long long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }

/* ---- bounded spin lock: a hook must never wait forever (signal handler on the same thread, fork child) ---- */
static volatile int g_lock; static unsigned g_dropped;
static int lock_(void) {
    for (int i = 0; i < 4000; i++) { if (!__sync_lock_test_and_set(&g_lock, 1)) return 1; if (i > 50) sched_yield(); }
    __sync_fetch_and_add(&g_dropped, 1); return 0;
}
static void unlock_(void) { __sync_lock_release(&g_lock); }

/* ---- classification ---- */
static int has_suffix(const char *p, size_t n, const char *s) { size_t m = strlen(s); return n >= m && !strcasecmp(p + n - m, s); }
static int classify(const char *p) {
    if (!p || p[0] != '/') return C_NONE;
    size_t n = strlen(p);
    if (!strncmp(p, "/mnt/us/", 8)) {
        if (!strncmp(p, "/mnt/us/system/thumbnails/", 26)) return C_THUMB;
        if (strstr(p, ".sdr/")) return C_SDR;
        static const char *const sfx[] = {".kfx", ".azw", ".azw3", ".azw8", ".mobi", ".prc", ".pdf", ".epub", ".kfx-zip", ".txt", 0};
        for (int i = 0; sfx[i]; i++) if (has_suffix(p, n, sfx[i])) return C_BOOK;
        return C_US;
    }
    if (!strncmp(p, "/mnt/kfxcache/", 14)) return C_KFXCACHE;
    if (!strncmp(p, "/var/local/", 11)) return C_VARLOCAL;
    if (!strncmp(p, "/usr/", 5) || !strncmp(p, "/opt/", 5) || !strncmp(p, "/app/", 5) || !strncmp(p, "/lib/", 5) || !strncmp(p, "/etc/", 5)) return C_ROOTFS;
    return C_NONE;
}
static inline int cls_of(int fd) { return (unsigned)fd < NFD ? g_cls[fd] : 0; }

/* ---- accounting ---- */
struct cst { unsigned n[NOP], nfast; unsigned long long bytes, blk_us, main_us, max_us; };
#define NTH 8
static struct { struct cst c[NCLS]; long long start, last; int active; struct { int tid; unsigned long long us; } th[NTH]; } g_b;
static struct cst g_tot[NCLS];
static unsigned g_fast[NCLS];                /* stdio calls served from the buffer (atomic, folded in at emit) */
static sem_t g_sem; static volatile int g_worker;   /* 0 none, 1 starting/running, -1 failed */
static void worker_start(void);

static void logw(const char *s, int n) {
    if (g_log_left <= 0) return;
    if (g_logfd < 0) { NEXT(r_open, "open"); g_logfd = r_open(g_logpath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644); if (g_logfd < 0) { g_log_left = 0; return; } }
    NEXT(r_write, "write");
    if (n > g_log_left) { static const char f[] = "kppio: log full\n"; r_write(g_logfd, f, sizeof f - 1); g_log_left = 0; return; }
    g_log_left -= n; r_write(g_logfd, s, n);
}
static void logf_(const char *fmt, ...) { char b[400]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a); if (n > 0) logw(b, n < (int)sizeof b ? n : (int)sizeof b - 1); }

static void comm_of(int tid, char *out, int sz) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid); out[0] = '?'; out[1] = 0;
    NEXT(r_open, "open"); NEXT(r_read, "read"); NEXT(r_close, "close");
    int fd = r_open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return;
    ssize_t n = r_read(fd, out, sz - 1); r_close(fd); if (n > 0) { out[n] = 0; out[strcspn(out, "\n")] = 0; }
}
/* lock held */
static void emit_burst(void) {
    if (!g_b.active) return;
    unsigned long long blk = 0; unsigned calls = 0;
    for (int c = 1; c < NCLS; c++) { g_b.c[c].nfast += __sync_lock_test_and_set(&g_fast[c], 0); blk += g_b.c[c].blk_us; for (int o = 0; o < NOP; o++) calls += g_b.c[c].n[o]; calls += g_b.c[c].nfast; }
    if (blk >= 1000 || calls >= 20) {
        logf_("B t=%lld.%03lld dur_ms=%lld blocked_ms=%llu.%llu calls=%u\n", g_b.start / 1000, g_b.start % 1000, (g_b.last - g_b.start) / 1000, blk / 1000, blk % 1000 / 100, calls);
        for (int c = 1; c < NCLS; c++) { struct cst *s = &g_b.c[c]; unsigned k = s->nfast; for (int o = 0; o < NOP; o++) k += s->n[o]; if (!k) continue;
            logf_("  %-8s open=%u close=%u read=%u(+%u buffered) seek=%u write=%u sync=%u mmap=%u bytes=%llu blocked_ms=%llu.%llu main_ms=%llu.%llu max_ms=%llu.%llu\n", g_cname[c],
                  s->n[OP_OPEN], s->n[OP_CLOSE], s->n[OP_READ], s->nfast, s->n[OP_SEEK], s->n[OP_WRITE], s->n[OP_SYNC], s->n[OP_MMAP], s->bytes,
                  s->blk_us / 1000, s->blk_us % 1000 / 100, s->main_us / 1000, s->main_us % 1000 / 100, s->max_us / 1000, s->max_us % 1000 / 100); }
        for (int i = 0; i < NTH && g_b.th[i].tid; i++) { char cm[32]; comm_of(g_b.th[i].tid, cm, sizeof cm);
            logf_("  thread %d %s blocked_ms=%llu.%llu\n", g_b.th[i].tid, cm, g_b.th[i].us / 1000, g_b.th[i].us % 1000 / 100); }
    }
    for (int c = 1; c < NCLS; c++) { struct cst *s = &g_b.c[c], *t = &g_tot[c]; for (int o = 0; o < NOP; o++) t->n[o] += s->n[o];
        t->nfast += s->nfast; t->bytes += s->bytes; t->blk_us += s->blk_us; t->main_us += s->main_us; if (s->max_us > t->max_us) t->max_us = s->max_us; }
    memset(&g_b, 0, sizeof g_b);
}
/* one timed call */
static void account(int c, int op, long long t0, long long t1, long long bytes, int fd, long long off) {
    if (getpid() != g_pid || !lock_()) return;
    if (g_b.active && t0 - g_b.last > g_gap_us) emit_burst();
    int fresh = !g_b.active;
    if (fresh) { g_b.active = 1; g_b.start = t0; }
    if (t1 > g_b.last) g_b.last = t1;
    struct cst *s = &g_b.c[c]; unsigned long long d = t1 > t0 ? t1 - t0 : 0; int tid = syscall(SYS_gettid);
    s->n[op]++; s->blk_us += d; if (bytes > 0) s->bytes += bytes; if (d > s->max_us) s->max_us = d; if (tid == g_pid) s->main_us += d;
    for (int i = 0; i < NTH; i++) { if (!g_b.th[i].tid) g_b.th[i].tid = tid; if (g_b.th[i].tid == tid) { g_b.th[i].us += d; break; } }
    if (g_trace_left > 0 && (c == C_BOOK || c == C_SDR)) { g_trace_left--;
        logf_("T %lld %d %s %s fd=%d off=%lld n=%lld us=%llu\n", t0, tid, g_cname[c], g_oname[op], fd, off, bytes, d); }
    unlock_();
    if (fresh) { if (!g_worker) worker_start(); if (g_worker == 1) sem_post(&g_sem); }
}
static inline void count_fast(int c) { __sync_fetch_and_add(&g_fast[c], 1); }

/* ---- helper thread: ends bursts on time, issues the advisory read-ahead ---- */
#define NRA 16
static struct { int fd; off64_t off, len; } g_raq[NRA]; static unsigned g_ra_n, g_ra_done;
static void ra_drain(void) {
    for (;;) { int fd = -1; off64_t off = 0, len = 0;
        if (!lock_()) return;
        for (int i = 0; i < NRA; i++) if (g_raq[i].fd > 0) { fd = g_raq[i].fd; off = g_raq[i].off; len = g_raq[i].len; g_raq[i].fd = 0; break; }
        unlock_();
        if (fd < 0) return;
        posix_fadvise64(fd, off, len, POSIX_FADV_WILLNEED);
        r_close(fd); __sync_fetch_and_add(&g_ra_done, 1); }
}
static void *worker(void *x) {
    for (;;) {
        while (sem_wait(&g_sem) && errno == EINTR) ;
        ra_drain();
        while (g_stat) { struct timespec ts = {0, 50 * 1000000L}; nanosleep(&ts, 0); ra_drain();
            if (!lock_()) continue;
            int done = !g_b.active || now_us() - g_b.last > g_gap_us; if (done) emit_burst();
            unlock_(); if (done) break; }
    }
    return 0;
}
static void worker_start(void) {
    if (!__sync_bool_compare_and_swap(&g_worker, 0, 2)) return;
    pthread_t t; pthread_attr_t a; sigset_t all, old; int ok = 0;
    NEXT(r_close, "close");
    if (!sem_init(&g_sem, 0, 0) && !pthread_attr_init(&a)) {
        pthread_attr_setstacksize(&a, 256 * 1024); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, &old);
        ok = !pthread_create(&t, &a, worker, 0);
        if (!ok) { pthread_attr_setstacksize(&a, 2048 * 1024); ok = !pthread_create(&t, &a, worker, 0); }   /* large static TLS */
        pthread_sigmask(SIG_SETMASK, &old, 0); pthread_attr_destroy(&a);
    }
    g_worker = ok ? 1 : -1;
}
static void ra_queue(int fd) {
    struct stat64 st; if (syscall(SYS_fstat64, fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) return;
    if (!g_worker) worker_start();
    if (g_worker != 1) return;
    off64_t head = (off64_t)g_ra * 1024, tail = (off64_t)g_ra_tail * 1024, sz = st.st_size;
    struct { off64_t off, len; } w[2]; int nw = 0;
    if (head > 0) { w[nw].off = 0; w[nw].len = head < sz ? head : sz; nw++; }
    if (tail > 0 && sz > head) { off64_t o = sz - tail; if (o < head) o = head; o &= ~4095LL; w[nw].off = o; w[nw].len = sz - o; nw++; }
    for (int k = 0; k < nw; k++) {
        int d = fcntl(fd, F_DUPFD_CLOEXEC, 64); if (d < 0) return;
        int put = 0;
        if (lock_()) { for (int i = 0; i < NRA && !put; i++) if (g_raq[i].fd <= 0) { g_raq[i].fd = d; g_raq[i].off = w[k].off; g_raq[i].len = w[k].len; put = 1; } unlock_(); }
        if (!put) { r_close(d); return; }
        __sync_fetch_and_add(&g_ra_n, 1); sem_post(&g_sem);
    }
}

/* ---- bigger stdio buffer for book files ---- */
#define NBUF 32
static struct { FILE *volatile fp; void *buf; } g_bufs[NBUF];
static void buf_attach(FILE *fp) {
    void *b = malloc((size_t)g_buf * 1024); if (!b) return;
    for (int i = 0; i < NBUF; i++) if (__sync_bool_compare_and_swap(&g_bufs[i].fp, 0, (FILE *)-1)) {
        if (setvbuf(fp, b, _IOFBF, (size_t)g_buf * 1024)) { g_bufs[i].fp = 0; break; }
        g_bufs[i].buf = b; g_bufs[i].fp = fp; return; }
    free(b);
}
static void *buf_detach(FILE *fp) {       /* before the real fclose: the slot must be free before fp can be reused */
    for (int i = 0; i < NBUF; i++) if (g_bufs[i].fp == fp) { void *b = g_bufs[i].buf; g_bufs[i].buf = 0; g_bufs[i].fp = 0; return b; }
    return 0;
}

static void opened_fd(int fd, const char *path, int rdonly, FILE *fp, long long t0) {
    int c = classify(path);
    if ((unsigned)fd >= NFD) return;
    g_cls[fd] = c;
    if (!c) return;
    if (g_stat) account(c, OP_OPEN, t0, now_us(), 0, fd, 0);
    if (c == C_BOOK && rdonly) { if (fp && g_buf) buf_attach(fp); if (g_ra || g_ra_tail) ra_queue(fd); }
}

/* ---- hooks ---- */
#define OPENHOOK(NAME) \
int NAME(const char *path, int flags, ...) { \
    mode_t m = 0; if (flags & (O_CREAT | 020000000)) { va_list a; va_start(a, flags); m = va_arg(a, int); va_end(a); } \
    NEXT(r_##NAME, #NAME); \
    if (!g_on) return r_##NAME(path, flags, m); \
    long long t0 = g_stat ? now_us() : 0; int fd = r_##NAME(path, flags, m); \
    if (fd >= 0) { int e = errno; opened_fd(fd, path, (flags & O_ACCMODE) == O_RDONLY, 0, t0); errno = e; } \
    return fd; }
OPENHOOK(open)
OPENHOOK(open64)

#define FOPENHOOK(NAME) \
FILE *NAME(const char *path, const char *mode) { \
    NEXT(r_##NAME, #NAME); \
    if (!g_on) return r_##NAME(path, mode); \
    long long t0 = g_stat ? now_us() : 0; FILE *fp = r_##NAME(path, mode); \
    if (fp) { int e = errno; opened_fd(fileno(fp), path, mode && mode[0] == 'r' && !strchr(mode, '+'), fp, t0); errno = e; } \
    return fp; }
FOPENHOOK(fopen)
FOPENHOOK(fopen64)

int close(int fd) {
    NEXT(r_close, "close");
    int c = g_on ? cls_of(fd) : 0;
    if (!c) return r_close(fd);
    g_cls[fd] = 0;
    if (!g_stat) return r_close(fd);
    long long t0 = now_us(); int r = r_close(fd); int e = errno; account(c, OP_CLOSE, t0, now_us(), 0, fd, 0); errno = e; return r;
}
int fclose(FILE *fp) {
    NEXT(r_fclose, "fclose");
    if (!g_on || !fp) return r_fclose(fp);
    int fd = fileno(fp), c = cls_of(fd); void *b = g_buf ? buf_detach(fp) : 0;
    if (c) g_cls[fd] = 0;
    long long t0 = c && g_stat ? now_us() : 0;
    int r = r_fclose(fp); int e = errno;
    if (b) free(b);
    if (c && g_stat) account(c, OP_CLOSE, t0, now_us(), 0, fd, 0);
    errno = e; return r;
}

#define TIMED(C, OP, FD, OFF, CALL, RTYPE, BYTES) { long long t0 = now_us(); RTYPE r = CALL; int e = errno; account(C, OP, t0, now_us(), BYTES, FD, OFF); errno = e; return r; }
ssize_t read(int fd, void *buf, size_t n) {
    NEXT(r_read, "read"); int c = g_stat ? cls_of(fd) : 0; if (!c) return r_read(fd, buf, n);
    TIMED(c, OP_READ, fd, -1, r_read(fd, buf, n), ssize_t, r) }
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    NEXT(r_pread, "pread"); int c = g_stat ? cls_of(fd) : 0; if (!c) return r_pread(fd, buf, n, off);
    TIMED(c, OP_READ, fd, off, r_pread(fd, buf, n, off), ssize_t, r) }
ssize_t pread64(int fd, void *buf, size_t n, off64_t off) {
    NEXT(r_pread64, "pread64"); int c = g_stat ? cls_of(fd) : 0; if (!c) return r_pread64(fd, buf, n, off);
    TIMED(c, OP_READ, fd, off, r_pread64(fd, buf, n, off), ssize_t, r) }
ssize_t write(int fd, const void *buf, size_t n) {
    NEXT(r_write, "write"); int c = g_stat ? cls_of(fd) : 0; if (!c) return r_write(fd, buf, n);
    TIMED(c, OP_WRITE, fd, -1, r_write(fd, buf, n), ssize_t, r) }
off_t lseek(int fd, off_t off, int wh) {
    NEXT(r_lseek, "lseek"); int c = g_stat ? cls_of(fd) : 0; if (!c) return r_lseek(fd, off, wh);
    TIMED(c, OP_SEEK, fd, off, r_lseek(fd, off, wh), off_t, 0) }
off64_t lseek64(int fd, off64_t off, int wh) {
    NEXT(r_lseek64, "lseek64"); int c = g_stat ? cls_of(fd) : 0; if (!c) return r_lseek64(fd, off, wh);
    TIMED(c, OP_SEEK, fd, off, r_lseek64(fd, off, wh), off64_t, 0) }
/* fsync is rare and can take long on flash: timed for every descriptor (untracked ones go to class varlocal) */
int fsync(int fd) {
    NEXT(r_fsync, "fsync"); if (!g_stat) return r_fsync(fd);
    int c = cls_of(fd); TIMED(c ? c : C_VARLOCAL, OP_SYNC, fd, -1, r_fsync(fd), int, 0) }
int fdatasync(int fd) {
    NEXT(r_fdatasync, "fdatasync"); if (!g_stat) return r_fdatasync(fd);
    int c = cls_of(fd); TIMED(c ? c : C_VARLOCAL, OP_SYNC, fd, -1, r_fdatasync(fd), int, 0) }

/* stdio: time the call only when it cannot be served from the buffer (then libc reads from the descriptor) */
static inline size_t buffered(FILE *fp) { return fp->_IO_read_end > fp->_IO_read_ptr ? (size_t)(fp->_IO_read_end - fp->_IO_read_ptr) : 0; }
size_t fread(void *p, size_t sz, size_t nm, FILE *fp) {
    NEXT(r_fread, "fread"); int fd, c;
    if (!g_stat || !fp || !(c = cls_of(fd = fileno(fp)))) return r_fread(p, sz, nm, fp);
    if ((unsigned long long)sz * nm <= buffered(fp)) { count_fast(c); return r_fread(p, sz, nm, fp); }
    long long off = g_trace_left > 0 ? ftello64(fp) : -1;
    TIMED(c, OP_READ, fd, off, r_fread(p, sz, nm, fp), size_t, (long long)r * sz) }
char *fgets(char *s, int n, FILE *fp) {
    NEXT(r_fgets, "fgets"); int fd, c;
    if (!g_stat || !fp || !(c = cls_of(fd = fileno(fp)))) return r_fgets(s, n, fp);
    if (buffered(fp) && memchr(fp->_IO_read_ptr, '\n', buffered(fp))) { count_fast(c); return r_fgets(s, n, fp); }
    TIMED(c, OP_READ, fd, -1, r_fgets(s, n, fp), char *, r ? (long long)strlen(r) : 0) }
int getc(FILE *fp) {
    NEXT(r_getc, "getc"); int fd, c;
    if (!g_stat || !fp || !(c = cls_of(fd = fileno(fp)))) return r_getc(fp);
    if (buffered(fp)) { count_fast(c); return r_getc(fp); }
    TIMED(c, OP_READ, fd, -1, r_getc(fp), int, r != EOF) }
int fgetc(FILE *fp) {
    NEXT(r_fgetc, "fgetc"); int fd, c;
    if (!g_stat || !fp || !(c = cls_of(fd = fileno(fp)))) return r_fgetc(fp);
    if (buffered(fp)) { count_fast(c); return r_fgetc(fp); }
    TIMED(c, OP_READ, fd, -1, r_fgetc(fp), int, r != EOF) }
int fseek(FILE *fp, long off, int wh) {
    NEXT(r_fseek, "fseek"); int fd, c;
    if (!g_stat || !fp || !(c = cls_of(fd = fileno(fp)))) return r_fseek(fp, off, wh);
    TIMED(c, OP_SEEK, fd, off, r_fseek(fp, off, wh), int, 0) }
int fseeko(FILE *fp, off_t off, int wh) {
    NEXT(r_fseeko, "fseeko"); int fd, c;
    if (!g_stat || !fp || !(c = cls_of(fd = fileno(fp)))) return r_fseeko(fp, off, wh);
    TIMED(c, OP_SEEK, fd, off, r_fseeko(fp, off, wh), int, 0) }
int fseeko64(FILE *fp, off64_t off, int wh) {
    NEXT(r_fseeko64, "fseeko64"); int fd, c;
    if (!g_stat || !fp || !(c = cls_of(fd = fileno(fp)))) return r_fseeko64(fp, off, wh);
    TIMED(c, OP_SEEK, fd, off, r_fseeko64(fp, off, wh), int, 0) }
void *mmap(void *a, size_t len, int prot, int fl, int fd, off_t off) {
    NEXT(r_mmap, "mmap"); int c = g_stat && fd >= 0 ? cls_of(fd) : 0; if (!c) return r_mmap(a, len, prot, fl, fd, off);
    TIMED(c, OP_MMAP, fd, off, r_mmap(a, len, prot, fl, fd, off), void *, (long long)len) }
void *mmap64(void *a, size_t len, int prot, int fl, int fd, off64_t off) {
    NEXT(r_mmap64, "mmap64"); int c = g_stat && fd >= 0 ? cls_of(fd) : 0; if (!c) return r_mmap64(a, len, prot, fl, fd, off);
    TIMED(c, OP_MMAP, fd, off, r_mmap64(a, len, prot, fl, fd, off), void *, (long long)len) }

static int env_int(const char *k, int lo, int hi) { const char *e = getenv(k); if (!e) return 0; int v = atoi(e); return v >= lo && v <= hi ? v : 0; }
__attribute__((constructor)) static void kppio_init(void) {
    char exe[200]; const char *e = getenv("KPPIO");
    if (!e || strcmp(e, "1")) return;
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); if (n <= 0) return;
    exe[n] = 0; const char *bn = strrchr(exe, '/'); bn = bn ? bn + 1 : exe;
    if (strncmp(bn, "KPPMainApp", 10) || access("/mnt/us/DISABLE_KPPFAST", F_OK) == 0) return;
    NEXT(r_open, "open"); NEXT(r_close, "close"); NEXT(r_read, "read"); NEXT(r_write, "write"); NEXT(r_fclose, "fclose");
    if (!r_open || !r_close || !r_read || !r_write || !r_fclose) return;
    e = getenv("KPPIO_LOG"); if (e && e[0] == '/' && strlen(e) < sizeof g_logpath) strcpy(g_logpath, e);
    int v;
    if ((v = env_int("KPPIO_LOG_MAX_KB", 1, 4096))) g_log_left = v * 1024L;
    if ((v = env_int("KPPIO_GAP_MS", 20, 10000))) g_gap_us = v * 1000;
    g_trace_left = env_int("KPPIO_TRACE", 1, 20000);
    v = env_int("KPPIO_BUF_KB", 8, 1024); if (v && !(v & (v - 1))) g_buf = v;
    g_ra = env_int("KPPIO_RA_KB", 4, 65536); g_ra_tail = env_int("KPPIO_RA_TAIL_KB", 4, 65536);
    g_stat = env_int("KPPIO_STAT", 1, 1);
    g_pid = getpid();
    g_on = g_stat || g_buf || g_ra || g_ra_tail;
    if (g_on && lock_()) { logf_("kppio: pid %d stat=%d trace=%d buf_kb=%d ra_kb=%d ra_tail_kb=%d gap_ms=%d\n", g_pid, g_stat, g_trace_left, g_buf, g_ra, g_ra_tail, g_gap_us / 1000); unlock_(); }
}
__attribute__((destructor)) static void kppio_fini(void) {
    if (!g_on || getpid() != g_pid || !lock_()) return;
    emit_burst();
    for (int c = 1; c < NCLS; c++) { struct cst *s = &g_tot[c]; unsigned k = s->nfast; for (int o = 0; o < NOP; o++) k += s->n[o]; if (!k) continue;
        logf_("TOTAL %-8s open=%u close=%u read=%u(+%u buffered) seek=%u write=%u sync=%u mmap=%u bytes=%llu blocked_ms=%llu main_ms=%llu max_ms=%llu\n", g_cname[c],
              s->n[OP_OPEN], s->n[OP_CLOSE], s->n[OP_READ], s->nfast, s->n[OP_SEEK], s->n[OP_WRITE], s->n[OP_SYNC], s->n[OP_MMAP], s->bytes, s->blk_us / 1000, s->main_us / 1000, s->max_us / 1000); }
    logf_("kppio: exit pid %d, ra queued %u done %u, dropped %u\n", g_pid, g_ra_n, g_ra_done, g_dropped);
    unlock_();
}
