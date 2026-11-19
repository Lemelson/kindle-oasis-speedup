/*
 * logtest: exercises kndk::log::log (libKindleNDKLoggingShared.so of FW 5.18.2) through whatever the dynamic linker
 * binds (libkppdb's trampoline when preloaded). syslog()/vsyslog() are defined here, so the library's output lands on
 * stdout. Compare runs: stock, preload KPPDB_QUIETLOG=0, preload KPPDB_QUIETLOG=1 (only the Entry2Attr/"Undefined
 * conversion" lines may disappear).   usage: logtest N
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

void vsyslog(int pri, const char *fmt, va_list ap) { printf("syslog %d: ", pri); vprintf(fmt, ap); putchar('\n'); }
void syslog(int pri, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsyslog(pri, fmt, ap); va_end(ap); }
void __syslog_chk(int pri, int flag, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsyslog(pri, fmt, ap); va_end(ap); }
void __vsyslog_chk(int pri, int flag, const char *fmt, va_list ap) { vsyslog(pri, fmt, ap); }

typedef void (*logfn)(int, const char *, const char *, ...);

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 3;
    void *h = dlopen("libKindleNDKLoggingShared.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    void (*init)(void *) = (void (*)(void *))dlsym(h, "_ZN4kndk3log4InitC1Ev");
    void (*setlvl)(int) = (void (*)(int))dlsym(h, "_ZN4kndk3log11setLogLevelENS0_8LogLevelE");
    static char initobj[64];
    if (init) init(initobj);
    if (setlvl) setlvl(7);
    logfn lg = (logfn)dlsym(RTLD_DEFAULT, "_ZN4kndk3log3logENS0_8LogLevelEPKcS3_z");
    logfn real = (logfn)dlsym(h, "_ZN4kndk3log3logENS0_8LogLevelEPKcS3_z");
    printf("bound to %s\n", lg == real ? "library" : "preload");
    for (int i = 0; i < n; i++) {
        for (int lvl = 0; lvl <= 6; lvl++) {
            lg(lvl, "Entry2Attr<ksdk::content_management::ccdb::sqlmodels::ContentCatalogEntry, AssetAttributeOf<long long int, 16> >", "Undefined conversion");
            lg(lvl, "Entry2Attr<X>", "Undefined conversion %d", 5);           /* different format: must stay */
            lg(lvl, "Other", "Undefined conversion");                         /* different tag: must stay */
            lg(lvl, "Fmt", "i=%d s=%s d=%.3f ll=%lld c=%c i2=%d i3=%d", i, "str", 3.25 * lvl, 1234567890123LL * lvl, 'x', -7, 1 << lvl);
            lg(lvl, "Fmt2", "%s %s %s %s %s %d %d %d %d %d", "a", "b", "c", "d", "e", 1, 2, 3, 4, 5);
        }
    }
    puts("done");
    return 0;
}
