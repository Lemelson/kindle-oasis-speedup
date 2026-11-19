/* Unit tests for libkpphot against the firmware's own libraries (run inside kindle-emu-rootfs:5.18.2, qemu-arm):
 *   gray  : original gdk_rgb_convert_gray8 (libgdk 0x29538) vs hot_gdk_gray8 on random cases, then the patch is
 *           applied through the real framework and the patched entry is called again (must give the same bytes).
 *   cairo : hot_cairo_get vs the original per-pixel semantics with libX11's real _XGetPixel8 (0x20870) on fake
 *           XImages / frames / visual_info colour tables; plus the fallback conditions.
 * usage: hottest [N]   -> prints PASS/FAIL counts, exit 0 if all pass */
#include "kpphot.c"
#include <dlfcn.h>

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static void fill(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = rnd(); }

typedef void (*gray_fn)(void *, uint8_t *, int, int, int, int, const uint8_t *, int, int, int, void *);

static int test_gray(int n, uintptr_t gdkbase, int patched_pass) {
    gray_fn orig = (gray_fn)(gdkbase + 0x29538);
    int bad = 0;
    for (int t = 0; t < n; t++) {
        int w = rnd() % 720, h = rnd() % 9, x0 = rnd() % 40, y0 = rnd() % 5;
        if (t % 50 == 0) { w = -(int)(rnd() % 4); }        /* non-positive sizes */
        if (t % 77 == 0) { h = -(int)(rnd() % 3); }
        int ww = w > 0 ? w : 0, hh = h > 0 ? h : 0;
        int rowstride = 3 * ww + rnd() % 13;
        int bpl = x0 + ww + rnd() % 17; if (bpl > 65535) bpl = 65535;
        size_t srcn = (size_t)rowstride * (hh + 1) + 64, dstn = (size_t)bpl * (y0 + hh + 1) + 64;
        uint8_t *src = malloc(srcn), *d1 = malloc(dstn), *d2 = malloc(dstn);
        uint8_t img1[64], img2[64];
        fill(src, srcn); fill(d1, dstn); memcpy(d2, d1, dstn);
        memset(img1, 0xa5, 64); *(uint16_t *)(img1 + 36) = bpl; memcpy(img2, img1, 64);
        *(uint8_t **)(img1 + 40) = d1; *(uint8_t **)(img2 + 40) = d2;
        int off = rnd() % 3;   /* unaligned source */
        if (!patched_pass) orig((void *)1, img1, x0, y0, w, h, src + off, rowstride, 0, 0, 0);
        else hot_gdk_gray8((void *)1, img1, x0, y0, w, h, src + off, rowstride, 0, 0, 0);
        /* second call goes through the libgdk entry: original code in pass 0 (compare hot vs orig),
           patched jump in pass 1 */
        if (!patched_pass) hot_gdk_gray8((void *)1, img2, x0, y0, w, h, src + off, rowstride, 0, 0, 0);
        else orig((void *)1, img2, x0, y0, w, h, src + off, rowstride, 0, 0, 0);
        if (memcmp(d1, d2, dstn) || memcmp(img1, img2, 40)) { bad++; if (bad < 5) printf("gray mismatch t=%d w=%d h=%d\n", t, w, h); }
        free(src); free(d1); free(d2);
    }
    return bad;
}

static int test_cairo(int n, uintptr_t x11base) {
    getpix_fn gp8 = (getpix_fn)(x11base + 0x20870);
    int bad = 0, fastn = 0, slown = 0;
    x11_getpixel8 = (uintptr_t)gp8;
    for (int t = 0; t < n; t++) {
        int w = rnd() % 400, h = rnd() % 12;
        if (t % 40 == 0) w = 0;
        if (t % 41 == 0) h = 0;
        int bpl = w + rnd() % 9, stride = (w * 4 + (rnd() % 3) * 4);
        uint8_t xi[96], vi[12 + 1024 + 64], frame[256], *fp = frame + 200;
        uint8_t *src = malloc((size_t)bpl * (h + 1) + 16), *dst = malloc((size_t)stride * (h + 1) + 16);
        fill(src, (size_t)bpl * (h + 1) + 16); fill(vi, sizeof vi); fill(dst, (size_t)stride * (h + 1) + 16);
        if (t % 3 == 0) for (int i = 0; i < 256; i++) { vi[12 + 4 * i + 1] = vi[12 + 4 * i + 2] = vi[12 + 4 * i + 3] = i; }
        memset(xi, 0, sizeof xi);
        *(int *)(xi + 0) = w; *(int *)(xi + 4) = h; *(int *)(xi + 12) = 2; *(uint8_t **)(xi + 16) = src;
        *(int *)(xi + 36) = 8; *(int *)(xi + 40) = bpl; *(int *)(xi + 44) = 8; *(uintptr_t *)(xi + 72) = (uintptr_t)gp8;
        int expect_slow = 0;
        switch (t % 23) {   /* fallback conditions */
        case 1: *(int *)(xi + 36) = 4; expect_slow = 1; break;          /* depth != 8 */
        case 2: *(int *)(xi + 12) = 1; expect_slow = 1; break;          /* XYPixmap */
        case 3: *(uintptr_t *)(xi + 72) = 0x1234; expect_slow = 1; break; /* foreign get_pixel */
        case 4: *(int *)(xi + 44) = 16; expect_slow = 1; break;
        }
        memset(frame, 0x5a, sizeof frame);
        *(uint8_t **)(fp - 84) = (t % 29 == 5) ? NULL : vi; if (t % 29 == 5) expect_slow = 1;
        *(uint8_t **)(fp - 128) = dst; *(int *)(fp - 152) = stride; *(int *)(fp - 136) = 0;
        uint8_t *ref = malloc((size_t)stride * (h + 1) + 16);
        memcpy(ref, dst, (size_t)stride * (h + 1) + 16);
        /* reference: the original loop's semantics with the real get_pixel */
        if (!expect_slow) {
            uint8_t *row = ref;
            for (int y = 0; y < h; y++, row += stride)
                for (int x = 0; x < w; x++) {
                    uint32_t p = (uint32_t)gp8(xi, x, y) & 0xff;
                    ((uint32_t *)row)[x] = (uint32_t)vi[13 + 4 * p] << 16 | (uint32_t)vi[14 + 4 * p] << 8 | vi[15 + 4 * p];
                }
        }
        uint8_t fcopy[256]; memcpy(fcopy, frame, 256);
        int r = hot_cairo_get(fp, xi);
        if (r != !expect_slow) { bad++; printf("cairo: wrong path t=%d r=%d\n", t, r); }
        if (r) {
            fastn++;
            if (memcmp(ref, dst, (size_t)stride * (h + 1) + 16)) { bad++; if (bad < 5) printf("cairo mismatch t=%d w=%d h=%d\n", t, w, h); }
            /* frame: only y slot and row slot may change, to the values the original leaves */
            *(int *)(fcopy + 200 - 136) = h > 0 ? h : 0;
            if (h > 0) *(uint8_t **)(fcopy + 200 - 128) = dst + (size_t)stride * h;
            if (memcmp(fcopy, frame, 256)) { bad++; printf("cairo frame state t=%d\n", t); }
        } else {
            slown++;
            if (memcmp(fcopy, frame, 256) || memcmp(ref, dst, (size_t)stride * (h + 1) + 16)) { bad++; printf("cairo slow path touched state t=%d\n", t); }
        }
        free(src); free(dst); free(ref);
    }
    printf("cairo: fast %d slow %d\n", fastn, slown);
    return bad;
}

#include <math.h>
static int test_sin(int n) {
    double (*ls)(double) = (double (*)(double))dlsym(dlopen("libm.so.6", RTLD_NOW), "sin");
    int bad = 0; union { double d; uint64_t u; } a, b;
    real_sin = ls;
    double pool[64];
    for (int i = 0; i < 64; i++) pool[i] = ((int)(rnd() % 20000) - 10000) * 3.14159265358979 / 1777.0;
    for (int t = 0; t < n * 20; t++) {
        double x;
        switch (t % 5) {
        case 0: x = pool[rnd() % 64]; break;                               /* repeated args -> hits */
        case 1: a.u = ((uint64_t)rnd() << 32) | rnd(); if (t % 97) a.u = (a.u & 0x800fffffffffffffULL) | ((uint64_t)(990 + rnd() % 70) << 52); x = a.d; break; /* any bit pattern (1/97: incl. nan/inf/huge/denormal) */
        case 2: x = (rnd() % 2001 - 1000) * 0.0031415926; break;
        case 3: { static const double sp[] = {0.0, -0.0, 1e-310, -1e-310, 1e300, -1e300, 3.141592653589793, 1.5707963267948966};
                  x = sp[rnd() % 8]; if (rnd() % 5 == 0) { a.u = 0x7ff0000000000000ULL | ((rnd() & 1) ? 1 : 0); x = a.d; } break; }
        default: x = pool[t % 64]; break;
        }
        a.d = hot_sin(x); b.d = ls(x);
        if (a.u != b.u && !(a.d != a.d && b.d != b.d)) { bad++; if (bad < 5) printf("sin mismatch %a: %a vs %a\n", x, a.d, b.d); }
    }
    printf("sin: %d calls, hit=%lu miss=%lu\n", n * 20, st_sin_hit, st_sin_miss);
    return bad;
}

typedef int (*lzdef_fn)(const char *, char *, int, int);
typedef int (*lzdec_fn)(const char *, char *, int, int);
static int test_lz4(int n) {
    void *h = dlopen("libReactNative.so", RTLD_LAZY);
    uintptr_t rb = h ? find_verified("libReactNative.so", RN_LZ4_FSIZE, RN_LZ4_OFF, rn_lz4_bytes, RN_LZ4_LEN) : 0;
    if (!rb) { printf("lz4: libReactNative not verified\n"); return 1; }
    lzdef_fn def = (lzdef_fn)(rb + 0x1e4264 + 1); lzdec_fn dec = (lzdec_fn)(rb + 0x1e6524 + 1);
    int bad = 0, sz1 = 0, szN = 0;
    char *src = malloc(1 << 18), *c1 = malloc(1 << 19), *cN = malloc(1 << 19), *out = malloc(1 << 18);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {   /* apply the patch through the framework */
            setenv("KPPHOT_LZ4ACCEL", "8", 1); setenv("KPPHOT_GDKGRAY", "0", 1); kpphot_init();
            if (*(uint16_t *)(rb + 0x1e4266) != 0x2408) { printf("lz4: patch not applied\n"); return 1; }
        }
        rs = 777;
        for (int t = 0; t < n; t++) {
            int len = rnd() % 100000, kind = t % 4;
            for (int i = 0; i < len; i++)   /* 4-bit-image-like: runs, gradients, noise */
                src[i] = kind == 0 ? (char)rnd() : kind == 1 ? (char)((i / (1 + rnd() % 40)) & 0x77) :
                         kind == 2 ? (char)(((i % 172) / 11) * 0x11) : (char)(rnd() % 3 ? 0xff : rnd());
            int cs = def(src, pass ? cN : c1, len, 1 << 19);
            if (pass) szN += cs; else sz1 += cs;
            memset(out, 0x5a, len + 16);
            int g = dec(pass ? cN : c1, out, cs, len);
            if ((len > 0 && cs <= 0) || g != len || memcmp(out, src, len)) { bad++; if (bad < 5) printf("lz4 round trip bad t=%d len=%d cs=%d g=%d\n", t, len, cs, g); }
        }
    }
    printf("lz4: %d cases x2, accel1 total %d bytes, accel8 total %d bytes\n", n, sz1, szN);
    return bad;
}

typedef int (*idiv_fn)(int, int);
typedef unsigned long long (*idivmod_fn)(int, int);
static int test_pixdiv(int n) {
    void *h = dlopen("libpixman-1.so.0", RTLD_NOW);
    uintptr_t pb = h ? find_verified("libpixman-1.so.0", PIX_DIV_FSIZE, PIX_DIV_OFF, pix_div_bytes, PIX_DIV_LEN) : 0;
    if (!pb) { printf("pixdiv: pixman not verified\n"); return 1; }
    idiv_fn dv = (idiv_fn)(pb + 0x41108); idivmod_fn dm = (idivmod_fn)(pb + 0x411e4);
    int cnt = n * 50, *A = malloc(cnt * 4 * sizeof(int)), bad = 0;
    unsigned long long *Q = malloc(cnt * sizeof(unsigned long long));
    int *R = malloc(cnt * sizeof(int));
    static const int sp[] = {0, 1, -1, 2, -2, 3, 0xf0f, -0xf0f, 0x7fffffff, (int)0x80000000, 0x7ffffffe, (int)0x80000001, 65536, -65536, 255, 1928};
    for (int i = 0; i < cnt; i++) {
        int a, b;
        switch (i % 4) {
        case 0: a = (int)rnd(); b = (int)rnd(); break;
        case 1: a = (int)rnd(); b = (int)(rnd() % 4096) - 2048; break;
        case 2: a = sp[rnd() % 16]; b = sp[rnd() % 16]; break;
        default: a = (int)(rnd() & 0xffff) + 1928; b = 0xf0f; break;
        }
        if (b == 0) b = (i & 1) ? 7 : -7;   /* division by zero path is left untouched by the patch */
        A[2 * i] = a; A[2 * i + 1] = b;
        R[i] = dv(a, b); Q[i] = dm(a, b);   /* original routines */
    }
    setenv("KPPHOT_PIXDIV", "1", 1); setenv("KPPHOT_LZ4ACCEL", "0", 1); kpphot_init();
    if (*(uint32_t *)(pb + 0x41110) != 0xe710f110u) { printf("pixdiv: patch not applied\n"); return 1; }
    for (int i = 0; i < cnt; i++) {
        int a = A[2 * i], b = A[2 * i + 1];
        if (dv(a, b) != R[i] || dm(a, b) != Q[i]) { bad++; if (bad < 5) printf("pixdiv mismatch %d / %d\n", a, b); }
    }
    printf("pixdiv: %d pairs (idiv + idivmod) before/after patch\n", cnt);
    return bad;
}

typedef struct { long long q, r; } lres;
static lres call_ldivmod(uintptr_t f, long long a, long long b) {   /* AAPCS: a r0:r1, b r2:r3 -> q r0:r1, r r2:r3 */
    register long long ra __asm__("r0") = a; register long long rb __asm__("r2") = b;
    __asm__ volatile("blx %4" : "+r"(ra), "+r"(rb) : "0"(ra), "1"(rb), "r"(f) : "ip", "lr", "cc", "memory");
    lres x = { ra, rb }; return x;
}
static int test_pixldiv(int n) {
    uintptr_t pb = find_verified("libpixman-1.so.0", PIX_LDIV_FSIZE, PIX_LDIV_OFF, pix_ldiv_bytes, PIX_LDIV_LEN);
    if (!pb) { printf("pixldiv: not verified\n"); return 1; }
    int cnt = n * 50, bad = 0, fast = 0;
    long long *A = malloc(cnt * 2 * sizeof(long long)); lres *R = malloc(cnt * sizeof(lres));
    static const long long sp[] = {0, 1, -1, 2, -3, 0x7fffffffLL, -0x80000000LL, 0x80000000LL, -0x80000001LL, 65536, -65536,
                                   0x7fffffffffffffffLL, (long long)0x8000000000000000ULL, 0x100000000LL, -0x100000000LL, 3855};
    for (int i = 0; i < cnt; i++) {
        long long a, b;
        switch (i % 5) {
        case 0: a = (int)rnd(); b = (int)rnd(); break;
        case 1: a = (long long)(int)rnd() * (int)(rnd() % 64); b = (int)(rnd() % 100000) - 50000; break;
        case 2: a = sp[rnd() % 16]; b = sp[rnd() % 16]; break;
        case 3: a = ((long long)rnd() << 32) | rnd(); b = ((long long)(int)rnd() << (rnd() % 32)); break;
        default: a = (int)rnd() >> (rnd() % 31); b = (int)(rnd() >> (rnd() % 31)) | 1; break;
        }
        if (b == 0) b = 5;   /* divide-by-zero path calls raise(SIGFPE) in both versions: not exercised */
        if (a == b && 0) a++;
        A[2 * i] = a; A[2 * i + 1] = b;
        R[i] = call_ldivmod(pb + 0x415bc, a, b);
        if (a == (int)a && b == (int)b && !(a == -0x80000000LL && b == -1)) fast++;
    }
    setenv("KPPHOT_PIXLDIV", "1", 1); kpphot_init();
    if (*(uint32_t *)(pb + 0x415bc) != 0xe51ff004u) { printf("pixldiv: patch not applied\n"); return 1; }
    for (int i = 0; i < cnt; i++) {
        lres x = call_ldivmod(pb + 0x415bc, A[2 * i], A[2 * i + 1]);
        if (x.q != R[i].q || x.r != R[i].r) { bad++; if (bad < 5) printf("pixldiv mismatch %lld / %lld\n", A[2 * i], A[2 * i + 1]); }
    }
    printf("pixldiv: %d pairs, %d on the fast path\n", cnt, fast);
    return bad;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 2000, bad = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    void *g = dlopen("libgdk-x11-2.0.so.0", RTLD_NOW), *x = dlopen("libX11.so.6", RTLD_NOW);
    if (!g || !x) { printf("dlopen failed\n"); return 2; }
    uintptr_t gb = find_verified("libgdk-x11-2.0.so.0", GDK_GRAY8_FSIZE, GDK_GRAY8_OFF, gdk_gray8_bytes, GDK_GRAY8_LEN);
    uintptr_t xb = find_verified("libX11.so.6", X11_GETPIXEL8_FSIZE, X11_GETPIXEL8_OFF, x11_getpixel8_bytes, X11_GETPIXEL8_LEN);
    printf("gdk base %lx x11 base %lx\n", (unsigned long)gb, (unsigned long)xb);
    if (!gb || !xb) return 2;
    int b1 = test_gray(n, gb, 0);
    printf("gray orig-vs-hot: %d cases, %d bad\n", n, b1);
    setenv("KPPHOT_GDKGRAY", "1", 1); setenv("KPPHOT_LOG", "/dev/stdout", 1);
    kpphot_init();
    if (memcmp((void *)(gb + 0x29538), gdk_gray8_bytes, 8) == 0) { printf("patch NOT applied\n"); bad++; }
    int b2 = test_gray(n, gb, 1);
    printf("gray via patched entry: %d cases, %d bad\n", n, b2);
    int b3 = test_cairo(n, xb);
    printf("cairo: %d cases, %d bad\n", n, b3);
    int b4 = test_sin(n);
    printf("sin: %d bad\n", b4);
    int b5 = getenv("LZ4TEST") ? test_lz4(300) : 0;
    printf("lz4: %d bad\n", b5);
    int b6 = test_pixdiv(2000);
    printf("pixdiv: %d bad\n", b6);
    int b7 = test_pixldiv(2000);
    printf("pixldiv: %d bad\n", b7);
    bad += b1 + b2 + b3 + b4 + b5 + b6 + b7;
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad != 0;
}
void _start(void) { exit(main(0, 0)); }
