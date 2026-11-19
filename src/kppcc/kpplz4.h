/* Fast LZ4 block decoder with the accept/reject rules of LZ4_decompress_safe (LZ4 1.9, full-block decode):
 * returns the number of bytes written, or a negative value for a malformed / truncated / oversized block.
 * Output bytes for a valid block are defined by the format, so they equal the reference decoder's. */
#include <stdint.h>
#include <string.h>
__attribute__((hot, optimize("O3"), target("arm"))) static int kpp_lz4_safe(const uint8_t *src, uint8_t *dst, int srcSize, int dstCap) {
    if (!src || srcSize < 0 || dstCap < 0) return -1;
    const uint8_t *ip = src, *const iend = src + srcSize;
    uint8_t *op = dst, *const oend = dst + dstCap;
    if (dstCap == 0) return (srcSize == 1 && *ip == 0) ? 0 : -1;
    if (srcSize == 0) return -1;
    for (;;) {
        unsigned tok = *ip++;
        size_t lit = tok >> 4;
        if (lit == 15) {
            unsigned s;
            do { if (ip >= iend - 15) return -1; s = *ip++; lit += s; } while (s == 255);
            if ((uintptr_t)op + lit < (uintptr_t)op || (uintptr_t)ip + lit < (uintptr_t)ip) return -1;
        }
        /* literals */
        if (op + lit > oend - 12 || ip + lit > iend - 8) {
            /* must be the last sequence */
            if (ip + lit != iend || op + lit > oend) return -1;
            memmove(op, ip, lit); op += lit;
            break;
        }
        if (lit <= 16 && op + 16 <= oend && ip + 16 <= iend) memcpy(op, ip, 16);
        else memcpy(op, ip, lit);
        ip += lit; op += lit;
        /* match */
        size_t off = ip[0] | ((size_t)ip[1] << 8); ip += 2;
        const uint8_t *m = op - off;
        size_t ml = tok & 15;
        if (ml == 15) {
            unsigned s;
            do { if (ip > iend - 5) return -1; s = *ip++; ml += s; } while (s == 255);
            if ((uintptr_t)op + ml < (uintptr_t)op) return -1;
        }
        ml += 4;
        if (off > (size_t)(op - dst)) return -1;          /* before the start of the output (also off == 0 below) */
        if (op + ml > oend - 5) return -1;
        if (off == 0) { memset(op, 0, ml); op += ml; continue; }   /* the firmware decoder writes zeros for offset 0 (kpp6/lz4/lz4off0.c) */
        if (off >= 16 && op + ml <= oend - 16) {           /* non-overlapping 16-byte steps, may write up to 15 bytes past */
            uint8_t *e = op + ml;
            do { memcpy(op, m, 16); op += 16; m += 16; } while (op < e);
            op = e;
        } else if (off >= ml) { memcpy(op, m, ml); op += ml; }
        else { uint8_t *e = op + ml; while (op < e) *op++ = *m++; }
    }
    return (int)(op - dst);
}
