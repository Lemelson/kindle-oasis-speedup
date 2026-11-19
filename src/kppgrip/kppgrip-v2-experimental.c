/* libkppgrip.so v2 (kpp6): keep a reader text-selection drag alive at the screen edges (KPPMainApp, FW 5.18.2).
 *
 * Root cause (device, 19.11.2026, reader/NOTES.md): the Xorg multitouch driver's grip suppression (XI device property
 * "Multitouch Grip Suppression" = 1) ignores touches in a ~40 px band along the screen edges. KUX turns the page
 * during a selection only when the selection end reaches the edge band, i.e. exactly when the finger is in that dead
 * zone: the driver then drops the finger's motion AND its release, KUX never sees the end of the touch ("hang").
 *
 * What the library does: it wraps the GDK event handler of KPPMainApp's main loop. GestureEngine reports a long-press
 * as button 9 (press + release) while the finger stays down with button 1; the lift is the button-1 release. From a
 * long-press to the lift the property is 0, afterwards it is put back.
 *
 * v2 lifecycle (answers the 19.11 audit of v1):
 *  - only in KPPMainApp itself (children inherit LD_PRELOAD: they are pass-through);
 *  - only for a long-press in the READER window: the X title (WM_NAME / _NET_WM_NAME) of the event window's toplevel
 *    must contain KPPGRIP_WIN (default "ID:com.lab126.krpp", the KUX book window; Home/Library is
 *    "ID:com.lab126.KPPMainApp_A:KPPMainApp"). Title unreadable or different -> nothing is touched.
 *    KPPGRIP_GATE=0 switches the window check off (v1 behaviour: any long-press).
 *  - restore never overwrites somebody else's value: the saved value is written back only while the property still
 *    is the 0 this library wrote. If it is non-zero, another writer (winmgr applies its grip setting on focus
 *    changes) has set a newer value and that value stays. Optional KPPGRIP_XEV=1: additionally watch XI property
 *    events of the device during the gesture; if somebody else wrote the property (even another 0) it is left alone.
 *  - no periodic thread: one helper thread sleeps in pthread_cond_wait with no timeout while nothing is switched
 *    off, and until the gesture's deadline (KPPGRIP_MAX_MS, default 30 s) while it is. A focus-out of the window
 *    during the gesture shortens the deadline to KPPGRIP_FOCUS_MS (default 2000) from that moment.
 *  - crash safety: while the property is 0 a tiny state file (/tmp/kppgrip.state: pid, saved value) exists. The
 *    next KPPMainApp start (upstart respawns it within seconds) finds the file of a dead process and puts the value
 *    back if the property still is 0. A normal exit restores in the destructor. (If kppfast falls back to a stock
 *    launch, winmgr's next focus change rewrites the property anyway.)
 *  - the event's `button` field is read only for button events; a finger lift costs one flag test when nothing is
 *    switched off (v1 took a mutex and could open the X connection on the first lift).
 * Uses its own X connection (libX11/libXi are already in the process), no LIPC, nothing persistent.
 * Env: KPPGRIP=1 enables (default off), KPPGRIP_LOG=path, KPPGRIP_MAX_MS, KPPGRIP_FOCUS_MS, KPPGRIP_GATE, KPPGRIP_WIN,
 * KPPGRIP_XEV, KPPGRIP_STATE=path. Kill switch /mnt/us/DISABLE_KPPFAST.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void Display;
typedef unsigned long Atom, XID;
typedef struct { int deviceid; char *name; int use; int attachment; int enabled; int num_classes; void **classes; } XIDeviceInfo;
typedef struct { int deviceid; int mask_len; unsigned char *mask; } XIEventMask;
typedef struct { int type; unsigned long serial; int send_event; Display *display; int extension; int evtype; unsigned int cookie; void *data; } XGenericEventCookie;
typedef union { int type; XGenericEventCookie xcookie; long pad[24]; } XEvent;
typedef struct { int type; unsigned long serial; int send_event; Display *display; int extension; int evtype; unsigned long time; int deviceid; Atom property; int what; } XIPropertyEvent;
#define GenericEvent 35
#define XI_PropertyEvent 12

static int on = -1, maxms = 30000, focusms = 2000, gate = 1, xev;
static char winpat[64] = "ID:com.lab126.krpp", statef[120] = "/tmp/kppgrip.state";
static FILE *lg;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv; static int cv_ok, wd_ok;
static Display *dpy; static int devid = -1; static Atom prop, ptype, a_netname; static int pformat;
static int saved = -1;           /* value before we switched it off; -1 = not switched (under mu) */
static volatile int g_off;       /* same fact for the unlocked fast path of the event handler */
static double deadline;          /* CLOCK_MONOTONIC ms; valid while saved >= 0 */
static int xev_sel;              /* property events are selected for this gesture */
static int pending_recover;

/* libX11 / libXi / gdk entry points, resolved lazily */
static Display *(*pXOpenDisplay)(const char *);
static Atom (*pXInternAtom)(Display *, const char *, int);
static int (*pXFlush)(Display *);
static int (*pXSync)(Display *, int);
static int (*pXFree)(void *);
static int (*pXPending)(Display *);
static int (*pXNextEvent)(Display *, XEvent *);
static int (*pXGetEventData)(Display *, XGenericEventCookie *);
static void (*pXFreeEventData)(Display *, XGenericEventCookie *);
static XID (*pXDefaultRootWindow)(Display *);
static int (*pXGetWindowProperty)(Display *, XID, Atom, long, long, int, Atom, Atom *, int *, unsigned long *, unsigned long *, unsigned char **);
static XIDeviceInfo *(*pXIQueryDevice)(Display *, int, int *);
static void (*pXIFreeDeviceInfo)(XIDeviceInfo *);
static int (*pXIGetProperty)(Display *, int, Atom, long, long, int, Atom, Atom *, int *, unsigned long *, unsigned long *, unsigned char **);
static void (*pXIChangeProperty)(Display *, int, Atom, Atom, int, int, unsigned char *, int);
static int (*pXISelectEvents)(Display *, XID, XIEventMask *, int);
static void *(*pgdk_window_get_toplevel)(void *);
static XID (*pgdk_x11_drawable_get_xid)(void *);
static void (*pgdk_error_trap_push)(void);
static int (*pgdk_error_trap_pop)(void);

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
__attribute__((format(printf, 1, 2))) static void klog(const char *fmt, ...) {
  if (!lg) return;
  va_list ap; va_start(ap, fmt); fprintf(lg, "kppgrip %.0f: ", now_ms()); vfprintf(lg, fmt, ap); va_end(ap); fputc('\n', lg); fflush(lg);
}
static int envi(const char *k, int def, int lo, int hi) { const char *e = getenv(k); if (!e || !*e) return def; int v = atoi(e); return v < lo || v > hi ? def : v; }

static void init(void) {
  if (on >= 0) return;
  const char *e = getenv("KPPGRIP"); char exe[200]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  const char *bn = "";
  if (n > 0) { exe[n] = 0; bn = strrchr(exe, '/'); bn = bn ? bn + 1 : exe; }
  on = e && e[0] == '1' && !e[1] && !strncmp(bn, "KPPMainApp", 10) && access("/mnt/us/DISABLE_KPPFAST", F_OK) != 0;
  if (!on) return;
  maxms = envi("KPPGRIP_MAX_MS", 30000, 1000, 120000);
  focusms = envi("KPPGRIP_FOCUS_MS", 2000, 100, 120000);
  gate = envi("KPPGRIP_GATE", 1, 0, 1);
  xev = envi("KPPGRIP_XEV", 0, 0, 1);
  if ((e = getenv("KPPGRIP_WIN")) && *e && strlen(e) < sizeof winpat) strcpy(winpat, e);
  if ((e = getenv("KPPGRIP_STATE")) && e[0] == '/' && strlen(e) < sizeof statef) strcpy(statef, e);
  if ((e = getenv("KPPGRIP_LOG")) && *e) lg = fopen(e, "ae");
}

static int x_ready(void) {
  if (dpy) return 1;
  if (devid == -2) return 0;     /* failed before: stay a pass-through */
  void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL), *xi = dlopen("libXi.so.6", RTLD_NOW | RTLD_GLOBAL);
  if (!x11 || !xi) { devid = -2; klog("no libX11/libXi"); return 0; }
  pXOpenDisplay = dlsym(x11, "XOpenDisplay"); pXInternAtom = dlsym(x11, "XInternAtom"); pXFlush = dlsym(x11, "XFlush");
  pXSync = dlsym(x11, "XSync"); pXFree = dlsym(x11, "XFree"); pXPending = dlsym(x11, "XPending"); pXNextEvent = dlsym(x11, "XNextEvent");
  pXGetEventData = dlsym(x11, "XGetEventData"); pXFreeEventData = dlsym(x11, "XFreeEventData");
  pXDefaultRootWindow = dlsym(x11, "XDefaultRootWindow"); pXGetWindowProperty = dlsym(x11, "XGetWindowProperty");
  pXIQueryDevice = dlsym(xi, "XIQueryDevice"); pXIFreeDeviceInfo = dlsym(xi, "XIFreeDeviceInfo");
  pXIGetProperty = dlsym(xi, "XIGetProperty"); pXIChangeProperty = dlsym(xi, "XIChangeProperty"); pXISelectEvents = dlsym(xi, "XISelectEvents");
  if (!pXOpenDisplay || !pXInternAtom || !pXFlush || !pXSync || !pXFree || !pXGetWindowProperty || !pXIQueryDevice || !pXIFreeDeviceInfo ||
      !pXIGetProperty || !pXIChangeProperty) { devid = -2; klog("X entry points missing"); return 0; }
  if (!pXPending || !pXNextEvent || !pXGetEventData || !pXFreeEventData || !pXDefaultRootWindow || !pXISelectEvents) xev = 0;
  pgdk_window_get_toplevel = dlsym(RTLD_DEFAULT, "gdk_window_get_toplevel"); pgdk_x11_drawable_get_xid = dlsym(RTLD_DEFAULT, "gdk_x11_drawable_get_xid");
  pgdk_error_trap_push = dlsym(RTLD_DEFAULT, "gdk_error_trap_push"); pgdk_error_trap_pop = dlsym(RTLD_DEFAULT, "gdk_error_trap_pop");
  if (!pgdk_error_trap_push || !pgdk_error_trap_pop) { pgdk_error_trap_push = NULL; pgdk_error_trap_pop = NULL; }
  Display *d = pXOpenDisplay(NULL);
  if (!d) { static int tries; if (++tries >= 3) devid = -2; klog("XOpenDisplay failed (%d)", tries); return 0; }  /* early start: retry later */
  prop = pXInternAtom(d, "Multitouch Grip Suppression", 1 /* only if exists */);
  a_netname = pXInternAtom(d, "_NET_WM_NAME", 1);
  int n = 0; XIDeviceInfo *di = prop ? pXIQueryDevice(d, 0 /* XIAllDevices */, &n) : NULL;
  for (int i = 0; di && i < n && devid < 0; i++) {
    Atom t; int f; unsigned long ni, after; unsigned char *data = NULL;
    if (pXIGetProperty(d, di[i].deviceid, prop, 0, 1, 0, 0 /* AnyPropertyType */, &t, &f, &ni, &after, &data) == 0 && t && ni == 1 && data &&
        (f == 8 || f == 16 || f == 32)) {
      devid = di[i].deviceid; ptype = t; pformat = f; klog("device %s id %d format %d", di[i].name ? di[i].name : "?", devid, f);
    }
    if (data) pXFree(data);
  }
  if (di) pXIFreeDeviceInfo(di);
  if (devid < 0) { devid = -2; klog("no device with the grip property"); return 0; }
  dpy = d; return 1;
}
static int get_val(void) {
  Atom t; int f; unsigned long ni, after; unsigned char *data = NULL; int v = -1;
  if (pXIGetProperty(dpy, devid, prop, 0, 1, 0, 0, &t, &f, &ni, &after, &data) == 0 && data && ni == 1)
    v = f == 8 ? data[0] : f == 16 ? *(short *)data : (int)*(long *)data;   /* format 32 = array of long in Xlib */
  if (data) pXFree(data);
  return v;
}
static void set_val(int v) {
  union { unsigned char b[8]; short s; long l; } u; memset(&u, 0, sizeof u);
  if (pformat == 8) u.b[0] = (unsigned char)v; else if (pformat == 16) u.s = (short)v; else u.l = v;
  pXIChangeProperty(dpy, devid, prop, ptype, pformat, 0 /* PropModeReplace */, u.b, 1); pXFlush(dpy);
}

/* XI property events of our device on our own connection (KPPGRIP_XEV=1): selected only during a gesture */
static void xev_select(int want) {
  unsigned char m[4] = {0}; XIEventMask em = {devid, sizeof m, m};
  if (want) m[XI_PropertyEvent >> 3] |= 1 << (XI_PropertyEvent & 7);
  pXISelectEvents(dpy, pXDefaultRootWindow(dpy), &em, 1);
}
static int xev_drain(void) { /* number of property events for our property that arrived since the last drain */
  int n = 0; pXSync(dpy, 0);
  while (pXPending(dpy) > 0) {
    XEvent ev; pXNextEvent(dpy, &ev);
    if (ev.type == GenericEvent && pXGetEventData(dpy, &ev.xcookie)) {
      if (ev.xcookie.evtype == XI_PropertyEvent) { XIPropertyEvent *p = ev.xcookie.data; if (p && p->deviceid == devid && p->property == prop) n++; }
      pXFreeEventData(dpy, &ev.xcookie);
    }
  }
  return n;
}

static void state_write(int val) {
  char b[64]; int n = snprintf(b, sizeof b, "%d %d\n", (int)getpid(), val);
  int fd = open(statef, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return;
  if (write(fd, b, n) != n) {}
  close(fd);
}
/* a previous KPPMainApp died while the property was 0: put its saved value back (with mu held, X ready) */
static void recover_locked(void) {
  char b[64] = {0}; int pid = 0, val = -1;
  pending_recover = 0;
  int fd = open(statef, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return;
  if (read(fd, b, sizeof b - 1) < 0) b[0] = 0;
  close(fd);
  if (sscanf(b, "%d %d", &pid, &val) != 2 || pid <= 1 || val < 1 || val > 255) { unlink(statef); return; }
  if (pid == (int)getpid()) return;                             /* ours (cannot happen at start) */
  if (kill(pid, 0) == 0 || errno == EPERM) {                    /* alive: only a KPPMainApp could own the file */
    char l[40], exe[200]; snprintf(l, sizeof l, "/proc/%d/exe", pid); ssize_t n = readlink(l, exe, sizeof exe - 1);
    const char *bn = "";
    if (n > 0) { exe[n] = 0; bn = strrchr(exe, '/'); bn = bn ? bn + 1 : exe; }
    if (!strncmp(bn, "KPPMainApp", 10)) { klog("state file of live pid %d: left alone", pid); return; }
  }
  int cur = get_val();
  if (cur == 0) { set_val(val); klog("recovered after pid %d died with grip off -> %d", pid, val); }
  else klog("state file of dead pid %d, property is %d: nothing to restore", pid, cur);
  unlink(statef);
}

static void restore_locked(const char *why) {
  if (saved < 0) return;
  int foreign = xev_sel ? xev_drain() : 0, cur = get_val();
  if (cur != 0) klog("%s: property is %d, set by someone else during the gesture: left alone (ours was %d)", why, cur, saved);
  else if (foreign > 0) klog("%s: %d foreign write(s) during the gesture, property 0 left alone (ours was %d)", why, foreign, saved);
  else { set_val(saved); klog("restored (%s) -> %d", why, saved); }
  if (xev_sel) { xev_select(0); xev_drain(); xev_sel = 0; }
  saved = -1; g_off = 0; unlink(statef);
  if (cv_ok) pthread_cond_signal(&cv);
}

/* the only helper thread: no timeout while nothing is switched off, the gesture's deadline while it is */
static void *watchdog(void *a) {
  (void)a;
  pthread_mutex_lock(&mu);
  for (;;) {
    if (saved < 0) { pthread_cond_wait(&cv, &mu); continue; }
    double left = deadline - now_ms();
    if (left <= 0) { restore_locked("timeout"); continue; }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long long ns = ts.tv_nsec + (long long)(left * 1e6) + 1000000;
    ts.tv_sec += ns / 1000000000LL; ts.tv_nsec = ns % 1000000000LL;
    pthread_cond_timedwait(&cv, &mu, &ts);
  }
  return NULL;
}
static int watchdog_start(void) {
  if (wd_ok) return 1;
  if (!cv_ok) {
    pthread_condattr_t ca;
    if (pthread_condattr_init(&ca) || pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) || pthread_cond_init(&cv, &ca)) return 0;
    cv_ok = 1;
  }
  pthread_t t; pthread_attr_t at; sigset_t all, old;
  pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&at, 65536);
  sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, &old);      /* the helper never runs the app's signal handlers */
  int r = pthread_create(&t, &at, watchdog, NULL);
  pthread_sigmask(SIG_SETMASK, &old, NULL); pthread_attr_destroy(&at);
  if (r == 0) wd_ok = 1;
  return wd_ok;
}

/* X title of the toplevel of a GdkWindow (main thread, inside event dispatch: the window is alive) */
static int window_title(void *gdkwin, char *out, size_t cap) {
  out[0] = 0;
  if (!gdkwin || !pgdk_window_get_toplevel || !pgdk_x11_drawable_get_xid) return 0;
  void *top = pgdk_window_get_toplevel(gdkwin);
  XID xid = top ? pgdk_x11_drawable_get_xid(top) : 0;
  if (!xid) return 0;
  Atom names[2] = {39 /* XA_WM_NAME */, a_netname};
  if (pgdk_error_trap_push) pgdk_error_trap_push();
  for (int i = 0; i < 2 && !out[0]; i++) {
    Atom t; int f; unsigned long ni, after; unsigned char *data = NULL;
    if (!names[i]) continue;
    if (pXGetWindowProperty(dpy, xid, names[i], 0, 64, 0, 0 /* AnyPropertyType */, &t, &f, &ni, &after, &data) == 0 && data && f == 8 && ni > 0) {
      size_t n = ni < cap - 1 ? ni : cap - 1; memcpy(out, data, n); out[n] = 0;
    }
    if (data) pXFree(data);
  }
  if (pgdk_error_trap_pop) { pXSync(dpy, 0); if (pgdk_error_trap_pop()) out[0] = 0; }
  return out[0] != 0;
}

static void long_press(void *event) {
  pthread_mutex_lock(&mu);
  if (saved < 0 && x_ready()) {
    if (pending_recover) recover_locked();
    char title[260]; int ok = 1;
    if (gate) {
      ok = window_title(*(void **)((char *)event + 4), title, sizeof title) && strstr(title, winpat) != NULL;
      if (!ok) klog("long-press outside the reader (window '%s'): ignored", title);
    }
    int cur = ok ? get_val() : -1;
    if (ok && cur > 0 && watchdog_start()) {
      if (xev) {            /* first use of this request type happens here, on the main thread, under GDK's error trap */
        if (pgdk_error_trap_push) pgdk_error_trap_push();
        xev_select(1); pXSync(dpy, 0);
        if (pgdk_error_trap_pop && pgdk_error_trap_pop()) { xev = 0; klog("XISelectEvents refused: property events off"); } else xev_sel = 1;
        if (xev_sel) xev_drain();
      }
      state_write(cur);
      set_val(0);
      if (xev_sel) xev_drain();     /* our own write (and whatever the driver echoes for it) is not "foreign" */
      saved = cur; deadline = now_ms() + maxms; g_off = 1;
      pthread_cond_signal(&cv);
      klog("long-press in the reader: grip off (was %d)", cur);
    }
  }
  pthread_mutex_unlock(&mu);
}
static void finger_up(void) { pthread_mutex_lock(&mu); restore_locked("release"); pthread_mutex_unlock(&mu); }
static void focus_out(void) {
  pthread_mutex_lock(&mu);
  if (saved >= 0 && deadline > now_ms() + focusms) { deadline = now_ms() + focusms; pthread_cond_signal(&cv); klog("focus lost during the gesture: deadline in %d ms", focusms); }
  pthread_mutex_unlock(&mu);
}

/* GDK events of KPPMainApp's main loop (GdkEventButton on armv7: type +0, window +4, button +40; GdkEventFocus: in +10) */
typedef void (*GdkEventFunc)(void *event, void *data);
static GdkEventFunc real_handler;
static void kppgrip_handler(void *event, void *data) {
  if (event) {
    int type = *(int *)event;
    if (type == 4 /* GDK_BUTTON_PRESS */) { if (*(unsigned *)((char *)event + 40) == 9) long_press(event); }
    else if (type == 7 /* GDK_BUTTON_RELEASE */) { if (g_off && *(unsigned *)((char *)event + 40) == 1) finger_up(); }
    else if (type == 12 /* GDK_FOCUS_CHANGE */) { if (g_off && *(short *)((char *)event + 10) == 0) focus_out(); }
  }
  if (real_handler) real_handler(event, data);
}
void gdk_event_handler_set(GdkEventFunc func, void *data, void (*notify)(void *)) {
  static void (*real)(GdkEventFunc, void *, void (*)(void *));
  if (!real) real = (void (*)(GdkEventFunc, void *, void (*)(void *)))dlsym(RTLD_NEXT, "gdk_event_handler_set");
  init();
  if (!real) return;
  if (on == 1 && func) {
    real_handler = func; klog("gdk handler wrapped (gate %d '%s', max %d ms, xev %d)", gate, winpat, maxms, xev);
    if (access(statef, F_OK) == 0) {       /* a previous process may have died with the property at 0 */
      pthread_mutex_lock(&mu); pending_recover = 1; if (x_ready()) recover_locked(); pthread_mutex_unlock(&mu);
    }
    real(kppgrip_handler, data, notify); return;
  }
  real(func, data, notify);
}
__attribute__((destructor)) static void kppgrip_fini(void) {
  if (on == 1 && g_off && pthread_mutex_trylock(&mu) == 0) { restore_locked("exit"); pthread_mutex_unlock(&mu); }
}
