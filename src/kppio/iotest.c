/* kpp7 test driver for libkppio.so (container glibc; run as /tmp/KPPMainAppiotest or under another name).
 * Every mode prints a digest of everything the calls returned (values, errno, bytes), so a run with the library must
 * print exactly what a run without it prints. */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static unsigned long long fnv(unsigned long long h, const void *p, size_t n) { const unsigned char *b = p; while (n--) { h ^= *b++; h *= 1099511628211ULL; } return h; }
#define MIX(v) do { long long v_ = (long long)(v); h = fnv(h, &v_, sizeof v_); } while (0)
static unsigned rnd(unsigned *s) { *s = *s * 1103515245u + 12345u; return *s >> 8; }
static unsigned long long seq(const char *path, unsigned seed, int nops, int use64) {
    unsigned long long h = 1469598103934665603ULL; static __thread unsigned char buf[300000]; char line[300];
    FILE *fp = use64 ? fopen64(path, "rb") : fopen(path, "r"); if (!fp) { MIX(errno); return h; }
    fseeko(fp, 0, SEEK_END); long long sz = ftello(fp); MIX(sz); rewind(fp);
    int fd = open64(path, O_RDONLY);
    for (int i = 0; i < nops; i++) {
        unsigned k = rnd(&seed) % 100; int r; size_t n;
        if (k < 30) { long long off = k < 3 ? sz + (long long)(rnd(&seed) % 5000) : k < 5 ? -(long long)(rnd(&seed) % 9) - 1 : (long long)(rnd(&seed) % (unsigned)sz);
            errno = 0; r = fseeko(fp, off, SEEK_SET); MIX(r); MIX(r ? errno : 0); }
        else if (k < 40) { errno = 0; r = fseeko(fp, (long long)(rnd(&seed) % 9000) - 4500, SEEK_CUR); MIX(r); MIX(r ? errno : 0); }
        else if (k < 44) { errno = 0; r = fseek(fp, -(long)(rnd(&seed) % 70000), SEEK_END); MIX(r); }
        else if (k < 80) { unsigned m = rnd(&seed) % 100; size_t want = m < 60 ? rnd(&seed) % 300 + 1 : m < 90 ? rnd(&seed) % 9000 + 1 : rnd(&seed) % 290000 + 1;
            size_t szs = (rnd(&seed) & 1) ? 1 : 4; n = fread(buf, szs, want / szs, fp); MIX(n); h = fnv(h, buf, n * szs); MIX(feof(fp)); MIX(ferror(fp)); if (feof(fp) && (rnd(&seed) & 1)) clearerr(fp); }
        else if (k < 85) { char *g = fgets(line, sizeof line, fp); MIX(g != 0); if (g) h = fnv(h, line, strlen(line)); }
        else if (k < 90) { int c = getc(fp); MIX(c); c = fgetc(fp); MIX(c); }
        else if (k < 95) { long long t = ftello(fp); MIX(t); }
        else { long long off = rnd(&seed) % (unsigned)(sz + 100); errno = 0; ssize_t g = pread64(fd, buf, rnd(&seed) % 5000, off); MIX(g); if (g > 0) h = fnv(h, buf, g);
            long long l = lseek64(fd, off, SEEK_SET); MIX(l); g = read(fd, buf, 100); MIX(g); if (g > 0) h = fnv(h, buf, g); }
    }
    void *m = mmap(0, 8192, PROT_READ, MAP_PRIVATE, fd, 0); if (m != MAP_FAILED) { h = fnv(h, m, 8192 < sz ? 8192 : sz); munmap(m, 8192); }
    MIX(close(fd)); MIX(fclose(fp)); return h;
}
struct ta { const char *p; unsigned seed; int n; unsigned long long h; };
static void *thr(void *x) { struct ta *a = x; a->h = seq(a->p, a->seed, a->n, a->seed & 1); return 0; }
static long rss_kb(void) { FILE *f = fopen("/proc/self/status", "r"); char l[200]; long v = 0; while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) v = atol(l + 6); if (f) fclose(f); return v; }
int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "", *p = argc > 2 ? argv[2] : "";
    if (!strcmp(m, "seq")) printf("%016llx\n", seq(p, atoi(argv[3]), atoi(argv[4]), argc > 5));
    else if (!strcmp(m, "threads")) { pthread_t t[6]; struct ta a[6]; for (int i = 0; i < 6; i++) { a[i] = (struct ta){p, 100 + i, atoi(argv[3]), 0}; pthread_create(&t[i], 0, thr, &a[i]); }
        for (int i = 0; i < 6; i++) { pthread_join(t[i], 0); printf("%016llx\n", a[i].h); } }
    else if (!strcmp(m, "leak")) { char b[5000]; int n = atoi(argv[3]); for (int i = 0; i < 200; i++) { FILE *f = fopen(p, "rb"); fread(b, 1, sizeof b, f); fclose(f); }
        long r0 = rss_kb(); for (int i = 0; i < n; i++) { FILE *f = fopen(p, "rb"); fseeko(f, i * 37 % 100000, SEEK_SET); fread(b, 1, sizeof b, f); fclose(f); }
        printf("rss growth %ld kB\n", rss_kb() - r0); }
    else if (!strcmp(m, "many")) { FILE *f[48]; char b[100]; unsigned long long h = 0; for (int i = 0; i < 48; i++) { f[i] = fopen(p, "rb"); fseeko(f[i], i * 1000, SEEK_SET); }
        for (int i = 0; i < 48; i++) { size_t n = fread(b, 1, sizeof b, f[i]); h = fnv(h, b, n); } for (int i = 0; i < 48; i++) fclose(f[i]); printf("%016llx\n", h); }
    else if (!strcmp(m, "reuse")) { int fd = open(p, O_RDONLY); char b[16]; read(fd, b, 4); close(fd); int pp[2]; if (pipe(pp)) return 1;
        printf("fd %d pipe %d,%d: ", fd, pp[0], pp[1]); write(pp[1], "ping", 4); ssize_t n = read(pp[0], b, 16); printf("%.*s\n", (int)n, b); close(pp[0]); close(pp[1]); }
    else if (!strcmp(m, "write")) { FILE *f = fopen(p, "wb"); if (!f) { printf("fopen failed %d\n", errno); return 1; } fwrite("0123456789", 1, 10, f); fflush(f); int r = fsync(fileno(f)); fclose(f);
        int fd = open(p, O_WRONLY | O_APPEND); write(fd, "abc", 3); fdatasync(fd); close(fd); f = fopen(p, "r+b"); char b[20]; size_t n = fread(b, 1, 20, f); fclose(f); printf("%d %.*s\n", r, (int)n, b); }
    else if (!strcmp(m, "idle")) { FILE *f = fopen(p, "rb"); char b[70000]; for (int i = 0; i < 30; i++) { fseeko(f, i * 90000, SEEK_SET); fread(b, 1, sizeof b, f); } usleep(atoi(argv[3]) * 1000); fclose(f); _exit(0); }
    return 0;
}

#ifdef KSTART   /* variant linked against the Kindle's glibc 2.20 without crt files (runs in kindle-emu-rootfs) */
extern int __libc_start_main(int (*)(int, char **, char **), int, char **, void (*)(void), void (*)(void), void (*)(void), void *);
void start_c(long *sp, void (*rtld_fini)(void)) { __libc_start_main((int (*)(int, char **, char **))main, (int)sp[0], (char **)(sp + 1), 0, 0, rtld_fini, sp); }
__attribute__((naked, noreturn)) void _start(void) { __asm__ volatile("mov r1, r0\n\tmov r0, sp\n\tmov r2, sp\n\tbic r2, r2, #7\n\tmov sp, r2\n\tbl start_c\n"); }
#endif
