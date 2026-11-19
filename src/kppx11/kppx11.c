/*
 * libkppx11.so — LD_PRELOAD accelerator for the X11 pixel path of Amazon's KPPMainApp
 * (Kindle FW 5.18.2, armv7 hf, glibc 2.20). Pixel-identical by construction; every failure falls back to
 * the original libX11 function.
 *
 *   KPPX11_SHMPUT=1   XPutImage (ZPixmap, >= KPPX11_MIN bytes) -> copy the sub-rectangle into a SysV shm slot
 *                     and send ShmPutImage instead of the image bytes over the socket. 4-slot ring; a slot is
 *                     reused only when the server has provably processed the request that used it
 *                     (XLastKnownRequestProcessed >= its serial), else one XSync. Request order is unchanged.
 *   KPPX11_SHMGET=1   XGetImage (ZPixmap, >= KPPX11_MIN bytes; also XGetSubImage, which calls XGetImage via PLT)
 *                     -> MIT-SHM GetImage through xcb (errors come back in the reply, never reach the app's
 *                     X error handler), then the same malloc-backed XImage libX11 would build
 *                     (XCreateImage(visual-of-reply, depth-of-reply, ZPixmap, 0, data, w, h, scanline pad)).
 *   KPPX11_PIXBUF=1   gdk_pixbuf_get_from_drawable on a depth-8 GdkPixmap with a gray/indexed colormap: pixels via
 *                     SHM GetImage straight from the segment + GDK's own rgb8 colour mapping as a 256-entry table
 *                     (GDK reads 256x64 scratch tiles via XGetSubImage + per-pixel _XSetImage, then rgb8()).
 *   KPPX11_PIXBUFWIN=1 gdk_pixbuf_get_from_drawable on a GdkWindow (KPP's cover cache read-back, trySaveGDKDrawable):
 *                     when the whole rectangle lies in the window's paint-stack pixmap (what GDK picks per 256x64 tile),
 *                     one GetImage of that pixmap (SHM with KPPX11_SHMGET, else xcb GetImage) + rgb8 mapping (NEON
 *                     byte replication for the StaticGray 0..255 ramp), no-alpha dest only. =2 also reads the window
 *                     itself when no paint is active (not seen on KPP, not parity-tested).
 *   KPPX11_DRAWPB=1   gdk_draw_pixbuf of a no-alpha RGB pixbuf onto an 8-bit gray drawable: GDK's gray8 formula
 *                     (g + ((r + b) >> 1)) >> 1 with NEON halving adds, then gdk_draw_gray_image (row-copy converter).
 *   KPPX11_SHMMODE=oct shm segment mode (default auto: 0600, retry 0666 when the server answers BadAccess).
 *   KPPX11_MIN=n      size threshold in bytes (default 16384).
 *   KPPX11_VERIFY=1   verify mode: every accelerated call is also done the original way and compared
 *                     (get: XImage fields + bytes; put: both paths into scratch pixmaps, read back, compared).
 *                     Mismatches are logged; in verify mode the ORIGINAL result is what the app gets.
 *   KPPX11_STATS=1    count calls/bytes per function and per caller (exe/gdk/cairo/other); dumped to the log
 *                     every 5 s of activity and on `touch /tmp/kppx11.req` (-> /tmp/kppx11.out).
 *   KPPX11_LOG=path   log file (default: none; errors still go to syslog once).
 * Kill switch: if /mnt/us/DISABLE_KPPFAST exists at start, nothing is enabled (pure pass-through).
 * Not active in forked children (pid check) and only for the first Display that uses it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <syslog.h>
#include <dlfcn.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <arm_neon.h>

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1l)
#endif

/* ---------- minimal Xlib ABI (public structs from Xlib.h / XShm.h, 32-bit ARM) ---------- */
typedef unsigned long XID, Drawable, Pixmap, VisualID;
typedef struct _XDisplay Display;
typedef struct _XGC *GC;
typedef struct { int type; Display *display; XID resourceid; unsigned long serial; unsigned char error_code, request_code, minor_code; } XErrorEvent;
typedef int (*XErrorHandler)(Display *, XErrorEvent *);
typedef struct { void *ext_data; VisualID visualid; int class_; unsigned long red_mask, green_mask, blue_mask; int bits_per_rgb, map_entries; } Visual;
typedef struct _XImage {
    int width, height, xoffset, format; char *data; int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad;
    int depth, bytes_per_line, bits_per_pixel; unsigned long red_mask, green_mask, blue_mask; char *obdata;
    struct { void *create_image; int (*destroy_image)(struct _XImage *); void *get_pixel, *put_pixel, *sub_image, *add_pixel; } f;
} XImage;
typedef struct { unsigned long shmseg; int shmid; char *shmaddr; int readOnly; } XShmSegmentInfo;
typedef struct { int depth, bits_per_pixel, scanline_pad; } XPixmapFormatValues;
#define ZPixmap 2
#define AllPlanes (~0UL)

/* xcb (from xcb.h / shm.h) */
typedef struct xcb_connection_t xcb_connection_t;
typedef struct { unsigned int sequence; } xcb_cookie_t;
typedef struct { uint8_t response_type, depth; uint16_t sequence; uint32_t length, visual, size; } xcb_shm_get_image_reply_t;
typedef struct { uint8_t response_type, error_code; uint16_t sequence; uint32_t resource_id; uint16_t minor_code; uint8_t major_code, pad0; uint32_t pad[5], full_sequence; } xcb_generic_error_t;

/* ---------- resolved functions ---------- */
static int (*r_XPutImage)(Display *, Drawable, GC, XImage *, int, int, int, int, unsigned, unsigned);
static XImage *(*r_XGetImage)(Display *, Drawable, int, int, unsigned, unsigned, unsigned long, int);
static XImage *(*x_XCreateImage)(Display *, Visual *, unsigned, int, int, char *, unsigned, unsigned, int, int);
static XPixmapFormatValues *(*x_XListPixmapFormats)(Display *, int *);
static int (*x_XFree)(void *);
static char *(*x_XDisplayString)(Display *);
static unsigned long (*x_XNextRequest)(Display *), (*x_XLastKnownRequestProcessed)(Display *);
static int (*x_XSync)(Display *, int);
static XErrorHandler (*x_XSetErrorHandler)(XErrorHandler);
static Visual *(*x_XVIDtoVisual)(Display *, VisualID);
static Pixmap (*x_XCreatePixmap)(Display *, Drawable, unsigned, unsigned, unsigned);
static int (*x_XFreePixmap)(Display *, Pixmap);
static GC (*x_XCreateGC)(Display *, Drawable, unsigned long, void *);
static int (*x_XFreeGC)(Display *, GC);
static int (*x_XImageByteOrder)(Display *);
static Drawable (*x_XDefaultRootWindow)(Display *);
static int (*s_Query)(Display *);
static int (*s_Attach)(Display *, XShmSegmentInfo *), (*s_Detach)(Display *, XShmSegmentInfo *);
static int (*s_Put)(Display *, Drawable, GC, XImage *, int, int, int, int, unsigned, unsigned, int);
static xcb_connection_t *(*c_XGetXCBConnection)(Display *);
static xcb_cookie_t (*c_shm_get_image)(xcb_connection_t *, uint32_t, int16_t, int16_t, uint16_t, uint16_t, uint32_t, uint8_t, uint32_t, uint32_t);
static xcb_shm_get_image_reply_t *(*c_shm_get_image_reply)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);

/* ---------- state ---------- */
static int g_pixbuf;
static unsigned long st_pb_fast, st_pb_orig, st_pb_why[4];
static int g_pixbufwin;
static unsigned long st_pbw_fast, st_pbw_px, st_pbw_why[6]; /* args, composite part/other, bounds, xfail, alpha, cmap */
static unsigned long st_pbw_win, st_pbw_pm, st_pbw_ident;
static int g_drawpb;
static unsigned long st_dp_fast, st_dp_px, st_dp_orig, st_dp_vok, st_dp_vbad;
static int g_put, g_get, g_verify, g_stats, g_min = 16384, g_mode = 0, g_mode_ok = 0; /* 0 = auto: 0600, then 0666 on BadAccess */
static pid_t g_pid;
static FILE *g_log;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static Display *g_dpy;           /* the one display we accelerate */
static int g_dpy_state;          /* 0 unknown, 1 ok, -1 unusable */
static int g_pad[33], g_bpp[33]; /* per depth: scanline pad (bits) and bits_per_pixel from the server */
static xcb_connection_t *g_xcb;

#define NSLOT 4
#define SEG_CAP (4u << 20)       /* larger images go the original way */
typedef struct { XShmSegmentInfo si; size_t size; unsigned long serial; int busy; } Slot;
static Slot g_slot[NSLOT], g_gslot;
static int g_next;

/* stats: [fn][caller] */
enum { F_PUT, F_PUTSHM, F_GET, F_GETSHM, F_NF };
static const char *FN[F_NF] = { "XPutImage", "  ->shm", "XGetImage", "  ->shm" };
static const char *CN[5] = { "exe", "gdk", "cairo", "x11(gdk XGetSubImage)", "other" };
static unsigned long st_n[F_NF][5], st_b[F_NF][5], st_sync, st_fail, st_vbad, st_vok, st_vpad;
static time_t st_last;

static void lg(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(g_log, "[kppx11 %d %ld.%03ld] ", (int)getpid(), (long)ts.tv_sec % 100000, ts.tv_nsec / 1000000);
    vfprintf(g_log, fmt, ap); fputc('\n', g_log); fflush(g_log); va_end(ap);
}

static int caller_class(void *ra)
{
    Dl_info di;
    if (!ra || !dladdr(ra, &di) || !di.dli_fname) return 4;
    if (strstr(di.dli_fname, "libX11")) return 3;
    if (strstr(di.dli_fname, "libgdk")) return 1;
    if (strstr(di.dli_fname, "libcairo")) return 2;
    if (strstr(di.dli_fname, "KPPMainApp")) return 0;
    return 4;
}

static void stats_dump(FILE *f)
{
    fprintf(f, "kppx11 stats pid=%d put=%d get=%d verify=%d syncs=%lu fail=%lu verify_ok=%lu verify_bad=%lu verify_padonly=%lu pixbuf_fast=%lu pixbuf_orig=%lu (notpixmap=%lu cmap/visual=%lu cmapsize=%lu bounds=%lu)\n",
            (int)getpid(), g_put, g_get, g_verify, st_sync, st_fail, st_vok, st_vbad, st_vpad, st_pb_fast, st_pb_orig, st_pb_why[0], st_pb_why[1], st_pb_why[2], st_pb_why[3]);
    if (g_pixbufwin) fprintf(f, "pixbufwin fast=%lu pixels=%lu (window=%lu paintpixmap=%lu neon-gray=%lu) orig: args=%lu composite=%lu bounds=%lu xfail=%lu alpha=%lu cmap=%lu\n",
                             st_pbw_fast, st_pbw_px, st_pbw_win, st_pbw_pm, st_pbw_ident, st_pbw_why[0], st_pbw_why[1], st_pbw_why[2], st_pbw_why[3], st_pbw_why[4], st_pbw_why[5]);
    if (g_drawpb) fprintf(f, "drawpb fast=%lu pixels=%lu orig=%lu verify_ok=%lu verify_bad=%lu\n", st_dp_fast, st_dp_px, st_dp_orig, st_dp_vok, st_dp_vbad);
    for (int i = 0; i < F_NF; i++)
        for (int c = 0; c < 5; c++)
            if (st_n[i][c]) fprintf(f, "%-10s %-6s calls=%lu bytes=%lu\n", FN[i], CN[c], st_n[i][c], st_b[i][c]);
}

static void stats_tick(void)
{
    time_t now = time(NULL);
    if (now - st_last < 1) return;
    if (access("/tmp/kppx11.req", F_OK) == 0) {
        FILE *f = fopen("/tmp/kppx11.out.tmp", "w");
        if (f) { stats_dump(f); fclose(f); rename("/tmp/kppx11.out.tmp", "/tmp/kppx11.out"); }
        unlink("/tmp/kppx11.req");
    }
    if (g_log && now - st_last >= 5) stats_dump(g_log), fflush(g_log);
    st_last = now;
}

#define STAT(fn, c, bytes) do { if (g_stats) { st_n[fn][c]++; st_b[fn][c] += (bytes); } } while (0)

/* ---------- init ---------- */
#define SYM(var, h, name) (*(void **)&(var) = dlsym((h), (name)))

__attribute__((constructor)) static void kppx11_init(void)
{
    SYM(r_XPutImage, RTLD_NEXT, "XPutImage");
    SYM(r_XGetImage, RTLD_NEXT, "XGetImage");
    const char *lp = getenv("KPPX11_LOG");
    if (lp && *lp) g_log = fopen(lp, "a");
    if (access("/mnt/us/DISABLE_KPPFAST", F_OK) == 0) { lg("kill switch /mnt/us/DISABLE_KPPFAST present: pass-through"); return; }
    const char *e;
    g_put = (e = getenv("KPPX11_SHMPUT")) && *e == '1';
    g_get = (e = getenv("KPPX11_SHMGET")) && *e == '1';
    g_verify = (e = getenv("KPPX11_VERIFY")) && *e == '1';
    g_stats = (e = getenv("KPPX11_STATS")) && *e == '1';
    g_pixbuf = (e = getenv("KPPX11_PIXBUF")) && *e == '1';
    g_drawpb = (e = getenv("KPPX11_DRAWPB")) && *e == '1';
    g_pixbufwin = (e = getenv("KPPX11_PIXBUFWIN")) && (*e == '1' || *e == '2') ? *e - '0' : 0;
    if ((e = getenv("KPPX11_MIN")) && *e) g_min = atoi(e);
    if ((e = getenv("KPPX11_SHMMODE")) && *e) g_mode = (int)strtol(e, NULL, 8) & 0777;
    g_pid = getpid();
    if (!r_XPutImage || !r_XGetImage) { g_put = g_get = g_pixbuf = g_pixbufwin = g_drawpb = 0; lg("no libX11 in this process"); }
    lg("init put=%d get=%d pixbuf=%d pixbufwin=%d drawpb=%d verify=%d stats=%d min=%d", g_put, g_get, g_pixbuf, g_pixbufwin, g_drawpb, g_verify, g_stats, g_min);
}

static void disable(const char *why)
{
    lg("disabled: %s", why);
    syslog(LOG_WARNING, "kppx11: SHM path disabled: %s", why);
    g_dpy_state = -1; st_fail++;
}

static int g_trap_err;
static int trap_handler(Display *d, XErrorEvent *ev) { g_trap_err = ev->error_code ? ev->error_code : 1; return 0; }

static void seg_free(Slot *s)
{
    if (!s->si.shmaddr) return;
    if (s->si.shmseg) s_Detach(g_dpy, &s->si);
    shmdt(s->si.shmaddr);
    memset(s, 0, sizeof *s);
}

/* make slot hold >= need bytes; returns 0 on success. Caller holds g_mu; the slot must be idle. */
static int seg_ensure(Slot *s, size_t need)
{
    if (s->si.shmaddr && s->size >= need) return 0;
    size_t sz = (need + 65535) & ~(size_t)65535;
    if (s->si.shmaddr) { x_XSync(g_dpy, 0); st_sync++; seg_free(s); } /* server must be done with the old one */
    for (int attempt = 0; attempt < 2; attempt++) {
        int mode = g_mode ? g_mode : (g_mode_ok ? g_mode_ok : (attempt ? 0666 : 0600));
        int id = shmget(IPC_PRIVATE, sz, IPC_CREAT | mode);
        if (id < 0) { lg("shmget(%zu): %s", sz, strerror(errno)); return -1; }
        void *a = shmat(id, NULL, 0);
        if (a == (void *)-1) { lg("shmat: %s", strerror(errno)); shmctl(id, IPC_RMID, NULL); return -1; }
        s->si.shmid = id; s->si.shmaddr = a; s->si.readOnly = 0; s->size = sz; s->si.shmseg = 0;
        x_XSync(g_dpy, 0);                  /* deliver pending errors to their real owner first */
        XErrorHandler old = x_XSetErrorHandler(trap_handler);
        g_trap_err = 0;
        int ok = s_Attach(g_dpy, &s->si);
        x_XSync(g_dpy, 0);
        x_XSetErrorHandler(old);
        shmctl(id, IPC_RMID, NULL);         /* freed when both sides detach / exit */
        if (ok && !g_trap_err) { g_mode_ok = mode; break; }
        lg("XShmAttach failed err=%d mode=%o", g_trap_err, mode);
        shmdt(a); memset(s, 0, sizeof *s);
        /* BadAccess(10) with 0600 = the server could not match our uid (seen in the docker emulator): retry 0666 */
        if (g_mode || g_mode_ok || g_trap_err != 10) return -1;
    }
    if (!s->si.shmaddr) return -1;
    s->busy = 0;
    lg("segment %d attached, %zu bytes, mode %o", s->si.shmid, sz, g_mode_ok);
    return 0;
}

static int dpy_ready(Display *dpy)
{
    if (getpid() != g_pid) return 0;
    if (g_dpy_state == 1) return dpy == g_dpy;
    if (g_dpy_state < 0 || (g_dpy && dpy != g_dpy)) return 0;
    g_dpy = dpy; g_dpy_state = -1;
    void *x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
    void *xext = dlopen("libXext.so.6", RTLD_LAZY);
    if (!x11 || !xext) { disable("libX11/libXext not loadable"); return 0; }
    SYM(x_XCreateImage, x11, "XCreateImage"); SYM(x_XListPixmapFormats, x11, "XListPixmapFormats");
    SYM(x_XFree, x11, "XFree"); SYM(x_XDisplayString, x11, "XDisplayString");
    SYM(x_XNextRequest, x11, "XNextRequest"); SYM(x_XLastKnownRequestProcessed, x11, "XLastKnownRequestProcessed");
    SYM(x_XSync, x11, "XSync"); SYM(x_XSetErrorHandler, x11, "XSetErrorHandler"); SYM(x_XVIDtoVisual, x11, "_XVIDtoVisual");
    SYM(x_XCreatePixmap, x11, "XCreatePixmap"); SYM(x_XFreePixmap, x11, "XFreePixmap");
    SYM(x_XCreateGC, x11, "XCreateGC"); SYM(x_XFreeGC, x11, "XFreeGC");
    SYM(x_XImageByteOrder, x11, "XImageByteOrder"); SYM(x_XDefaultRootWindow, x11, "XDefaultRootWindow");
    SYM(s_Query, xext, "XShmQueryExtension"); SYM(s_Attach, xext, "XShmAttach"); SYM(s_Detach, xext, "XShmDetach");
    SYM(s_Put, xext, "XShmPutImage");
    if (!x_XCreateImage || !x_XListPixmapFormats || !x_XFree || !x_XDisplayString || !x_XNextRequest ||
        !x_XLastKnownRequestProcessed || !x_XSync || !x_XSetErrorHandler || !x_XVIDtoVisual || !x_XCreatePixmap ||
        !x_XFreePixmap || !x_XCreateGC || !x_XFreeGC || !x_XImageByteOrder || !x_XDefaultRootWindow || !s_Query || !s_Attach || !s_Detach || !s_Put) { disable("missing symbols"); return 0; }
    const char *ds = x_XDisplayString(dpy);
    if (!ds || !(ds[0] == ':' || !strncmp(ds, "unix:", 5))) { disable("display not local"); return 0; }
    if (!s_Query(dpy)) { disable("server has no MIT-SHM"); return 0; }
    int n = 0;
    XPixmapFormatValues *pf = x_XListPixmapFormats(dpy, &n);
    for (int i = 0; pf && i < n; i++)
        if (pf[i].depth > 0 && pf[i].depth <= 32) { g_pad[pf[i].depth] = pf[i].scanline_pad; g_bpp[pf[i].depth] = pf[i].bits_per_pixel; }
    if (pf) x_XFree(pf);
    if (g_get) {
        void *xx = dlopen("libX11-xcb.so.1", RTLD_LAZY), *xs = dlopen("libxcb-shm.so.0", RTLD_LAZY);
        if (xx) SYM(c_XGetXCBConnection, xx, "XGetXCBConnection");
        if (xs) { SYM(c_shm_get_image, xs, "xcb_shm_get_image"); SYM(c_shm_get_image_reply, xs, "xcb_shm_get_image_reply"); }
        if (c_XGetXCBConnection) g_xcb = c_XGetXCBConnection(dpy);
        if (!g_xcb || !c_shm_get_image || !c_shm_get_image_reply) { lg("no xcb-shm: SHMGET off"); g_get = 0; }
    }
    /* probe: one segment must attach */
    if (seg_ensure(&g_slot[0], 65536)) { disable("XShmAttach probe failed"); return 0; }
    g_dpy_state = 1;
    lg("display %s ready (depth8 pad=%d bpp=%d)", ds, g_pad[8], g_bpp[8]);
    return 1;
}

static int processed(unsigned long serial)
{
    return (long)(x_XLastKnownRequestProcessed(g_dpy) - serial) >= 0;
}

/* ---------- XPutImage ---------- */
/* returns 0 = sent via SHM, <0 = caller must use the original path */
static int shm_put(Display *dpy, Drawable d, GC gc, XImage *im, int sx, int sy, int dx, int dy, unsigned w, unsigned h)
{
    int dep = im->depth;
    if (im->format != ZPixmap || dep < 1 || dep > 32 || !g_bpp[dep] || im->bits_per_pixel != g_bpp[dep] ||
        (im->bits_per_pixel & 7) || im->xoffset != 0 || !im->data) return -1;
    if (im->bits_per_pixel > 8 && im->byte_order != x_XImageByteOrder(dpy)) return -1; /* libX11 would swap */
    if (sx < 0 || sy < 0 || w == 0 || h == 0 || w > 32767 || h > 32767 ||
        sx + (long)w > im->width || sy + (long)h > im->height) return -1;     /* libX11 clips: leave it to it */
    int Bpp = im->bits_per_pixel >> 3;
    size_t row = (size_t)w * Bpp, pad = (size_t)g_pad[dep] >> 3;
    if (!pad) return -1;
    size_t stride = (row + pad - 1) / pad * pad, need = stride * h;
    if (need < (size_t)g_min || need > SEG_CAP) return -1;
    if ((size_t)im->bytes_per_line < (size_t)(sx + w) * Bpp) return -1;
    Slot *s = NULL;                         /* a slot the server has provably finished reading */
    for (int k = 0; k < NSLOT; k++) {
        Slot *c = &g_slot[(g_next + k) % NSLOT];
        if (!c->busy || processed(c->serial)) { c->busy = 0; s = c; g_next = (int)(c - g_slot + 1) % NSLOT; break; }
    }
    if (!s) {
        x_XSync(dpy, 0); st_sync++;
        for (int k = 0; k < NSLOT; k++) g_slot[k].busy = 0;
        s = &g_slot[g_next]; g_next = (g_next + 1) % NSLOT;
    }
    if (seg_ensure(s, need)) return -2;
    char *dst = s->si.shmaddr;
    const char *src = im->data + (size_t)sy * im->bytes_per_line + (size_t)sx * Bpp;
    if (stride == row && (size_t)im->bytes_per_line == row) memcpy(dst, src, need);
    else for (unsigned y = 0; y < h; y++) {
        memcpy(dst + y * stride, src + (size_t)y * im->bytes_per_line, row);
        if (stride > row) memset(dst + y * stride + row, 0, stride - row);
    }
    XImage t = *im;
    t.width = w; t.height = h; t.data = dst; t.obdata = (char *)&s->si; t.bytes_per_line = stride;
    s_Put(dpy, d, gc, &t, 0, 0, dx, dy, w, h, 0);
    s->serial = x_XNextRequest(dpy) - 1;
    s->busy = 1;
    return 0;
}

static int same_image(XImage *a, XImage *b, char *why, size_t whylen)
{
    if (!a || !b) { snprintf(why, whylen, "null a=%p b=%p", (void *)a, (void *)b); return a == b; }
#define F(x) if (a->x != b->x) { snprintf(why, whylen, #x " %ld vs %ld", (long)a->x, (long)b->x); return 0; }
    F(width) F(height) F(xoffset) F(format) F(byte_order) F(bitmap_unit) F(bitmap_bit_order) F(bitmap_pad)
    F(depth) F(bytes_per_line) F(bits_per_pixel) F(red_mask) F(green_mask) F(blue_mask)
    F(f.create_image) F(f.destroy_image) F(f.get_pixel) F(f.put_pixel) F(f.sub_image) F(f.add_pixel)
#undef F
    if ((a->obdata == NULL) != (b->obdata == NULL)) { snprintf(why, whylen, "obdata"); return 0; }
    /* pixel bytes of each row; the scanline pad after them is not part of the image (libX11/the server leave
       whatever the buffer held there, our SHM path zeroes it) and is reported separately */
    size_t row = ((size_t)a->width * a->bits_per_pixel + 7) / 8, padbad = 0;
    for (int y = 0; y < a->height; y++) {
        const char *pa = a->data + (size_t)y * a->bytes_per_line, *pb = b->data + (size_t)y * b->bytes_per_line;
        if (memcmp(pa, pb, row)) {
            size_t i = 0; while (i < row && pa[i] == pb[i]) i++;
            snprintf(why, whylen, "pixel differs at row %d col-byte %zu", y, i); return 0;
        }
        if (memcmp(pa + row, pb + row, a->bytes_per_line - row)) padbad++;
    }
    if (padbad) { st_vpad++; if (st_vpad <= 5) lg("verify: pixels identical, scanline pad bytes differ in %zu rows (%dx%d bpl=%d)", padbad, a->width, a->height, a->bytes_per_line); }
    return 1;
}

static void verify_put(Display *dpy, XImage *im, int sx, int sy, unsigned w, unsigned h)
{
    Drawable root = x_XDefaultRootWindow(dpy);
    Pixmap pa = x_XCreatePixmap(dpy, root, w, h, im->depth), pb = x_XCreatePixmap(dpy, root, w, h, im->depth);
    GC g = x_XCreateGC(dpy, pa, 0, NULL);
    r_XPutImage(dpy, pa, g, im, sx, sy, 0, 0, w, h);
    int rc = shm_put(dpy, pb, g, im, sx, sy, 0, 0, w, h);
    XImage *A = r_XGetImage(dpy, pa, 0, 0, w, h, AllPlanes, ZPixmap), *B = r_XGetImage(dpy, pb, 0, 0, w, h, AllPlanes, ZPixmap);
    char why[160] = "";
    if (rc == 0) {
        if (same_image(A, B, why, sizeof why)) st_vok++;
        else { st_vbad++; lg("VERIFY PUT MISMATCH %ux%u depth=%d bpl=%d sx=%d sy=%d: %s", w, h, im->depth, im->bytes_per_line, sx, sy, why); }
    }
    if (A) A->f.destroy_image(A);
    if (B) B->f.destroy_image(B);
    x_XFreeGC(dpy, g); x_XFreePixmap(dpy, pa); x_XFreePixmap(dpy, pb);
}

int XPutImage(Display *dpy, Drawable d, GC gc, XImage *im, int sx, int sy, int dx, int dy, unsigned w, unsigned h)
{
    if (!r_XPutImage) SYM(r_XPutImage, RTLD_NEXT, "XPutImage");
    if (!(g_put || g_stats) || !dpy || !im) return r_XPutImage(dpy, d, gc, im, sx, sy, dx, dy, w, h);
    int c = g_stats ? caller_class(__builtin_return_address(0)) : 4;
    pthread_mutex_lock(&g_mu);
    STAT(F_PUT, c, (unsigned long)w * h * (im->bits_per_pixel ? im->bits_per_pixel : 1) / 8);
    int rc = -1;
    if (g_put && dpy_ready(dpy)) {
        if (g_verify) verify_put(dpy, im, sx, sy, w, h);
        else if ((rc = shm_put(dpy, d, gc, im, sx, sy, dx, dy, w, h)) == 0) STAT(F_PUTSHM, c, (unsigned long)w * h * im->bits_per_pixel / 8);
        else if (rc == -2) disable("segment allocation failed (put)");
    }
    if (g_stats) stats_tick();
    pthread_mutex_unlock(&g_mu);
    if (rc == 0) return 0;                  /* libX11's XPutImage always returns 0 */
    return r_XPutImage(dpy, d, gc, im, sx, sy, dx, dy, w, h);
}

/* ---------- XGetImage ---------- */
static XImage *shm_get(Display *dpy, Drawable d, int x, int y, unsigned w, unsigned h, unsigned long pm, int fmt, int *fail)
{
    *fail = 0;
    if (fmt != ZPixmap || !g_get || !w || !h || w > 65535 || h > 65535) return NULL;
    int dd = 8, maxbpp = 0;                 /* size the segment for the deepest pixmap format the server has */
    for (int k = 1; k <= 32; k++) if (g_bpp[k] > maxbpp) { maxbpp = g_bpp[k]; dd = k; }
    size_t pad = (size_t)(g_pad[dd] ? g_pad[dd] : 32) >> 3;
    size_t need = ((size_t)w * maxbpp / 8 + pad - 1) / pad * pad * h;
    size_t minb = (size_t)w * h * (g_bpp[8] ? g_bpp[8] : 8) / 8;
    if (minb < (size_t)g_min || need > 2 * SEG_CAP) return NULL;
    if (seg_ensure(&g_gslot, need)) { *fail = 2; return NULL; }
    xcb_generic_error_t *err = NULL;
    xcb_cookie_t ck = c_shm_get_image(g_xcb, (uint32_t)d, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, (uint32_t)pm,
                                      ZPixmap, (uint32_t)g_gslot.si.shmseg, 0);
    xcb_shm_get_image_reply_t *r = c_shm_get_image_reply(g_xcb, ck, &err);
    if (!r) {
        if (err) { lg("ShmGetImage error %d (major %d minor %d) d=0x%lx %ux%u", err->error_code, err->major_code, err->minor_code, d, w, h); free(err); }
        *fail = 1; return NULL;
    }
    int depth = r->depth; uint32_t vis = r->visual, size = r->size;
    free(r);
    if (depth < 1 || depth > 32 || size > g_gslot.size) { *fail = 1; return NULL; }
    char *data = malloc(size ? size : 1);
    if (!data) { *fail = 1; return NULL; }
    int spad = g_pad[depth] ? g_pad[depth] : 32;
    XImage *im = x_XCreateImage(dpy, vis ? x_XVIDtoVisual(dpy, vis) : NULL, depth, ZPixmap, 0, data, w, h, spad, 0);
    if (!im) { free(data); *fail = 1; return NULL; }
    if (im->height < 1 || im->bytes_per_line < 1 || (size_t)im->bytes_per_line * im->height > size) {
        im->f.destroy_image(im); *fail = 1; return NULL;
    }
    size_t row = ((size_t)w * im->bits_per_pixel + 7) / 8, bpl = im->bytes_per_line;
    if (row == bpl) memcpy(data, g_gslot.si.shmaddr, size);
    else for (unsigned yy = 0; yy < h; yy++) {      /* copy pixels, zero the scanline pad (deterministic) */
        memcpy(data + yy * bpl, g_gslot.si.shmaddr + yy * bpl, row);
        memset(data + yy * bpl + row, 0, bpl - row);
    }
    if (size > bpl * h) memset(data + bpl * h, 0, size - bpl * h);
    return im;
}


XImage *XGetImage(Display *dpy, Drawable d, int x, int y, unsigned w, unsigned h, unsigned long pm, int fmt)
{
    if (!r_XGetImage) SYM(r_XGetImage, RTLD_NEXT, "XGetImage");
    if (!(g_get || g_stats) || !dpy) return r_XGetImage(dpy, d, x, y, w, h, pm, fmt);
    int c = g_stats ? caller_class(__builtin_return_address(0)) : 4;
    XImage *res = NULL;
    pthread_mutex_lock(&g_mu);
    if (g_get && dpy_ready(dpy)) {
        int fail;
        res = shm_get(dpy, d, x, y, w, h, pm, fmt, &fail);
        if (fail == 2) disable("segment allocation failed (get)");
        if (res && g_verify) {
            XImage *o = r_XGetImage(dpy, d, x, y, w, h, pm, fmt);
            char why[160] = "";
            if (same_image(o, res, why, sizeof why)) st_vok++;
            else { st_vbad++; lg("VERIFY GET MISMATCH d=0x%lx %d,%d %ux%u: %s", d, x, y, w, h, why); }
            res->f.destroy_image(res);
            res = o;                        /* verify mode: the app gets the original */
            if (!o) { pthread_mutex_unlock(&g_mu); return NULL; }
        }
        if (res) STAT(F_GETSHM, c, (unsigned long)res->bytes_per_line * res->height);
    }
    if (g_stats) stats_tick();
    pthread_mutex_unlock(&g_mu);
    if (!res) res = r_XGetImage(dpy, d, x, y, w, h, pm, fmt);
    if (g_stats && res) { pthread_mutex_lock(&g_mu); STAT(F_GET, c, (unsigned long)res->bytes_per_line * res->height); pthread_mutex_unlock(&g_mu); }
    return res;
}

/* ---------- gdk_pixbuf_get_from_drawable fast path (KPPX11_PIXBUF=1) ----------
 * Only for a GdkPixmap of depth 8 whose colormap visual is a depth-8 colormapped/gray visual, with the same
 * argument checks as GTK 2.20 (anything else -> original). Pixels come from the X pixmap (SHM GetImage when
 * available, else the original XGetImage), and are converted with exactly GDK's rgb8()/rgb8a() mapping:
 * R,G,B = colormap->colors[p].red/green/blue >> 8 (alpha 0xff), via a 256-entry table built per call. */
typedef struct { uint32_t pixel; uint16_t red, green, blue; } GdkColor_;
typedef struct { void *g_class; unsigned ref; void *qdata; int type, depth; } GdkVisual_;
typedef struct { void *g_class; unsigned ref; void *qdata; int size; GdkColor_ *colors; GdkVisual_ *visual; } GdkColormap_;
typedef void GdkPixbuf;
static GdkPixbuf *(*r_get_from_drawable)(GdkPixbuf *, void *, void *, int, int, int, int, int, int);
static struct {
    unsigned long (*pixmap_type)(void); int (*is_a)(void *, unsigned long);
    int (*depth)(void *); void *(*cmap)(void *); void (*size)(void *, int *, int *);
    unsigned long (*xid)(void *); Display *(*xdpy)(void *);
    GdkPixbuf *(*pnew)(int, int, int, int, int); int (*nch)(GdkPixbuf *), (*cs)(GdkPixbuf *), (*bps)(GdkPixbuf *);
    int (*pw)(GdkPixbuf *), (*ph)(GdkPixbuf *), (*rs)(GdkPixbuf *); unsigned char *(*px)(GdkPixbuf *);
    GdkPixbuf *(*pcopy)(GdkPixbuf *); void (*unref)(void *);
    int ok;
} G;

static int gsyms(void)
{
    if (G.ok) return G.ok > 0;
    G.ok = -1;
    SYM(G.pixmap_type, RTLD_DEFAULT, "gdk_pixmap_get_type"); SYM(G.is_a, RTLD_DEFAULT, "g_type_check_instance_is_a");
    SYM(G.depth, RTLD_DEFAULT, "gdk_drawable_get_depth"); SYM(G.cmap, RTLD_DEFAULT, "gdk_drawable_get_colormap");
    SYM(G.size, RTLD_DEFAULT, "gdk_drawable_get_size"); SYM(G.xid, RTLD_DEFAULT, "gdk_x11_drawable_get_xid");
    SYM(G.xdpy, RTLD_DEFAULT, "gdk_x11_drawable_get_xdisplay"); SYM(G.pnew, RTLD_DEFAULT, "gdk_pixbuf_new");
    SYM(G.nch, RTLD_DEFAULT, "gdk_pixbuf_get_n_channels"); SYM(G.cs, RTLD_DEFAULT, "gdk_pixbuf_get_colorspace");
    SYM(G.bps, RTLD_DEFAULT, "gdk_pixbuf_get_bits_per_sample"); SYM(G.pw, RTLD_DEFAULT, "gdk_pixbuf_get_width");
    SYM(G.ph, RTLD_DEFAULT, "gdk_pixbuf_get_height"); SYM(G.rs, RTLD_DEFAULT, "gdk_pixbuf_get_rowstride");
    SYM(G.px, RTLD_DEFAULT, "gdk_pixbuf_get_pixels"); SYM(G.pcopy, RTLD_DEFAULT, "gdk_pixbuf_copy");
    SYM(G.unref, RTLD_DEFAULT, "g_object_unref");
    if (G.pixmap_type && G.is_a && G.depth && G.cmap && G.size && G.xid && G.xdpy && G.pnew && G.nch && G.cs && G.bps &&
        G.pw && G.ph && G.rs && G.px && G.pcopy && G.unref) G.ok = 1;
    else lg("pixbuf fast path: gdk symbols missing");
    return G.ok > 0;
}

/* fetch w x h depth-8 pixels of pixmap xid at (x,y): returns pointer + stride; *tofree set if malloc'd XImage */
static const unsigned char *fetch8(Display *dpy, unsigned long xid, int x, int y, int w, int h, int *stride, XImage **tofree)
{
    *tofree = NULL;
    if (g_get && dpy_ready(dpy) && g_bpp[8] == 8) {
        size_t pad = (size_t)(g_pad[8] ? g_pad[8] : 32) >> 3, bpl = ((size_t)w + pad - 1) / pad * pad;
        if (bpl * h <= 2 * SEG_CAP && !seg_ensure(&g_gslot, bpl * h)) {
            xcb_generic_error_t *err = NULL;
            xcb_cookie_t ck = c_shm_get_image(g_xcb, (uint32_t)xid, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h,
                                              0xffffffffu, ZPixmap, (uint32_t)g_gslot.si.shmseg, 0);
            xcb_shm_get_image_reply_t *r = c_shm_get_image_reply(g_xcb, ck, &err);
            if (r && r->depth == 8 && r->size >= bpl * h) { free(r); *stride = (int)bpl; return (unsigned char *)g_gslot.si.shmaddr; }
            free(r); free(err);
        }
    }
    XImage *im = r_XGetImage(dpy, xid, x, y, w, h, AllPlanes, ZPixmap);
    if (!im) return NULL;
    if (im->depth != 8 || im->bits_per_pixel != 8) { im->f.destroy_image(im); return NULL; }
    *tofree = im; *stride = im->bytes_per_line;
    return (unsigned char *)im->data;
}

/* ---------- GdkWindow source (KPPX11_PIXBUFWIN=1) ----------
 * GTK 2.20 gdk_pixbuf_get_from_drawable on a window, per 256x64 tile: gdk_drawable_copy_to_image ->
 * class->get_composite_drawable (vtable +0x98) picks the paint-stack pixmap (tile inside a paint region), the window
 * itself (outside all) or a temporary composite pixmap (partly inside); the window path ends in
 * _gdk_x11_copy_to_image(impl): XGrabServer, XTranslateCoordinates(root->impl), XGetGeometry(impl), tile clipped to
 * (window ∩ screen), XGetSubImage of the clipped part into the scratch image (the rest of the scratch keeps stale
 * bytes), ungrab, XSync; then rgb8() per tile.
 * Fast path: ONE get_composite_drawable for the whole rectangle. If the whole rectangle is inside a paint region,
 * every tile is (same pixmap, same offsets); if it is outside every region, every tile is (window). Partial overlap
 * (temporary composite pixmap) -> original. The rectangle must lie fully inside (pixmap) / (window ∩ screen), so
 * stock never leaves stale scratch bytes in the result; otherwise -> original. Pixels: one GetImage of the impl
 * window / paint pixmap (MIT-SHM via xcb with KPPX11_SHMGET, else xcb GetImage; errors come back in the reply),
 * geometry + translate + image pipelined in one round trip. Conversion = rgb8 (no-alpha dest only: GTK's rgb8a
 * builds its 256-entry table from index x1 upward and leaves remap[0..x1-1] uninitialised). */
typedef struct { uint8_t response_type, depth; uint16_t sequence; uint32_t length, root; int16_t x, y; uint16_t width, height, border; uint8_t pad[2]; } geom_reply_t;
typedef struct { uint8_t response_type, same_screen; uint16_t sequence; uint32_t length, child; int16_t dst_x, dst_y; } trans_reply_t;
typedef struct { uint8_t response_type, depth; uint16_t sequence; uint32_t length, visual; uint8_t pad[20]; } getimg_reply_t;
static struct {
    unsigned long (*wtype)(void), (*wimpl_type)(void); int (*viewable)(void *);
    void *(*screen)(void *); int (*sw)(void *), (*sh)(void *);
    Drawable (*root)(Display *); xcb_connection_t *(*xcb)(Display *);
    xcb_cookie_t (*geom)(xcb_connection_t *, uint32_t); geom_reply_t *(*geom_r)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);
    xcb_cookie_t (*trans)(xcb_connection_t *, uint32_t, uint32_t, int16_t, int16_t); trans_reply_t *(*trans_r)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);
    xcb_cookie_t (*img)(xcb_connection_t *, uint8_t, uint32_t, int16_t, int16_t, uint16_t, uint16_t, uint32_t);
    getimg_reply_t *(*img_r)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);
    xcb_cookie_t (*simg)(xcb_connection_t *, uint32_t, int16_t, int16_t, uint16_t, uint16_t, uint32_t, uint8_t, uint32_t, uint32_t);
    xcb_shm_get_image_reply_t *(*simg_r)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);
    int ok;
} W;

static int wsyms(void)
{
    if (W.ok) return W.ok > 0;
    W.ok = -1;
    if (!gsyms()) return 0;
    SYM(W.wtype, RTLD_DEFAULT, "gdk_window_object_get_type"); SYM(W.wimpl_type, RTLD_DEFAULT, "gdk_window_impl_x11_get_type");
    SYM(W.viewable, RTLD_DEFAULT, "gdk_window_is_viewable"); SYM(W.screen, RTLD_DEFAULT, "gdk_drawable_get_screen");
    SYM(W.sw, RTLD_DEFAULT, "gdk_screen_get_width"); SYM(W.sh, RTLD_DEFAULT, "gdk_screen_get_height");
    SYM(W.root, RTLD_DEFAULT, "XDefaultRootWindow");
    void *xx = dlopen("libX11-xcb.so.1", RTLD_LAZY), *xc = dlopen("libxcb.so.1", RTLD_LAZY), *xs = dlopen("libxcb-shm.so.0", RTLD_LAZY);
    if (xx) SYM(W.xcb, xx, "XGetXCBConnection");
    if (xc) { SYM(W.geom, xc, "xcb_get_geometry"); SYM(W.geom_r, xc, "xcb_get_geometry_reply");
              SYM(W.trans, xc, "xcb_translate_coordinates"); SYM(W.trans_r, xc, "xcb_translate_coordinates_reply");
              SYM(W.img, xc, "xcb_get_image"); SYM(W.img_r, xc, "xcb_get_image_reply"); }
    if (xs) { SYM(W.simg, xs, "xcb_shm_get_image"); SYM(W.simg_r, xs, "xcb_shm_get_image_reply"); }
    if (W.wtype && W.wimpl_type && W.viewable && W.screen && W.sw && W.sh && W.root && W.xcb && W.geom && W.geom_r &&
        W.trans && W.trans_r && W.img && W.img_r) W.ok = 1;
    else lg("pixbuf window path: symbols missing");
    return W.ok > 0;
}

static int rect_in(int x, int y, int w, int h, int rx, int ry, int rw, int rh)
{
    return rw > 0 && rh > 0 && x >= rx && y >= ry && (long)x + w <= (long)rx + rw && (long)y + h <= (long)ry + rh;
}

/* depth-8 pixels of X drawable xid at (x,y,w,h), one round trip. is_win: also check GDK's (window ∩ screen) clip.
   Returns pixel pointer + stride; *rep = reply/segment owner to free (NULL when the SHM segment is used). */
static const unsigned char *fetch_xcb(Display *dpy, uint32_t xid, int is_win, int x, int y, int w, int h, void *screen,
                                      int *stride, void **rep)
{
    *rep = NULL;
    xcb_connection_t *c = W.xcb(dpy);
    if (!c || w > 32767 || h > 32767 || x > 32767 || y > 32767) return NULL;
    size_t bpl = ((size_t)w + 3) & ~(size_t)3;
    int shm = g_get && W.simg && W.simg_r && dpy_ready(dpy) && g_bpp[8] == 8 && g_pad[8] == 32 &&
              bpl * h <= 2 * SEG_CAP && !seg_ensure(&g_gslot, bpl * h);
    xcb_cookie_t cg = {0}, ct = {0}, ci;
    if (is_win) { cg = W.geom(c, xid); ct = W.trans(c, (uint32_t)W.root(dpy), xid, 0, 0); }
    if (shm) ci = W.simg(c, xid, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0xffffffffu, ZPixmap, (uint32_t)g_gslot.si.shmseg, 0);
    else ci = W.img(c, ZPixmap, xid, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0xffffffffu);
    xcb_generic_error_t *e1 = NULL, *e2 = NULL, *e3 = NULL;
    geom_reply_t *g = is_win ? W.geom_r(c, cg, &e1) : NULL;
    trans_reply_t *t = is_win ? W.trans_r(c, ct, &e2) : NULL;
    void *ir = shm ? (void *)W.simg_r(c, ci, &e3) : (void *)W.img_r(c, ci, &e3);
    const unsigned char *px = NULL;
    int ok = ir != NULL && ((uint8_t *)ir)[1] == 8;
    if (ok && is_win) {   /* _gdk_x11_copy_to_image: window_rect = (0,0,geometry) ∩ (translate(root->win), screen size) */
        ok = g && t && g->depth == 8;
        if (ok) {
            int sx0 = t->dst_x, sy0 = t->dst_y, sw = W.sw(screen), sh = W.sh(screen);
            int x0 = sx0 > 0 ? sx0 : 0, y0 = sy0 > 0 ? sy0 : 0;
            int x1 = (sx0 + sw < g->width) ? sx0 + sw : g->width, y1 = (sy0 + sh < g->height) ? sy0 + sh : g->height;
            ok = rect_in(x, y, w, h, x0, y0, x1 - x0, y1 - y0);
            if (!ok) st_pbw_why[2]++;
        }
    } else if (!ok) st_pbw_why[3]++;
    if (ok) {
        if (shm) { if (((xcb_shm_get_image_reply_t *)ir)->size >= bpl * h) px = (unsigned char *)g_gslot.si.shmaddr; }
        else if ((size_t)((getimg_reply_t *)ir)->length * 4 >= bpl * h) { px = (unsigned char *)ir + 32; *rep = ir; ir = NULL; }
        if (!px) st_pbw_why[3]++;
    }
    if (e1 || e2 || e3) {
        static int nlog;
        if (nlog++ < 5) lg("pixbufwin: X error geom=%d trans=%d img=%d xid=0x%x %d,%d %dx%d", e1 ? e1->error_code : 0, e2 ? e2->error_code : 0, e3 ? e3->error_code : 0, xid, x, y, w, h);
    }
    free(e1); free(e2); free(e3); free(g); free(t); free(ir);
    if (px) *stride = (int)bpl;
    return px;
}

/* window source: returns result, or NO */
static GdkPixbuf *pixbuf_fast_win(GdkPixbuf *dest, void *src, GdkColormap_ *cm, int sx, int sy, int dx, int dy, int w, int h)
{
    GdkPixbuf *const NO = (GdkPixbuf *)-1;
    if (!wsyms()) return NO;
    /* argument checks of gdk_pixbuf_get_from_drawable (any failure -> original, which warns/returns NULL) */
    if (!W.viewable(src) || w <= 0 || h <= 0 || sx < 0 || sy < 0) { st_pbw_why[0]++; return NO; }
    if (dest && G.nch(dest) != 3) { st_pbw_why[4]++; return NO; }
    if (!dest && (dx || dy)) { st_pbw_why[0]++; return NO; }
    if (dest && (dx < 0 || dy < 0 || dx + w > G.pw(dest) || dy + h > G.ph(dest))) { st_pbw_why[0]++; return NO; }
    void **klass = *(void ***)src;
    void *(*composite)(void *, int, int, int, int, int *, int *) = klass ? (void *(*)(void *, int, int, int, int, int *, int *))klass[0x98 / 4] : NULL;
    if (!composite) { st_pbw_why[1]++; return NO; }
    int cx = 0, cy = 0;
    void *comp = composite(src, sx, sy, w, h, &cx, &cy);
    if (!comp) { st_pbw_why[1]++; return NO; }
    uint32_t xid = 0; int is_win = 0, px0 = sx - cx, py0 = sy - cy;
    if (comp == src) {                      /* outside every paint region: the window itself */
        void *impl = ((void **)src)[3];     /* GdkWindowObject.impl (+12), GdkDrawableImplX11.xid (+20) */
        /* never seen on KPP (all read-backs hit the paint pixmap) and not parity-tested: only with KPPX11_PIXBUFWIN=2 */
        if (g_pixbufwin >= 2 && impl && G.is_a(impl, W.wimpl_type())) { xid = ((uint32_t *)impl)[5]; is_win = 1; st_pbw_win++; }
    } else if (G.is_a(comp, G.pixmap_type()) && ((unsigned *)comp)[1] >= 2) {   /* paint pixmap (paint holds a ref) */
        int pw, ph; G.size(comp, &pw, &ph);
        if (G.depth(comp) == 8 && rect_in(px0, py0, w, h, 0, 0, pw, ph)) { xid = (uint32_t)G.xid(comp); st_pbw_pm++; }
        else st_pbw_why[2]++;
    }
    static int nlog;
    if (nlog++ < 12) {
        Dl_info a, b; void *fa = klass[0xa0 / 4], *fb = (*(void ***)comp)[0xa0 / 4];
        long oa = dladdr(fa, &a) ? (long)((char *)fa - (char *)a.dli_fbase) : -1, ob = dladdr(fb, &b) ? (long)((char *)fb - (char *)b.dli_fbase) : -1;
        lg("pixbufwin: src=%p comp=%p %s ref=%u off=%d,%d rect=%d,%d %dx%d dest=%p d=%d,%d xid=0x%x copy_to_image src=0x%lx comp=0x%lx composite=0x%lx",
           src, comp, comp == src ? "window" : G.is_a(comp, G.pixmap_type()) ? "pixmap" : "other",
           ((unsigned *)comp)[1], cx, cy, sx, sy, w, h, dest, dx, dy, xid, oa, ob,
           dladdr((void *)composite, &a) ? (long)((char *)(void *)composite - (char *)a.dli_fbase) : -1);
    }
    G.unref(comp);
    if (!xid) { st_pbw_why[1]++; return NO; }
    if (px0 < 0 || py0 < 0) { st_pbw_why[2]++; return NO; }
    Display *dpy = G.xdpy(src);
    if (!dpy) return NO;
    int stride; void *rep;
    const unsigned char *p = fetch_xcb(dpy, xid, is_win, px0, py0, w, h, W.screen(src), &stride, &rep);
    if (!p) return NO;
    unsigned char lut[256][3]; int ident = 1;   /* rgb8(): R,G,B = colors[p & 0xff].red/green/blue >> 8 */
    for (int i = 0; i < 256; i++) {
        lut[i][0] = cm->colors[i].red >> 8; lut[i][1] = cm->colors[i].green >> 8; lut[i][2] = cm->colors[i].blue >> 8;
        ident &= lut[i][0] == i && lut[i][1] == i && lut[i][2] == i;
    }
    GdkPixbuf *out = dest ? dest : G.pnew(0, 0, 8, w, h);
    if (!out) { free(rep); return NULL; }   /* original returns NULL too */
    int rs = G.rs(out);
    unsigned char *o0 = G.px(out) + (size_t)dy * rs + (size_t)dx * 3;
    for (int y = 0; y < h; y++) {
        const unsigned char *s = p + (size_t)y * stride; unsigned char *o = o0 + (size_t)y * rs;
        int x = 0;
        if (ident) {                        /* StaticGray 0..255 ramp: every byte replicated to R,G,B */
            for (; x + 16 <= w; x += 16) { uint8x16_t v = vld1q_u8(s + x); uint8x16x3_t t = {{ v, v, v }}; vst3q_u8(o + 3 * x, t); }
            for (; x < w; x++) o[3 * x] = o[3 * x + 1] = o[3 * x + 2] = s[x];
        } else
            for (; x < w; x++) { const unsigned char *c = lut[s[x]]; o[3 * x] = c[0]; o[3 * x + 1] = c[1]; o[3 * x + 2] = c[2]; }
    }
    free(rep);
    if (ident) st_pbw_ident++;
    st_pbw_fast++; st_pbw_px += (unsigned long)w * h;
    return out;
}

/* returns the result pixbuf, or (GdkPixbuf *)-1 = not handled (caller runs the original) */
static GdkPixbuf *pixbuf_fast(GdkPixbuf *dest, void *src, void *cmap, int sx, int sy, int dx, int dy, int w, int h)
{
    GdkPixbuf *const NO = (GdkPixbuf *)-1;
    if (!src || !gsyms()) return NO;
    if (g_pixbufwin && wsyms() && G.is_a(src, W.wtype())) {
        if (dest && (G.cs(dest) != 0 || (G.nch(dest) != 3 && G.nch(dest) != 4) || G.bps(dest) != 8)) return NO;
        GdkColormap_ *cm = cmap ? cmap : G.cmap(src);
        if (!cm || G.depth(src) != 8 || !cm->visual || cm->visual->depth != 8 || cm->visual->type > 3 || !cm->colors || cm->size < 256) {
            st_pbw_why[5]++; return NO;
        }
        return pixbuf_fast_win(dest, src, cm, sx, sy, dx, dy, w, h);
    }
    if (!g_pixbuf) return NO;
    if (!G.is_a(src, G.pixmap_type())) { st_pb_why[0]++; return NO; }
    if (!dest && (w < 0 || h < 0)) return NO;
    if (dest && (G.cs(dest) != 0 || (G.nch(dest) != 3 && G.nch(dest) != 4) || G.bps(dest) != 8)) return NO;
    GdkColormap_ *cm = cmap ? cmap : G.cmap(src);
    if (!cm || G.depth(src) != 8 || !cm->visual || cm->visual->depth != 8 || cm->visual->type > 3 || !cm->colors) {
        st_pb_why[1]++;
        if (st_pb_why[1] < 4) lg("pixbuf: cmap=%p depth=%d vis=%p visdepth=%d type=%d colors=%p size=%d", (void *)cm, G.depth(src),
                                 cm ? (void *)cm->visual : 0, cm && cm->visual ? cm->visual->depth : -1, cm && cm->visual ? cm->visual->type : -1,
                                 cm ? (void *)cm->colors : 0, cm ? cm->size : -1);
        return NO;
    }
    if (cm->size < 256) { st_pb_why[2]++; return NO; }         /* rgb8 indexes colors[0..255] */
    int pw, ph; G.size(src, &pw, &ph);
    if (w < 0) w = pw;
    if (h < 0) h = ph;
    if (sx < 0 || sy < 0 || sx + w > pw || sy + h > ph) { st_pb_why[3]++; return NO; }
    if (dest && (dx < 0 || dy < 0 || dx + w > G.pw(dest) || dy + h > G.ph(dest))) return NO;
    if (!dest && (dx < 0 || dy < 0 || dx + w > w || dy + h > h)) return NO;
    if (w == 0 || h == 0) return NO;
    Display *dpy = G.xdpy(src);
    if (!dpy) return NO;
    unsigned char lut[256][3];
    for (int i = 0; i < 256; i++) { lut[i][0] = cm->colors[i].red >> 8; lut[i][1] = cm->colors[i].green >> 8; lut[i][2] = cm->colors[i].blue >> 8; }
    int stride; XImage *tf;
    const unsigned char *p = fetch8(dpy, G.xid(src), sx, sy, w, h, &stride, &tf);
    if (!p) return NO;
    GdkPixbuf *out = dest ? dest : G.pnew(0, 0, 8, w, h);
    if (!out) { if (tf) tf->f.destroy_image(tf); return NULL; }   /* original returns NULL too */
    int n = G.nch(out), rs = G.rs(out);
    unsigned char *o0 = G.px(out) + (size_t)dy * rs + (size_t)dx * n;
    for (int y = 0; y < h; y++) {
        const unsigned char *s = p + (size_t)y * stride; unsigned char *o = o0 + (size_t)y * rs;
        if (n == 3) for (int x = 0; x < w; x++, o += 3) { const unsigned char *c = lut[s[x]]; o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; }
        else for (int x = 0; x < w; x++, o += 4) { const unsigned char *c = lut[s[x]]; o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 0xff; }
    }
    if (tf) tf->f.destroy_image(tf);
    return out;
}

GdkPixbuf *gdk_pixbuf_get_from_drawable(GdkPixbuf *dest, void *src, void *cmap, int sx, int sy, int dx, int dy, int w, int h)
{
    if (!r_get_from_drawable) SYM(r_get_from_drawable, RTLD_NEXT, "gdk_pixbuf_get_from_drawable");
    if (!(g_pixbuf || g_pixbufwin) || getpid() != g_pid) return r_get_from_drawable(dest, src, cmap, sx, sy, dx, dy, w, h);
    pthread_mutex_lock(&g_mu);
    GdkPixbuf *res = (GdkPixbuf *)-1;
    if (!g_verify) res = pixbuf_fast(dest, src, cmap, sx, sy, dx, dy, w, h);
    else {
        GdkPixbuf *mine = dest ? G.pcopy ? G.pcopy(dest) : NULL : NULL;
        GdkPixbuf *f = (dest && !mine) ? (GdkPixbuf *)-1 : pixbuf_fast(mine, src, cmap, sx, sy, dx, dy, w, h);
        if (f != (GdkPixbuf *)-1) {
            pthread_mutex_unlock(&g_mu);
            GdkPixbuf *o = r_get_from_drawable(dest, src, cmap, sx, sy, dx, dy, w, h);
            pthread_mutex_lock(&g_mu);
            int bad = !o || !f;
            if (!bad) {
                int n = G.nch(o), ww = w < 0 ? G.pw(o) : w, hh = h < 0 ? G.ph(o) : h;
                for (int y = 0; y < hh && !bad; y++)
                    if (memcmp(G.px(o) + (size_t)(dy + y) * G.rs(o) + (size_t)dx * n, G.px(f) + (size_t)(dy + y) * G.rs(f) + (size_t)dx * n, (size_t)ww * n)) {
                        bad = 1; lg("VERIFY PIXBUF MISMATCH row %d (%dx%d n=%d)", y, ww, hh, n);
                    }
            }
            if (bad) st_vbad++; else st_vok++;
            if (f) G.unref(f);
            if (g_stats) stats_tick();
            pthread_mutex_unlock(&g_mu);
            return o;                       /* verify mode: the app gets the original */
        }
        if (mine) G.unref(mine);
    }
    if (res != (GdkPixbuf *)-1) st_pb_fast++; else st_pb_orig++;
    if (g_stats) stats_tick();
    pthread_mutex_unlock(&g_mu);
    if (res != (GdkPixbuf *)-1) return res;
    return r_get_from_drawable(dest, src, cmap, sx, sy, dx, dy, w, h);
}

/* ---------- gdk_draw_pixbuf RGB -> StaticGray 8 (KPPX11_DRAWPB=1) ----------
 * GTK 2.20 on an 8-bit StaticGray/GrayScale visual converts a no-alpha RGB pixbuf with gdk_rgb_convert_gray8
 * (libgdk 0x29584: out = (g + ((r + b) >> 1)) >> 1, no dither) into 256x64 scratch images and XPutImage's them.
 * Fast path: the same formula with NEON halving adds (vhadd = (a+b)>>1, exact) into a gray buffer, then
 * gdk_draw_gray_image (gdk_rgb_convert_gray8_gray = row copy into the same scratch tiles, same window/paint-stack
 * drawing layer). Only: RGB, 3 channels, no alpha, 8 bit, gc given, drawable depth 8 with a gray visual, source
 * and destination rectangles fully inside pixbuf / drawable. VERIFY: both ways into two scratch pixmaps, compared. */
static void (*r_draw_pixbuf)(void *, void *, GdkPixbuf *, int, int, int, int, int, int, int, int, int);
static struct {
    void (*gray)(void *, void *, int, int, int, int, int, const unsigned char *, int);
    void *(*visual)(void *); int (*alpha)(GdkPixbuf *);
    void *(*pixmap_new)(void *, int, int, int); void *(*gc_new)(void *);
    int ok;
} D;
static unsigned char *g_dpbuf; static size_t g_dpcap; static int g_dpbusy;

static int dsyms(void)
{
    if (D.ok) return D.ok > 0;
    D.ok = -1;
    if (!gsyms()) return 0;
    SYM(D.gray, RTLD_DEFAULT, "gdk_draw_gray_image"); SYM(D.visual, RTLD_DEFAULT, "gdk_drawable_get_visual");
    SYM(D.alpha, RTLD_DEFAULT, "gdk_pixbuf_get_has_alpha"); SYM(D.pixmap_new, RTLD_DEFAULT, "gdk_pixmap_new");
    SYM(D.gc_new, RTLD_DEFAULT, "gdk_gc_new");
    if (D.gray && D.visual && D.alpha && D.pixmap_new && D.gc_new) D.ok = 1; else lg("drawpb: gdk symbols missing");
    return D.ok > 0;
}

static void gray8_rows(const unsigned char *src, int srs, unsigned char *dst, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const unsigned char *s = src + (size_t)y * srs; unsigned char *o = dst + (size_t)y * w;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            uint8x16x3_t v = vld3q_u8(s + 3 * x);
            vst1q_u8(o + x, vhaddq_u8(v.val[1], vhaddq_u8(v.val[0], v.val[2])));
        }
        for (; x < w; x++) o[x] = (unsigned char)((s[3 * x + 1] + ((s[3 * x] + s[3 * x + 2]) >> 1)) >> 1);
    }
}

/* 1 = arguments are in the fast-path domain */
static int drawpb_ok(void *drawable, void *gc, GdkPixbuf *pb, int sx, int sy, int dx, int dy, int w, int h)
{
    if (!drawable || !gc || !pb || !dsyms()) return 0;
    if (G.cs(pb) != 0 || G.nch(pb) != 3 || D.alpha(pb) || G.bps(pb) != 8) return 0;
    if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || sx + w > G.pw(pb) || sy + h > G.ph(pb)) return 0;
    if (G.depth(drawable) != 8) return 0;
    GdkVisual_ *v = D.visual(drawable);
    if (!v || v->depth != 8 || (v->type != 0 && v->type != 1)) return 0;   /* STATIC_GRAY, GRAYSCALE */
    int dw, dh; G.size(drawable, &dw, &dh);
    return rect_in(dx, dy, w, h, 0, 0, dw, dh);
}

static unsigned char *drawpb_convert(GdkPixbuf *pb, int sx, int sy, int w, int h)
{
    size_t need = (size_t)w * h;
    if (need > g_dpcap) {                   /* grows to the largest cover once, then reused */
        unsigned char *n = realloc(g_dpbuf, need);
        if (!n) return NULL;
        g_dpbuf = n; g_dpcap = need;
    }
    int rs = G.rs(pb);
    gray8_rows(G.px(pb) + (size_t)sy * rs + (size_t)sx * 3, rs, g_dpbuf, w, h);
    return g_dpbuf;
}

static void drawpb_verify(void *drawable, GdkPixbuf *pb, int sx, int sy, int w, int h, int dither, int xd, int yd)
{
    void *pa = D.pixmap_new(drawable, w, h, -1), *pbm = D.pixmap_new(drawable, w, h, -1);
    void *ga = pa ? D.gc_new(pa) : NULL, *gb = pbm ? D.gc_new(pbm) : NULL;
    unsigned char *buf = (pa && pbm && ga && gb) ? drawpb_convert(pb, sx, sy, w, h) : NULL;
    if (buf) {
        r_draw_pixbuf(pa, ga, pb, sx, sy, 0, 0, w, h, dither, xd, yd);
        D.gray(pbm, gb, 0, 0, w, h, dither, buf, w);
        Display *dpy = G.xdpy(pa);
        XImage *A = r_XGetImage(dpy, G.xid(pa), 0, 0, w, h, AllPlanes, ZPixmap), *B = r_XGetImage(dpy, G.xid(pbm), 0, 0, w, h, AllPlanes, ZPixmap);
        int bad = !A || !B || A->bits_per_pixel != 8 || B->bits_per_pixel != 8;
        for (int y = 0; !bad && y < h; y++)
            if (memcmp(A->data + (size_t)y * A->bytes_per_line, B->data + (size_t)y * B->bytes_per_line, w)) {
                bad = 1; lg("VERIFY DRAWPB MISMATCH row %d (%dx%d)", y, w, h);
            }
        if (bad) st_dp_vbad++; else st_dp_vok++;
        if (A) A->f.destroy_image(A);
        if (B) B->f.destroy_image(B);
    }
    if (ga) G.unref(ga);
    if (gb) G.unref(gb);
    if (pa) G.unref(pa);
    if (pbm) G.unref(pbm);
}

void gdk_draw_pixbuf(void *drawable, void *gc, GdkPixbuf *pb, int sx, int sy, int dx, int dy, int w, int h, int dither, int xd, int yd)
{
    if (!r_draw_pixbuf) SYM(r_draw_pixbuf, RTLD_NEXT, "gdk_draw_pixbuf");
    if (!g_drawpb || getpid() != g_pid || g_dpbusy) { r_draw_pixbuf(drawable, gc, pb, sx, sy, dx, dy, w, h, dither, xd, yd); return; }
    g_dpbusy = 1;                           /* also guards the shared buffer (KPP draws on its main thread only) */
    if (w == -1 && pb && gsyms()) w = G.pw(pb);   /* gdk_draw_pixbuf: -1 = pixbuf width/height */
    if (h == -1 && pb && gsyms()) h = G.ph(pb);
    int ok = drawpb_ok(drawable, gc, pb, sx, sy, dx, dy, w, h);
    static int nlog;
    if (!ok && nlog < 8 && pb && gsyms() && dsyms()) {
        nlog++; GdkVisual_ *v = drawable ? D.visual(drawable) : NULL;
        lg("drawpb orig: gc=%p cs=%d nch=%d alpha=%d bps=%d src=%d,%d %dx%d of %dx%d dest=%d,%d depth=%d vis=%d/%d dither=%d",
           gc, G.cs(pb), G.nch(pb), D.alpha(pb), G.bps(pb), sx, sy, w, h, G.pw(pb), G.ph(pb), dx, dy,
           drawable ? G.depth(drawable) : -1, v ? v->type : -1, v ? v->depth : -1, dither);
    }
    unsigned char *buf = NULL;
    if (ok && g_verify) drawpb_verify(drawable, pb, sx, sy, w, h, dither, xd, yd);
    else if (ok) buf = drawpb_convert(pb, sx, sy, w, h);
    if (buf) { D.gray(drawable, gc, dx, dy, w, h, dither, buf, w); st_dp_fast++; st_dp_px += (unsigned long)w * h; }
    else { st_dp_orig++; r_draw_pixbuf(drawable, gc, pb, sx, sy, dx, dy, w, h, dither, xd, yd); }
    g_dpbusy = 0;
    if (g_stats) { pthread_mutex_lock(&g_mu); stats_tick(); pthread_mutex_unlock(&g_mu); }
}
