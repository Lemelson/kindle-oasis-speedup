/* test only: LD_PRELOAD before libkppcc.so. KPPFAULT=write|close|rename|open makes that call fail for paths ending
 * in ".tmp" (the copies' temporary files); KPPFAULT_ERRNO (default 28 ENOSPC). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int is(const char *w) { const char *e = getenv("KPPFAULT"); return e && !strcmp(e, w); }
static int err(void) { const char *e = getenv("KPPFAULT_ERRNO"); errno = e ? atoi(e) : ENOSPC; return -1; }
static int tmpfd(int fd) { char l[64], p[400]; snprintf(l, sizeof l, "/proc/self/fd/%d", fd); ssize_t n = readlink(l, p, sizeof p - 1); if (n < 4) return 0; p[n] = 0; return !strcmp(p + n - 4, ".tmp"); }
static int tmpp(const char *p) { size_t n = strlen(p); return n > 4 && !strcmp(p + n - 4, ".tmp"); }
ssize_t write(int fd, const void *b, size_t n) {
    static ssize_t (*r)(int, const void *, size_t); if (!r) r = dlsym(RTLD_NEXT, "write");
    if (is("write") && tmpfd(fd)) { if (n > 1000) r(fd, b, 1000); return err(); }   /* partial file, then the error */
    return r(fd, b, n);
}
int close(int fd) {
    static int (*r)(int); if (!r) r = dlsym(RTLD_NEXT, "close");
    if (is("close") && tmpfd(fd)) { r(fd); return err(); }
    return r(fd);
}
int rename(const char *a, const char *b) {
    static int (*r)(const char *, const char *); if (!r) r = dlsym(RTLD_NEXT, "rename");
    if (is("rename") && tmpp(a)) return err();
    return r(a, b);
}
int open(const char *p, int fl, ...) {
    static int (*r)(const char *, int, ...); if (!r) r = dlsym(RTLD_NEXT, "open");
    va_list ap; va_start(ap, fl); int m = va_arg(ap, int); va_end(ap);
    if (is("open") && tmpp(p)) return err();
    return r(p, fl, m);
}
