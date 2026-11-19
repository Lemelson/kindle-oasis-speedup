/* libkppimg.so - NEON replacement for imageDecoder::ImageChannel::grayscale2RGB (libImageUtils.so, FW 5.18.2).
 * The stock function expands 8-bit gray to RGB888 with a byte-at-a-time loop that re-reads the image width from
 * memory for every pixel (~12 ms of main-thread CPU per Library page turn on the device, ~1 M pixels).
 * It is an exported symbol called through the PLT (R_ARM_JUMP_SLOT in libImageUtils itself; imported by
 * libReactNative and libKindleNDKImageProcessingShared), so LD_PRELOAD replaces it for all callers.
 *
 * Stock semantics (disassembly 0xa6d0..0xa77a), reproduced exactly:
 *   dst.fmt = 1; dst.w = src.w; dst.h = src.h; dst.stride = (3*w + align-1) & -align;
 *   p = malloc(h * stride); dst.buf = unique_ptr<uchar, void(*)(void*)>(p, free)   (old buffer released by its deleter)
 *   if (!p) return false;
 *   for each row: d[3x] = d[3x+1] = d[3x+2] = s[x]; row padding bytes are left untouched; return true.
 * Switch: KPPIMG=1 enables; otherwise (or with /mnt/us/DISABLE_KPPFAST) the original is called.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <arm_neon.h>

typedef struct {
    uint32_t w, h, stride, fmt;
    void (*del)(void *); /* std::unique_ptr<unsigned char, void(*)(void*)>: deleter first, then pointer */
    uint8_t *data;
} PixBuf;

#define SYM "_ZN12imageDecoder12ImageChannel13grayscale2RGBERKNS_12ImageDecoder6PixBufERS2_i"
typedef int (*g2rgb_fn)(const PixBuf *, PixBuf *, int);
static g2rgb_fn orig;
static int on;

int kpp_gray2rgb(const PixBuf *src, PixBuf *dst, int align) __asm__(SYM);
int kpp_gray2rgb(const PixBuf *src, PixBuf *dst, int align) {
    uint32_t w, h, stride, y;
    uint8_t *p, *old, *d;
    const uint8_t *s;
    if (!on && orig) return orig(src, dst, align);
    w = src->w; h = src->h;
    dst->fmt = 1;
    dst->w = w;
    stride = (w * 3u + (uint32_t)align - 1u) & (uint32_t)(-align);
    dst->h = h;
    dst->stride = stride;
    p = malloc(h * stride);
    old = dst->data;
    dst->data = p;
    if (old) dst->del(old);
    dst->del = free;
    if (!p) return 0;
    s = src->data; d = p;
    for (y = 0; y < h; y++) {
        uint32_t x = 0;
        const uint8_t *sp = s;
        uint8_t *dp = d;
        for (; x + 16 <= w; x += 16, sp += 16, dp += 48) {
            uint8x16x3_t v;
            v.val[0] = vld1q_u8(sp);
            v.val[1] = v.val[0];
            v.val[2] = v.val[0];
            vst3q_u8(dp, v);
        }
        for (; x < w; x++, dp += 3) { uint8_t g = *sp++; dp[0] = g; dp[1] = g; dp[2] = g; }
        s += src->stride;
        d += stride;
    }
    return 1;
}

__attribute__((constructor)) static void init(void) {
    const char *e = getenv("KPPIMG");
    orig = (g2rgb_fn)dlsym(RTLD_NEXT, SYM);
    if (!e || e[0] != '1') return;
    if (access("/mnt/us/DISABLE_KPPFAST", F_OK) == 0) return;
    on = 1;
}
