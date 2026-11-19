/* libkppcc.so (kpp5): keep Amazon's own cover drawable cache across reboots.
 *
 * KPPMainApp caches every rendered Library cover tile as /mnt/kfxcache/kpp/<md5> (4-bit LZ4, UIDecoratorImplImageCache
 * in libReactNative). /mnt/kfxcache is a tmpfs, so after every reboot each cover is decoded and scaled again on its
 * first view. This library mirrors those files, unchanged, into a directory on flash and puts them back before the app
 * builds its cache index:
 *   constructor (KPPMainApp only, before main): STORE -> CACHE for files missing in CACHE (times kept),
 *                                               CACHE -> STORE for valid files missing in STORE
 *   fopen(CACHE/x, "w..") .. fclose            : after the real fclose the finished file is copied to STORE (tmp + rename)
 *   remove()/unlink(CACHE/x)                   : STORE/x is removed too (the app's own eviction / invalidation)
 * The file name is the app's own cache key: md5(image type, uri, [size + time of the source file], w, h, format) -
 * generateCacheKey 0x1a9624 - so a restored file can only be used for exactly the image it was made from.
 * Only complete, structurally valid files are stored/restored (kpp6: header fields + LZ4 block walk, see valid_image).
 * kpp6 restore rules: a store file is deleted ONLY when it was read completely and is invalid; an error of the
 * destination (no space, I/O, close/rename failure) or a transient read error leaves the store file for the next
 * start. Losing the private copy is never worse than a miss of the app's own cache.
 * KPPCC_FSYNC=1 (default off): fsync the store copy before the rename (costs flash latency on the closing thread).
 * KPPCC_CAP_MB=16|32 (independent switch, default off): size limit of that cache. The app's constructor
 * (UIDecoratorImplImageCache(), libReactNative 0x1aae74) picks 16 MB when the device-info value is > 8 and 2 MB
 * otherwise ("ite gt; movgt.w r3,#0x1000000; movle.w r3,#0x200000" at 0x1aaed8). 2 MB = ~56 cover tiles = 6 Library
 * pages, so on this Kindle most page turns decode + Lanczos-scale their JPEG covers again. The switch rewrites the
 * immediate of the movle (4 bytes at 0x1aaede) to 16 MB (Amazon's own other value) or 32 MB, after comparing the 10
 * code bytes and the file size of the mapped libReactNative.so; anything unexpected -> not patched.
 * KPPCC_BOOK=1 (with KPPCC=1): mirror the whole-item tile cache /mnt/kfxcache/kppbook too (store subdirectory book/).
 * KPPCC_BOOK_MB=8|16|35 (independent switch): size limit of that cache, see below.
 * KPPCC_LZ4=1 (independent switch, default off): faster LZ4 decoder for the cache files (see below).
 * Switch: KPPCC=1 (default off). KPPCC_STORE=<dir> (default /var/local/kppfast/covercache), KPPCC_MAX_MB (default 48),
 * KPPCC_LOG=<file>. Does nothing in other processes (children inherit LD_PRELOAD).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <link.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* glibc 2.20 exports only the __xstat family (stat/lstat/fstat live in libc_nonshared.a, not linked here) */
extern int __xstat(int, const char *, struct stat *), __lxstat(int, const char *, struct stat *), __fxstat(int, int, struct stat *);
#define stat(p, b) __xstat(3, p, b)
#define lstat(p, b) __lxstat(3, p, b)
#define fstat(f, b) __fxstat(3, f, b)

/* mirrored cache directories: 0 = cover tiles (always with KPPCC=1), 1 = whole-item tiles /mnt/kfxcache/kppbook
 * (only with KPPCC_BOOK=1; store subdirectory "book"). g_d selects the directory the helpers below work on; it is only
 * changed in the constructor or with g_mu held. */
#define NDIR 2
static const char *const g_cdirs[NDIR] = {"/mnt/kfxcache/kpp/", "/mnt/kfxcache/kppbook/"};
static char g_sdirs[NDIR][216];
static int g_d, g_ndir = 1;
#define CDIR g_cdirs[g_d]
#define SDIR g_sdirs[g_d]
#define MINFREE_KB 40000 /* never let the store push its filesystem below this */

static int g_on;
static char g_store[200] = "/var/local/kppfast/covercache";
static long long g_max = 48LL << 20, g_bytes;
static const char *g_log;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
#define NSLOT 16
static struct { FILE *f; char name[40]; int dir; } g_slot[NSLOT];
static unsigned g_saved, g_restored, g_removed, g_skipped, g_dsterr;

static FILE *(*real_fopen)(const char *, const char *);
static FILE *(*real_fopen64)(const char *, const char *);
static int (*real_fclose)(FILE *);
static int (*real_remove)(const char *);
static int (*real_unlink)(const char *);

static void klog(const char *fmt, ...) {
    if (!g_log) return;
    int fd = open(g_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char b[300]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    if (n > 0) { b[n++] = '\n'; if (write(fd, b, n) < 0) {} }
    close(fd);
}

/* cache file names are 32 lowercase hex digits */
static int good_name(const char *n) {
    int i;
    for (i = 0; i < 32; i++) if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return 0;
    return n[32] == 0;
}

/* ---- validation (kpp6): a file is accepted only if it is a complete, structurally sound cache file ----
 * CacheHeader (0x30 bytes, kpp3/NOTES.md): +0 u16 w, +2 u16 h, +4 u16 stride, +8 u32 LZ4 size, +0xc invert flag,
 * +0x10 rectangle; then one LZ4 block of the 4-bit packed image, ((w+1)/2)*h bytes raw (what the app's reader passes
 * as the output capacity and requires as the result). Checked: size == 48 + LZ4 size, w/h/stride plausible, and the
 * LZ4 block parses sequence by sequence (lengths inside the block, offsets non-zero and inside the output produced
 * so far, last sequence literals-only ending exactly at the block end) to exactly the raw size. Nothing is decoded.
 * Stricter than the app would be is harmless: a rejected file is simply a cache miss for the app. */
#define MAXFILE (4 << 20)
static int lz4_walk(const unsigned char *s, size_t n, size_t raw) {
    size_t ip = 0, out = 0;
    for (;;) {
        if (ip >= n) return 0;
        unsigned tok = s[ip++], b; size_t lit = tok >> 4, ml = tok & 15, off;
        if (lit == 15) do { if (ip >= n) return 0; b = s[ip++]; lit += b; } while (b == 255);
        if (lit > n - ip) return 0;
        ip += lit; out += lit;
        if (out > raw) return 0;
        if (ip == n) return out == raw;
        if (n - ip < 2) return 0;
        off = s[ip] | (size_t)s[ip + 1] << 8; ip += 2;
        if (off == 0 || off > out) return 0;
        if (ml == 15) do { if (ip >= n) return 0; b = s[ip++]; ml += b; } while (b == 255);
        out += ml + 4;
        if (out > raw) return 0;
    }
}
/* whole file in memory (n bytes): 1 = good cache file */
static int valid_image(const unsigned char *p, size_t n) {
    if (n < 49 || n > MAXFILE) return 0;
    unsigned w = p[0] | p[1] << 8, h = p[2] | p[3] << 8, stride = p[4] | p[5] << 8;
    unsigned csz = p[8] | p[9] << 8 | p[10] << 16 | (unsigned)p[11] << 24;
    if ((unsigned long long)csz + 48 != n) return 0;
    if (!w || !h || w > 8192 || h > 8192 || stride < w || stride > w + 15) return 0;
    return lz4_walk(p + 48, csz, (size_t)((w + 1) / 2) * h);
}

static long free_kb(const char *dir) {
    struct statvfs v;
    if (statvfs(dir, &v)) return -1;
    return (long)((unsigned long long)v.f_bavail * v.f_frsize / 1024);
}

/* copy src -> dst through dst.tmp + rename, keep mode and times. Returns the size (> 0) or WHY it failed, because
 * the callers must treat the cases differently (kpp6):
 *   CP_SRC_BAD  the source was read completely and is not a valid cache file  -> the caller may drop the source
 *   CP_SRC_IO   the source could not be opened/read (or changed while reading) -> transient, keep everything
 *   CP_DST      the destination could not be created/written/closed/renamed (no space, I/O error, ...) -> the
 *               source is fine and stays; dst is untouched (a previous valid dst is still there) */
enum { CP_SRC_BAD = -1, CP_SRC_IO = -2, CP_DST = -3 };
static unsigned char *g_buf; static size_t g_bufcap; /* callers hold g_mu or run in the constructor */
static int g_fsync;
static long long copy_file(const char *src, const char *dst, int durable) {
    char tmp[300]; struct stat st;
    int in = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) return CP_SRC_IO;
    if (fstat(in, &st)) { close(in); return CP_SRC_IO; }
    if (!S_ISREG(st.st_mode) || st.st_size < 49 || st.st_size > MAXFILE) { close(in); return CP_SRC_BAD; }
    size_t sz = (size_t)st.st_size, got = 0;
    if (g_bufcap < sz + 1) { /* anonymous mapping, not malloc: this also runs in the constructor, before main */
        size_t cap = sz + 1 < 131072 ? 131072 : (sz + 1 + 4095) & ~(size_t)4095;
        void *nb = mmap(0, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (nb == MAP_FAILED) { close(in); return CP_SRC_IO; }
        if (g_buf) munmap(g_buf, g_bufcap);
        g_buf = nb; g_bufcap = cap;
    }
    while (got < sz + 1) { /* one byte more than the size: a file that grew meanwhile is noticed */
        ssize_t n = read(in, g_buf + got, sz + 1 - got);
        if (n < 0) { if (errno == EINTR) continue; close(in); return CP_SRC_IO; }
        if (n == 0) break;
        got += (size_t)n;
    }
    close(in);
    if (got != sz) return CP_SRC_IO;
    if (!valid_image(g_buf, sz)) return CP_SRC_BAD;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", dst) >= sizeof tmp) return CP_DST;
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, st.st_mode & 0777);
    if (out < 0) return CP_DST;
    size_t done = 0; int ok = 1;
    while (done < sz) {
        ssize_t n = write(out, g_buf + done, sz - done);
        if (n < 0) { if (errno == EINTR) continue; ok = 0; break; }
        if (n == 0) { ok = 0; break; }
        done += (size_t)n;
    }
    if (ok) {
        fchmod(out, st.st_mode & 0777);
        if (fchown(out, st.st_uid, st.st_gid)) {}
        if (durable && g_fsync && fsync(out)) ok = 0;
    }
    if (close(out)) ok = 0;                 /* a delayed write error is reported here */
    if (ok) {
        struct timeval tv[2] = {{st.st_atime, 0}, {st.st_mtime, 0}};
        utimes(tmp, tv);
        if (rename(tmp, dst) == 0) return (long long)sz;
    }
    real_unlink(tmp);
    return CP_DST;
}

static void store_one(const char *name) { /* CACHE/name -> STORE/name, with g_mu held */
    char src[80], dst[300]; struct stat old, cur;
    snprintf(src, sizeof src, "%s%s", CDIR, name); snprintf(dst, sizeof dst, "%s/%s", SDIR, name);
    if (lstat(src, &cur) || !S_ISREG(cur.st_mode)) { g_skipped++; return; }
    long long had = lstat(dst, &old) == 0 ? old.st_size : 0;
    /* both limits include the file that is about to be written (the old copy is replaced, tmp + rename needs the
       space of both for a moment) */
    if (g_bytes - had + cur.st_size > g_max || free_kb(SDIR) < MINFREE_KB + (long)(cur.st_size / 1024) + 1) { g_skipped++; return; }
    long long n = copy_file(src, dst, 1);
    if (n > 0) { g_bytes += n - had; g_saved++; } else { g_skipped++; if (n == CP_DST) g_dsterr++; }
}

static int tmp_name(const char *n) { /* our own leftovers: <32 hex>.tmp */
    int i;
    for (i = 0; i < 32; i++) if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return 0;
    return !strcmp(n + 32, ".tmp");
}

static void initial_sync(void) {
    DIR *d; struct dirent *e; char a[300], b[300]; struct stat st, cst;
    unsigned in_store = 0, kept = 0, dstfail = 0;
    if (stat("/mnt/kfxcache", &st) || !S_ISDIR(st.st_mode)) return;
    snprintf(a, sizeof a, "%s/.dirmeta", SDIR);
    if (stat(CDIR, &cst)) {
        /* After a reboot the directory may not exist yet, and this constructor still runs as root (the app drops to
           its own user later). Never invent owner/mode: recreate the directory exactly as the app made it last time
           (recorded below in SDIR/.dirmeta); without a record nothing is restored for this directory.
           19.11: a root-owned kppbook made by this code locked the app out of its item cache for a whole boot. */
        unsigned long mu = 0, mg = 0, mm = 0; char mbuf[64]; int fd = open(a, O_RDONLY | O_CLOEXEC); ssize_t n = fd < 0 ? -1 : read(fd, mbuf, sizeof mbuf - 1);
        if (fd >= 0) close(fd);
        if (n <= 0) { klog("dir %d: %s missing and no .dirmeta -> no restore", g_d, CDIR); return; }
        mbuf[n] = 0; { char *q = mbuf; mu = strtoul(q, &q, 10); mg = strtoul(q, &q, 10); mm = strtoul(q, &q, 8); }
        if (!mm || (mm & ~0777UL)) { klog("dir %d: bad .dirmeta", g_d); return; }
        if (mkdir(CDIR, 0700)) return;
        if (chown(CDIR, (uid_t)mu, (gid_t)mg) || chmod(CDIR, (mode_t)mm) || stat(CDIR, &cst) || cst.st_uid != mu || cst.st_gid != mg || (cst.st_mode & 0777) != mm) {
            rmdir(CDIR); klog("dir %d: could not recreate %s as %lu:%lu %lo -> removed again, no restore", g_d, CDIR, mu, mg, mm); return; }
        klog("created %s as %lu:%lu %lo", CDIR, mu, mg, mm);
    } else {
        /* remember how the app (or the boot scripts) made this directory */
        char want[64], have[64] = ""; int wn = snprintf(want, sizeof want, "%lu %lu %lo\n", (unsigned long)cst.st_uid, (unsigned long)cst.st_gid, (unsigned long)(cst.st_mode & 0777));
        int fd = open(a, O_RDONLY | O_CLOEXEC); ssize_t n = fd < 0 ? 0 : read(fd, have, sizeof have - 1); if (fd >= 0) close(fd);
        if (n < 0) n = 0;
        have[n] = 0;
        if (strcmp(want, have)) { fd = open(a, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (fd >= 0) { if (write(fd, want, wn) < 0) {} close(fd); } }
    }
    if ((d = opendir(CDIR))) {
        /* a previous process died inside a restore: remove its temporary files before the app indexes the directory */
        while ((e = readdir(d))) if (tmp_name(e->d_name)) { snprintf(b, sizeof b, "%s%s", CDIR, e->d_name); real_unlink(b); }
        closedir(d);
    }
    /* STORE -> CACHE */
    if ((d = opendir(SDIR))) {
        while ((e = readdir(d))) {
            if (!good_name(e->d_name)) {
                if (tmp_name(e->d_name)) { snprintf(a, sizeof a, "%s/%s", SDIR, e->d_name); real_unlink(a); }
                continue;
            }
            snprintf(a, sizeof a, "%s/%s", SDIR, e->d_name); snprintf(b, sizeof b, "%s%s", CDIR, e->d_name);
            if (lstat(a, &st) || !S_ISREG(st.st_mode)) continue;
            in_store++; g_bytes += st.st_size;
            if (lstat(b, &cst) == 0) continue;
            /* the destination failed repeatedly (full, read-only, ...): stop trying, keep the store as it is */
            if (dstfail >= 3 || free_kb(CDIR) < 30000 + (long)(st.st_size / 1024)) { g_skipped++; continue; }
            long long r = copy_file(a, b, 0);
            if (r > 0) { g_restored++; dstfail = 0; }
            else if (r == CP_SRC_BAD) { real_unlink(a); g_bytes -= st.st_size; in_store--; g_removed++; } /* incomplete/corrupt store file */
            else { kept++; if (r == CP_DST) { dstfail++; g_dsterr++; } } /* not the store file's fault: it stays for the next start */
        }
        closedir(d);
    }
    /* CACHE -> STORE for files the store does not have yet */
    if ((d = opendir(CDIR))) {
        while ((e = readdir(d))) {
            if (!good_name(e->d_name)) continue;
            snprintf(a, sizeof a, "%s/%s", SDIR, e->d_name);
            if (lstat(a, &st) == 0) continue;
            store_one(e->d_name);
        }
        closedir(d);
    }
    if (g_bufcap > 262144) { munmap(g_buf, g_bufcap); g_buf = 0; g_bufcap = 0; }
    klog("init dir %d pid %d: store had %u files, restored %u, saved %u, dropped %u, skipped %u, kept after error %u (dst errors %u), store bytes %lld",
         g_d, (int)getpid(), in_store, g_restored, g_saved, g_removed, g_skipped, kept, g_dsterr, g_bytes);
}

/* ---- KPPCC_CAP_MB: cache size limit ---- */
#define RN_SIZE 3260312
#define CAP_SITE 0x1aaed8
/* Expected code bytes are checked by FNV-1a-64 hash only (the public source ships no firmware bytes). The hash is
 * computed over the exact bytes of the stock 5.18.2 binary; together with the file-size check any other build
 * makes the comparison fail and the patch is skipped. tools/genbytes.py --hash prints these values for your device. */
static unsigned long long kpp_fnv64(const unsigned char *p, unsigned n) {
    unsigned long long h = 0xcbf29ce484222325ULL;
    while (n--) { h ^= *p++; h *= 0x100000001b3ULL; }
    return h;
}
#define CAP_EXPECT_FNV 0xef42a53ee159623cULL /* FNV-1a-64 of the 10 expected code bytes */
static int g_cap_mb, g_cap_done;
static int cap_cb(struct dl_phdr_info *i, size_t sz, void *d) {
    const char *n = i->dlpi_name, *b; struct stat st; int k;
    if (!n || !(b = strrchr(n, '/')) || strcmp(b + 1, "libReactNative.so")) return 0;
    if (stat(n, &st) || st.st_size != RN_SIZE) { g_cap_done = -2; return 1; }
    for (k = 0; k < i->dlpi_phnum; k++) {
        const ElfW(Phdr) *ph = &i->dlpi_phdr[k];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X) || CAP_SITE < ph->p_vaddr || CAP_SITE + 10 > ph->p_vaddr + ph->p_filesz) continue;
        unsigned char *a = (unsigned char *)(i->dlpi_addr + CAP_SITE);
        if (kpp_fnv64(a, 10) != CAP_EXPECT_FNV) { g_cap_done = -3; return 1; }
        unsigned long pg = (unsigned long)a & ~4095UL, len = ((unsigned long)a + 10 - pg + 4095) & ~4095UL;
        if (mprotect((void *)pg, len, PROT_READ | PROT_WRITE | PROT_EXEC)) { g_cap_done = -4; return 1; }
        /* movle.w r3,#0x1000000 = f04f 7380 ; movle.w r3,#0x2000000 = f04f 7300 */
        a[6] = 0x4f; a[7] = 0xf0; a[8] = g_cap_mb == 32 ? 0x00 : 0x80; a[9] = 0x73;
        __builtin___clear_cache((char *)a, (char *)a + 10);
        mprotect((void *)pg, len, PROT_READ | PROT_EXEC);
        g_cap_done = 1; return 1;
    }
    g_cap_done = -5; return 1;
}

/* ---- KPPCC_LZ4=1: libReactNative's static LZ4_decompress_safe (0x1e6524, Thumb, used only for the cover cache:
 * callers 0x1a948c / 0x1a95a8 accept the result only when it equals the expected size) -> kpp_lz4_safe (kpplz4.h).
 * The entry (8 bytes) becomes `ldr.w pc, [pc]` + address. Differential test: src/lz4test.c. ---- */
#include "kpplz4.h"
#define LZ_SITE 0x1e6524
#define LZ_EXPECT_FNV 0xf9714a3202f592e6ULL /* FNV-1a-64 of the 8 expected code bytes */
static int g_lz_done;
__attribute__((target("arm"))) static int lz_entry(const char *src, char *dst, int n, int cap) { return kpp_lz4_safe((const uint8_t *)src, (uint8_t *)dst, n, cap); }
static int lz_cb(struct dl_phdr_info *i, size_t sz, void *d) {
    const char *n = i->dlpi_name, *b; struct stat st; int k;
    if (!n || !(b = strrchr(n, '/')) || strcmp(b + 1, "libReactNative.so")) return 0;
    if (stat(n, &st) || st.st_size != RN_SIZE) { g_lz_done = -2; return 1; }
    for (k = 0; k < i->dlpi_phnum; k++) {
        const ElfW(Phdr) *ph = &i->dlpi_phdr[k];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X) || LZ_SITE < ph->p_vaddr || LZ_SITE + 8 > ph->p_vaddr + ph->p_filesz) continue;
        unsigned char *a = (unsigned char *)(i->dlpi_addr + LZ_SITE);
        if (((unsigned long)a & 3) || kpp_fnv64(a, 8) != LZ_EXPECT_FNV) { g_lz_done = -3; return 1; }
        unsigned long pg = (unsigned long)a & ~4095UL, len = ((unsigned long)a + 8 - pg + 4095) & ~4095UL;
        if (mprotect((void *)pg, len, PROT_READ | PROT_WRITE | PROT_EXEC)) { g_lz_done = -4; return 1; }
        unsigned long t = (unsigned long)lz_entry;
        unsigned char j[8] = {0xdf, 0xf8, 0x00, 0xf0, t, t >> 8, t >> 16, t >> 24};
        memcpy(a, j, 8);
        __builtin___clear_cache((char *)a, (char *)a + 8);
        mprotect((void *)pg, len, PROT_READ | PROT_EXEC);
        g_lz_done = 1; return 1;
    }
    g_lz_done = -5; return 1;
}

/* ---- KPPCC_BOOK_MB=8|16|35: size limit of the app's WHOLE-ITEM tile cache /mnt/kfxcache/kppbook (cover + badges as
 * drawn; expose handler 0xa93468 in KPPMainAppV2: hit -> the stored tile is drawn and drawBadgeableImage, the badge
 * drawing, the X read-back and the save are skipped). The size is chosen at 0xa8abe8:
 *   GetMemorySizeMB() > 512 ? 35 MB : 1 MB   ("cmp.w r0,#512; ite hi; movhi.w r5,#0x8c00; movls.w r5,#0x400", then
 * lsls r5,#10). This Kindle reports 482 MB -> 1 MB = the last 2-3 Library pages. The switch rewrites the immediate of
 * the movls (4 bytes at 0xa8abf2) after comparing 28 code bytes and the size of the executable. ---- */
#define APP_SIZE 25599640
#define BOOK_CTX 0xa8abe2UL
#define BOOK_EXPECT_FNV 0x211c0d22d4f33a62ULL /* FNV-1a-64 of the 28 expected code bytes */
static int book_patch(int mb) {
    struct stat st; unsigned char *a = (unsigned char *)BOOK_CTX, *site = a + 16;
    if (stat("/proc/self/exe", &st) || st.st_size != APP_SIZE) return -2;
    /* the executable is ET_EXEC (absolute addresses); make sure the page is mapped from it before reading */
    /* no stdio/malloc here: this runs in the constructor, before main */
    static char mb_[16384]; int ok = 0, fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC); ssize_t n, got = 0;
    if (fd < 0) return -6;
    while (got < (ssize_t)sizeof mb_ - 1 && (n = read(fd, mb_ + got, sizeof mb_ - 1 - got)) > 0) got += n;
    close(fd); mb_[got] = 0;
    for (char *l = mb_; l && *l; ) { char *nl = strchr(l, '\n'); if (nl) *nl = 0;
        char *end; unsigned long lo = strtoul(l, &end, 16), hi = *end == '-' ? strtoul(end + 1, &end, 16) : 0;
        if (lo <= BOOK_CTX && BOOK_CTX + 28 <= hi && end[0] == ' ' && end[1] == 'r' && end[3] == 'x' && strstr(end, "KPPMainApp")) ok = 1;
        l = nl ? nl + 1 : 0; }
    if (!ok) return -5;
    if (kpp_fnv64(a, 28) != BOOK_EXPECT_FNV) return -3;
    unsigned long pg = (unsigned long)site & ~4095UL, len = ((unsigned long)site + 4 - pg + 4095) & ~4095UL;
    if (mprotect((void *)pg, len, PROT_READ | PROT_WRITE | PROT_EXEC)) return -4;
    /* movls.w r5,#0x2000 (8 MB) = 4f f4 00 55 ; #0x4000 (16 MB) = 4f f4 80 45 ; #0x8c00 (35 MB, Amazon's own) = 4f f4 0c 45 */
    if (mb == 8) { site[2] = 0x00; site[3] = 0x55; } else if (mb == 16) { site[2] = 0x80; site[3] = 0x45; } else { site[2] = 0x0c; site[3] = 0x45; }
    __builtin___clear_cache((char *)site, (char *)site + 4);
    mprotect((void *)pg, len, PROT_READ | PROT_EXEC);
    return 1;
}

__attribute__((constructor)) static void kppcc_init(void) {
    char exe[200]; const char *e;
    real_fopen = dlsym(RTLD_NEXT, "fopen"); real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
    real_fclose = dlsym(RTLD_NEXT, "fclose"); real_remove = dlsym(RTLD_NEXT, "remove"); real_unlink = dlsym(RTLD_NEXT, "unlink");
    if (!real_fopen || !real_fclose || !real_remove || !real_unlink) return;
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    const char *bn = strrchr(exe, '/'); bn = bn ? bn + 1 : exe;
    if (strncmp(bn, "KPPMainApp", 10)) return;
    if (access("/mnt/us/DISABLE_KPPFAST", F_OK) == 0) return;
    g_log = getenv("KPPCC_LOG");
    e = getenv("KPPCC_CAP_MB");
    if (e && (atoi(e) == 16 || atoi(e) == 32)) { g_cap_mb = atoi(e); dl_iterate_phdr(cap_cb, 0); klog("cap %d MB: %s (%d)", g_cap_mb, g_cap_done == 1 ? "patched" : "SKIPPED", g_cap_done); }
    e = getenv("KPPCC_BOOK_MB");
    if (e && (atoi(e) == 8 || atoi(e) == 16 || atoi(e) == 35)) { int r = book_patch(atoi(e)); klog("book cache %d MB: %s (%d)", atoi(e), r == 1 ? "patched" : "SKIPPED", r); }
    e = getenv("KPPCC_LZ4");
    if (e && !strcmp(e, "1")) { dl_iterate_phdr(lz_cb, 0); klog("lz4: %s (%d)", g_lz_done == 1 ? "patched" : "SKIPPED", g_lz_done); }
    e = getenv("KPPCC");
    if (!e || strcmp(e, "1")) return;
    e = getenv("KPPCC_STORE"); if (e && e[0] == '/' && strlen(e) < sizeof g_store) strcpy(g_store, e);
    e = getenv("KPPCC_MAX_MB"); if (e && atoi(e) > 0 && atoi(e) <= 200) g_max = (long long)atoi(e) << 20;
    e = getenv("KPPCC_FSYNC"); g_fsync = e && !strcmp(e, "1");
    if (mkdir(g_store, 0775) && errno != EEXIST) return;
    snprintf(g_sdirs[0], sizeof g_sdirs[0], "%s", g_store); snprintf(g_sdirs[1], sizeof g_sdirs[1], "%s/book", g_store);
    e = getenv("KPPCC_BOOK"); if (e && !strcmp(e, "1") && (mkdir(g_sdirs[1], 0775) == 0 || errno == EEXIST)) g_ndir = 2;
    for (g_d = 0; g_d < g_ndir; g_d++) initial_sync();
    g_d = 0;
    g_on = 1;
}

static int cache_name(const char *path, const char **name) { /* 0 = not ours, else directory index + 1 */
    int d;
    if (!g_on || !path) return 0;
    for (d = 0; d < g_ndir; d++) { size_t l = strlen(g_cdirs[d]);
        if (!strncmp(path, g_cdirs[d], l) && good_name(path + l)) { *name = path + l; return d + 1; } }
    return 0;
}

static FILE *open_common(FILE *(*real)(const char *, const char *), const char *path, const char *mode) {
    FILE *f = real(path, mode); const char *name; int i, d;
    if (f && mode && (mode[0] == 'w' || mode[0] == 'a') && (d = cache_name(path, &name))) {
        pthread_mutex_lock(&g_mu);
        for (i = 0; i < NSLOT; i++) if (!g_slot[i].f) { g_slot[i].f = f; g_slot[i].dir = d - 1; memcpy(g_slot[i].name, name, 33); break; }
        pthread_mutex_unlock(&g_mu);
    }
    return f;
}
FILE *fopen(const char *path, const char *mode) {
    if (!real_fopen) real_fopen = dlsym(RTLD_NEXT, "fopen");
    return open_common(real_fopen, path, mode);
}
FILE *fopen64(const char *path, const char *mode) {
    if (!real_fopen64) real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
    return open_common(real_fopen64, path, mode);
}

int fclose(FILE *f) {
    char name[40]; int i, hit = 0, r, d = 0;
    if (!real_fclose) real_fclose = dlsym(RTLD_NEXT, "fclose");
    if (g_on && f) {
        pthread_mutex_lock(&g_mu);
        for (i = 0; i < NSLOT; i++) if (g_slot[i].f == f) { memcpy(name, g_slot[i].name, 33); d = g_slot[i].dir; g_slot[i].f = 0; hit = 1; break; }
        pthread_mutex_unlock(&g_mu);
    }
    r = real_fclose(f);
    if (hit) {
        int e = errno;
        pthread_mutex_lock(&g_mu);
        if (r == 0) { g_d = d; store_one(name); g_d = 0; }
        pthread_mutex_unlock(&g_mu);
        errno = e;
    }
    return r;
}

static void forget(int d, const char *name) {
    char p[300]; struct stat st; int e = errno;
    snprintf(p, sizeof p, "%s/%s", g_sdirs[d], name);
    pthread_mutex_lock(&g_mu);
    if (lstat(p, &st) == 0 && real_unlink(p) == 0) { g_bytes -= st.st_size; g_removed++; }
    pthread_mutex_unlock(&g_mu);
    errno = e;
}
int remove(const char *path) {
    const char *name; int r, d;
    if (!real_remove) real_remove = dlsym(RTLD_NEXT, "remove");
    r = real_remove(path);
    if (r == 0 && (d = cache_name(path, &name))) forget(d - 1, name);
    return r;
}
int unlink(const char *path) {
    const char *name; int r, d;
    if (!real_unlink) real_unlink = dlsym(RTLD_NEXT, "unlink");
    r = real_unlink(path);
    if (r == 0 && (d = cache_name(path, &name))) forget(d - 1, name);
    return r;
}

__attribute__((destructor)) static void kppcc_fini(void) {
    if (g_on) klog("exit pid %d: saved %u, removed %u, skipped %u, dst errors %u, store bytes %lld", (int)getpid(), g_saved, g_removed, g_skipped, g_dsterr, g_bytes);
}
