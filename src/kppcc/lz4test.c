/* lz4test DIR [MUTATIONS]: kpp_lz4_safe vs libReactNative's own LZ4_decompress_safe (0x1e6524) on cover-cache files
 * and on random corruptions of them. Needs the firmware's own libReactNative.so (an image of your own device's rootfs);
 * not run by ../../build.sh test. */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include "kpplz4.h"
typedef int (*dec_fn)(const char *, char *, int, int);
static uintptr_t rnbase;
static int cb(struct dl_phdr_info *i, size_t s, void *d) { if (i->dlpi_name && strstr(i->dlpi_name, "libReactNative.so")) rnbase = i->dlpi_addr; return 0; }
static unsigned rs = 12345; static unsigned rnd(void) { rs = rs * 1103515245 + 12345; return rs >> 8; }
int main(int argc, char **argv) {
    if (!dlopen("libReactNative.so", RTLD_NOW)) { printf("dlopen: %s\n", dlerror()); return 1; }
    dl_iterate_phdr(cb, 0); dec_fn ref = (dec_fn)(rnbase + 0x1e6524 + 1);
    int nmut = argc > 2 ? atoi(argv[2]) : 200, nf = 0, bad = 0, okboth = 0, agree_fail = 0, ref_only = 0, mine_only = 0;
    DIR *d = opendir(argv[1]); struct dirent *e; static unsigned char buf[1 << 20], mut[1 << 20];
    unsigned char *o1 = malloc(1 << 21), *o2 = malloc(1 << 21);
    while ((e = readdir(d))) {
        char p[600]; snprintf(p, sizeof p, "%s/%s", argv[1], e->d_name);
        FILE *f = fopen(p, "rb"); if (!f) continue; int n = fread(buf, 1, sizeof buf, f); fclose(f);
        if (n < 49) continue;
        int w = buf[0] | buf[1] << 8, h = buf[2] | buf[3] << 8, csz = buf[8] | buf[9] << 8 | buf[10] << 16 | buf[11] << 24, cap = ((w + 1) / 2) * h;
        if (csz + 48 != n || cap <= 0 || cap > (1 << 20)) continue;
        nf++;
        memset(o1, 0xAA, cap + 64); memset(o2, 0xAA, cap + 64);
        int r1 = ref((char *)buf + 48, (char *)o1, csz, cap), r2 = kpp_lz4_safe(buf + 48, o2, csz, cap);
        if (r1 != cap || r2 != cap || memcmp(o1, o2, cap + 64)) { bad++; printf("VALID FILE MISMATCH %s ref %d mine %d cap %d\n", e->d_name, r1, r2, cap); }
        for (int k = 0; k < nmut; k++) {
            memcpy(mut, buf + 48, csz); int ms = csz, mc = cap;
            switch (rnd() % 5) {
                case 0: for (int j = 0, c = 1 + rnd() % 3; j < c; j++) mut[rnd() % csz] ^= 1 << (rnd() % 8); break;
                case 1: ms = 1 + rnd() % csz; break;
                case 2: mc = cap - 1 - (int)(rnd() % 40); if (mc < 0) mc = 0; break;
                case 3: mut[rnd() % csz] = rnd(); mut[rnd() % csz] = 0xff; break;
                case 4: mc = cap + 1 + rnd() % 40; break;
            }
            memset(o1, 0xAA, cap + 128); memset(o2, 0xAA, cap + 128);
            r1 = ref((char *)mut, (char *)o1, ms, mc); r2 = kpp_lz4_safe(mut, o2, ms, mc);
            int s1 = r1 == mc, s2 = r2 == mc;
            if (s1 && s2) { okboth++; if (memcmp(o1, o2, cap + 128)) { bad++; if (bad < 10) printf("OUTPUT MISMATCH %s mut %d\n", e->d_name, k); } }
            else if (!s1 && !s2) { agree_fail++; if (memcmp(o2 + mc, o1 + cap + 100 > o1 ? "\xAA\xAA\xAA\xAA" : "", 0)) {} }
            else if (s1) ref_only++; else mine_only++;
            /* my decoder must never write past the capacity it was given */
            for (int j = mc; j < mc + 32 && j < cap + 128; j++) if (o2[j] != 0xAA) { bad++; if (bad < 10) printf("OVERRUN %s mut %d at +%d\n", e->d_name, k, j - mc); break; }
        }
    }
    printf("files %d, mutations/file %d: both accept %d (outputs compared), both reject %d, only ref accepts %d, only mine accepts %d, BAD %d\n",
           nf, nmut, okboth, agree_fail, ref_only, mine_only, bad);
    return bad != 0 || ref_only != 0 || mine_only != 0;
}
