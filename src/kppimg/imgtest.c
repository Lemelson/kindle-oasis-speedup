/* Parity + speed test: stock grayscale2RGB (libImageUtils.so via RTLD_NEXT) vs libkppimg.so (first in LD_PRELOAD).
 * run: LD_PRELOAD="libkppimg.so /usr/lib/libImageUtils.so" KPPIMG=1 ./imgtest   (inside the 5.18.2 rootfs) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <time.h>
typedef struct { uint32_t w, h, stride, fmt; void (*del)(void *); uint8_t *data; } PixBuf;
#define SYM "_ZN12imageDecoder12ImageChannel13grayscale2RGBERKNS_12ImageDecoder6PixBufERS2_i"
typedef int (*fn)(const PixBuf *, PixBuf *, int);
static unsigned rs = 12345; static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }
static int nfree; static void myfree(void *p) { nfree++; free(p); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(void) {
    fn mine = (fn)dlsym(RTLD_DEFAULT, SYM), orig = (fn)dlsym(RTLD_NEXT, SYM);
    Dl_info a, b; int i, fails = 0, aligns[] = {1, 2, 4, 8, 16, 32, 64};
    if (!mine || !orig) { printf("symbols missing mine=%p orig=%p\n", mine, orig); return 2; }
    /* RTLD_NEXT from the main program = first definition after the executable = same as RTLD_DEFAULT; find the 2nd */
    dladdr(mine, &a);
    { void *h = dlopen("libImageUtils.so", RTLD_NOW); orig = h ? (fn)dlsym(h, SYM) : 0; }
    if (!orig) { printf("no orig\n"); return 2; }
    dladdr(orig, &b);
    printf("mine in %s, orig in %s\n", a.dli_fname, b.dli_fname);
    if (mine == orig) { printf("FAIL: shim not interposed\n"); return 2; }
    for (i = 0; i < 3000; i++) {
        PixBuf s, d1, d2; uint32_t w = i < 40 ? i : rnd() % 700, h = rnd() % 40 + (i % 7 == 0 ? 0 : 1), y, x;
        int al = aligns[rnd() % 7], r1, r2, bad = 0;
        s.w = w; s.h = h; s.stride = w + rnd() % 9; s.fmt = 0; s.del = free; s.data = malloc(s.stride * h + 1);
        for (x = 0; x < s.stride * h; x++) s.data[x] = rnd();
        memset(&d1, 0xee, sizeof d1); memset(&d2, 0xee, sizeof d2);
        d1.del = d2.del = myfree; d1.data = (i & 1) ? malloc(10) : 0; d2.data = (i & 1) ? malloc(10) : 0;
        nfree = 0; r1 = orig(&s, &d1, al); r2 = mine(&s, &d2, al);
        if (nfree != ((i & 1) ? 2 : 0)) bad = 1;
        if (r1 != r2 || d1.w != d2.w || d1.h != d2.h || d1.stride != d2.stride || d1.fmt != d2.fmt || d1.del != d2.del) bad = 1;
        else for (y = 0; y < h && !bad; y++) if (memcmp(d1.data + y * d1.stride, d2.data + y * d2.stride, 3 * w)) bad = 1;
        if (bad) { fails++; if (fails < 5) printf("MISMATCH i=%d w=%u h=%u al=%d r=%d/%d stride=%u/%u del=%p/%p\n", i, w, h, al, r1, r2, d1.stride, d2.stride, (void*)d1.del, (void*)d2.del); }
        d1.del(d1.data); d2.del(d2.data); free(s.data);
    }
    printf("parity: %d cases, %d mismatches; deleter is free: %s\n", i, fails, "checked equal to stock's");
    {   /* speed: one Library cover 343x515, 60 conversions */
        PixBuf s, d; double t0, t1, t2; int k;
        s.w = 343; s.h = 515; s.stride = 344; s.fmt = 0; s.del = free; s.data = malloc(344 * 515);
        memset(s.data, 0x55, 344 * 515); memset(&d, 0, sizeof d);
        for (k = 0; k < 5; k++) { orig(&s, &d, 4); mine(&s, &d, 4); }
        t0 = now(); for (k = 0; k < 60; k++) orig(&s, &d, 4);
        t1 = now(); for (k = 0; k < 60; k++) mine(&s, &d, 4);
        t2 = now();
        printf("speed 343x515 x60: stock %.2f ms/call, kppimg %.2f ms/call, ratio %.1fx\n", (t1 - t0) / 60 * 1e3, (t2 - t1) / 60 * 1e3, (t1 - t0) / (t2 - t1));
    }
    return fails != 0;
}

void _start(void) { exit(main()); }
