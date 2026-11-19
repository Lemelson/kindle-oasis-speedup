/*
 * libkppprof.so: CPU sampling profiler for KPPMainApp, loaded with LD_PRELOAD (via kppfast preload.list).
 *
 * Active only when KPPPROF=1 is in the environment (kppfast env file). ITIMER_PROF fires every 10 ms of
 * process CPU time (all threads); the kernel delivers SIGPROF to the thread that was running, so every sample
 * is real CPU time. The handler stores {time, tid, pc, lr, sp-word} into a lock-free ring buffer; a writer
 * thread appends new samples to /tmp/kppprof/<pid>.bin once a second and refreshes <pid>.maps and
 * <pid>.threads. Kernel time inside a syscall shows up as the libc wrapper's PC (the signal is taken on
 * return to user space) - that is the right attribution for "this thread spent CPU in that call".
 *
 * Read-only for the app: no allocation in the handler, no interposed functions. Off without KPPPROF=1.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <dirent.h>

typedef struct { uint32_t t_us, tid, pc, lr; } Sample;   /* t_us: CLOCK_MONOTONIC microseconds (wraps ~71 min) */

#define NBUF 65536
static Sample g_buf[NBUF];
static volatile uint32_t g_head;     /* next write slot (monotonic counter) */
static uint32_t g_tail;              /* written up to here (writer thread only) */
static pid_t g_pid;
static char g_dir[64] = "/tmp/kppprof";

static void on_prof(int sig, siginfo_t *si, void *uc_)
{
    (void)sig; (void)si;
    ucontext_t *uc = uc_;
    int e = errno;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t i = __sync_fetch_and_add(&g_head, 1);
    Sample *s = &g_buf[i % NBUF];
    s->t_us = (uint32_t)(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000);
    s->tid = (uint32_t)syscall(SYS_gettid);
    s->pc = (uint32_t)uc->uc_mcontext.arm_pc;
    s->lr = (uint32_t)uc->uc_mcontext.arm_lr;
    errno = e;
}

static void copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY); if (in < 0) return;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); return; }
    char b[8192]; ssize_t n;
    while ((n = read(in, b, sizeof b)) > 0) if (write(out, b, n) != n) break;
    close(in); close(out);
}

static void dump_threads(void)
{
    char p[96]; snprintf(p, sizeof p, "%s/%d.threads", g_dir, g_pid);
    FILE *f = fopen(p, "w"); if (!f) return;
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char q[96], c[64] = ""; snprintf(q, sizeof q, "/proc/self/task/%s/comm", e->d_name);
        FILE *g = fopen(q, "r"); if (g) { if (fgets(c, sizeof c, g)) c[strcspn(c, "\n")] = 0; fclose(g); }
        fprintf(f, "%s %s\n", e->d_name, c);
    }
    if (d) closedir(d);
    fclose(f);
}

static void *writer(void *a)
{
    (void)a;
    char p[96]; snprintf(p, sizeof p, "%s/%d.bin", g_dir, g_pid);
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return NULL;
    for (int k = 0;; k++) {
        sleep(1);
        uint32_t h = g_head;
        if (h - g_tail > NBUF) g_tail = h - NBUF;          /* overrun: drop the oldest */
        while (g_tail != h) {
            uint32_t i = g_tail % NBUF, n = h - g_tail;
            if (n > NBUF - i) n = NBUF - i;
            if (write(fd, &g_buf[i], n * sizeof(Sample)) < 0) break;
            g_tail += n;
        }
        if (k % 5 == 0) {   /* maps/threads change rarely (dlopen, thread pools) */
            char m[96]; snprintf(m, sizeof m, "%s/%d.maps", g_dir, g_pid); copy_file("/proc/self/maps", m);
            dump_threads();
        }
    }
    return NULL;
}

__attribute__((constructor)) static void kppprof_init(void)
{
    const char *on = getenv("KPPPROF");
    if (!on || strcmp(on, "1")) return;
    const char *d = getenv("KPPPROF_DIR"); if (d && *d) snprintf(g_dir, sizeof g_dir, "%s", d);
    mkdir(g_dir, 0755);
    g_pid = getpid();
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL)) return;
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, writer, NULL)) return;
    int us = 10000; const char *iv = getenv("KPPPROF_US"); if (iv) us = atoi(iv);
    struct itimerval it = { { 0, us }, { 0, us } };
    setitimer(ITIMER_PROF, &it, NULL);
}
