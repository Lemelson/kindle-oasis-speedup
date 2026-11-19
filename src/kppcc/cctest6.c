/* test driver for libkppcc.so (kpp6): run as KPPMainAppcctest <phase> (see cctest6.sh). Writes cache files through
 * fopen/fclose/remove like the app does. Files are real CacheHeaders + a literal-only LZ4 block. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* kind: 0 good, 1 short (file shorter than the header says), 2 header w=0, 3 LZ4 raw size != ((w+1)/2)*h,
 *       4 right length but zero-filled payload (what a lost data block looks like) */
static void mk(const char *name, unsigned w, unsigned h, int kind) {
    char p[100]; unsigned char hd[48] = {0}; unsigned raw = ((w + 1) / 2) * h, i, n = raw, csz;
    if (kind == 3) n = raw - 7;
    unsigned char *blk = malloc(n + n / 255 + 32), *q = blk;
    if (n >= 15) { unsigned r = n - 15; *q++ = 0xF0; while (r >= 255) { *q++ = 255; r -= 255; } *q++ = r; } else *q++ = n << 4;
    for (i = 0; i < n; i++) *q++ = (i * 7 + name[31]) & 255;
    csz = q - blk;
    if (kind == 4) memset(blk, 0, csz);
    snprintf(p, sizeof p, "/mnt/kfxcache/kpp/%s", name);
    FILE *f = fopen(p, "wb"); if (!f) { perror(p); exit(1); }
    if (kind == 2) w = 0;
    hd[0] = w; hd[1] = w >> 8; hd[2] = h; hd[3] = h >> 8; hd[4] = (w + 1) & ~1u; hd[5] = ((w + 1) & ~1u) >> 8; hd[6] = 0xfc; hd[7] = 0x42;
    hd[8] = csz; hd[9] = csz >> 8; hd[10] = csz >> 16;
    fwrite(hd, 1, 48, f); fwrite(blk, 1, kind == 1 ? csz - 100 : csz, f);
    fclose(f); free(blk);
}
int main(int argc, char **argv) {
    if (!strcmp(argv[1], "write")) {
        mk("00000000000000000000000000000001", 343, 515, 0);
        mk("00000000000000000000000000000002", 343, 400, 0);
        mk("00000000000000000000000000000003", 343, 515, 1);   /* incomplete: must not be stored */
        mk("0000000000000000000000000000000g", 43, 43, 0);     /* not a cache name */
        mk("00000000000000000000000000000004", 43, 43, 0);
        mk("00000000000000000000000000000002", 43, 43, 0);     /* rewrite: store must follow */
        mk("00000000000000000000000000000005", 343, 515, 2);   /* bad header: not stored */
        mk("00000000000000000000000000000006", 343, 515, 3);   /* LZ4 decodes to the wrong size: not stored */
        mk("00000000000000000000000000000007", 343, 515, 4);   /* zero-filled payload: not stored */
        mk("00000000000000000000000000000008", 1, 1, 0);       /* smallest image */
        remove("/mnt/kfxcache/kpp/00000000000000000000000000000004");   /* eviction: gone from the store too */
        FILE *f = fopen("/tmp/other", "w"); fputs("x", f); fclose(f); remove("/tmp/other");
    } else if (!strcmp(argv[1], "one")) mk(argv[2], 343, 515, 0);
    else if (!strcmp(argv[1], "burst")) { /* N files as fast as possible, then one eviction and one rewrite */
        int i, n = atoi(argv[2]); char nm[40];
        for (i = 0; i < n; i++) { snprintf(nm, sizeof nm, "2%031x", i); mk(nm, 43, 43, 0); }
        remove("/mnt/kfxcache/kpp/20000000000000000000000000000000"); mk("20000000000000000000000000000001", 343, 515, 0);
    } else if (!strcmp(argv[1], "asynccheck")) { /* the store copy must NOT exist when fclose returns, and must exist later */
        char p[200]; snprintf(p, sizeof p, "%s/30000000000000000000000000000001", getenv("KPPCC_STORE"));
        mk("30000000000000000000000000000001", 343, 515, 0);
        printf("%s fclose returned before the store copy was written\n", access(p, F_OK) ? "PASS" : "FAIL");
        usleep(700000);
        printf("%s store copy written by the worker afterwards\n", access(p, F_OK) == 0 ? "PASS" : "FAIL");
        remove("/mnt/kfxcache/kpp/30000000000000000000000000000001");
        printf("%s remove returned before the store copy was removed\n", access(p, F_OK) == 0 ? "PASS" : "FAIL");
        usleep(700000);
        printf("%s store copy removed by the worker afterwards\n", access(p, F_OK) ? "PASS" : "FAIL");
        return 0;
    }
    if (getenv("CCTEST_WAIT_MS")) usleep(atoi(getenv("CCTEST_WAIT_MS")) * 1000);   /* let an asynchronous store finish */
    return 0;
}
