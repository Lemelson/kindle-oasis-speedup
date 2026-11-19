/* libkppgrip.so: keep a reader text-selection drag alive at the screen edges (KPPMainApp, FW 5.18.2).
 *
 * Root cause (device, 19.11.2026): the Xorg multitouch driver's grip suppression ("Multitouch Grip Suppression",
 * GripSuppr=1) ignores touches in a ~40 px band along the screen edges. KUX turns the page during a selection only
 * when the selection end reaches the edge band, i.e. exactly when the finger goes into that dead zone: the driver
 * then drops the finger's motion AND its release (no "Sending button 1 up"), KUX never sees the end of the touch,
 * the selection stops following and no selection bar appears until the next touch ("hang").
 *
 * This library (v1, the version measured on the device) wraps GTK's event handler (gdk_event_handler_set): on the
 * GestureEngine long-press (GDK button 9) it saves the touch device's grip-suppression property and sets it to 0;
 * on the finger lift (button 1 release) or after KPPGRIP_MAX_MS (default 30 s) it writes the saved value back.
 * Known limits (fixed in kppgrip-v2-experimental.c, which however failed its device test): reacts to any long-press,
 * not only in the reader; a crash mid-gesture leaves the property at 0 until the next start.
 * Env: KPPGRIP=1 enables (default off), KPPGRIP_LOG=path, KPPGRIP_MAX_MS. Kill switch /mnt/us/DISABLE_KPPFAST.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stddef.h>

typedef void *LIPC;
typedef int LIPCcode;
typedef void Display;
typedef unsigned long Atom;
typedef struct { int deviceid; char *name; int use; int attachment; int enabled; int num_classes; void **classes; } XIDeviceInfo;

static int on = -1, maxms = 30000; static FILE *lg;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static Display *dpy; static int devid = -1; static Atom prop, ptype; static int pformat;
static int saved = -1;           /* value before we switched it off; -1 = not switched */
static double off_since;

/* libX11 / libXi entry points, resolved lazily */
static Display *(*pXOpenDisplay)(const char *);
static Atom (*pXInternAtom)(Display *, const char *, int);
static int (*pXFlush)(Display *);
static int (*pXFree)(void *);
static XIDeviceInfo *(*pXIQueryDevice)(Display *, int, int *);
static void (*pXIFreeDeviceInfo)(XIDeviceInfo *);
static int (*pXIGetProperty)(Display *, int, Atom, long, long, int, Atom, Atom *, int *, unsigned long *, unsigned long *, unsigned char **);
static void (*pXIChangeProperty)(Display *, int, Atom, Atom, int, int, unsigned char *, int);

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void logf_(const char *fmt, const char *a, int b) { if (lg) { fprintf(lg, "kppgrip %.0f: ", now_ms()); fprintf(lg, fmt, a, b); fputc('\n', lg); fflush(lg); } }

static void init(void) {
  if (on >= 0) return;
  const char *e = getenv("KPPGRIP");
  on = e && *e == '1' && access("/mnt/us/DISABLE_KPPFAST", F_OK) != 0;
  if ((e = getenv("KPPGRIP_MAX_MS"))) { int v = atoi(e); if (v >= 1000 && v <= 120000) maxms = v; }
  if (on && (e = getenv("KPPGRIP_LOG")) && *e) lg = fopen(e, "a");
}

static int x_ready(void) {
  if (dpy) return 1;
  if (devid == -2) return 0;     /* failed before: stay a pass-through */
  void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL), *xi = dlopen("libXi.so.6", RTLD_NOW | RTLD_GLOBAL);
  if (!x11 || !xi) { devid = -2; logf_("%s no libX11/libXi %d", "", 0); return 0; }
  pXOpenDisplay = dlsym(x11, "XOpenDisplay"); pXInternAtom = dlsym(x11, "XInternAtom"); pXFlush = dlsym(x11, "XFlush");
  pXFree = dlsym(x11, "XFree"); pXIQueryDevice = dlsym(xi, "XIQueryDevice"); pXIFreeDeviceInfo = dlsym(xi, "XIFreeDeviceInfo");
  pXIGetProperty = dlsym(xi, "XIGetProperty"); pXIChangeProperty = dlsym(xi, "XIChangeProperty");
  if (!pXOpenDisplay || !pXInternAtom || !pXFlush || !pXFree || !pXIQueryDevice || !pXIFreeDeviceInfo || !pXIGetProperty || !pXIChangeProperty) { devid = -2; return 0; }
  Display *d = pXOpenDisplay(NULL);
  if (!d) { devid = -2; logf_("%s XOpenDisplay failed %d", "", 0); return 0; }
  prop = pXInternAtom(d, "Multitouch Grip Suppression", 1 /* only if exists */);
  int n = 0; XIDeviceInfo *di = prop ? pXIQueryDevice(d, 0 /* XIAllDevices */, &n) : NULL;
  for (int i = 0; di && i < n && devid < 0; i++) {
    Atom t; int f; unsigned long ni, after; unsigned char *data = NULL;
    if (pXIGetProperty(d, di[i].deviceid, prop, 0, 1, 0, 0 /* AnyPropertyType */, &t, &f, &ni, &after, &data) == 0 && t && ni == 1 && data) {
      devid = di[i].deviceid; ptype = t; pformat = f; logf_("device %s id %d", di[i].name ? di[i].name : "?", devid);
    }
    if (data) pXFree(data);
  }
  if (di) pXIFreeDeviceInfo(di);
  if (devid < 0) { devid = -2; logf_("%s no device with the grip property %d", "", 0); return 0; }
  dpy = d; return 1;
}
static int get_val(void) {
  Atom t; int f; unsigned long ni, after; unsigned char *data = NULL; int v = -1;
  if (pXIGetProperty(dpy, devid, prop, 0, 1, 0, 0, &t, &f, &ni, &after, &data) == 0 && data && ni == 1)
    v = f == 8 ? data[0] : f == 16 ? *(short *)data : *(int *)data;
  if (data) pXFree(data);
  return v;
}
static void set_val(int v) {
  unsigned char b[4] = {0}; if (pformat == 8) b[0] = (unsigned char)v; else if (pformat == 16) *(short *)b = (short)v; else *(int *)b = v;
  pXIChangeProperty(dpy, devid, prop, ptype, pformat, 0 /* PropModeReplace */, b, 1); pXFlush(dpy);
}
static void restore_locked(const char *why) {
  if (saved < 0) return;
  set_val(saved); logf_("restored (%s) -> %d", why, saved); saved = -1;
}
static void *watchdog(void *a) {
  (void)a;
  for (;;) { usleep(500000); pthread_mutex_lock(&mu);
    if (saved >= 0 && now_ms() - off_since > maxms) restore_locked("timeout");
    pthread_mutex_unlock(&mu); }
  return NULL;
}
static void on_selection_json(const char *v) {
  if (!v || !strstr(v, "\"action\"")) return;
  int start = strstr(v, "\"action\":\"started\"") != NULL;
  int end = strstr(v, "\"action\":\"stabilized\"") || strstr(v, "\"action\":\"finalized\"");
  if (!start && !end) return;
  pthread_mutex_lock(&mu);
  if (x_ready()) {
    if (start && saved < 0) {
      int cur = get_val();
      if (cur > 0) { saved = cur; off_since = now_ms(); set_val(0); logf_("selection %s: grip off (was %d)", "started", cur);
        static int wd; if (!wd) { pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
          if (pthread_create(&t, &at, watchdog, NULL) == 0) wd = 1; } }
    } else if (end) restore_locked(strstr(v, "stabilized") ? "stabilized" : "finalized");
  }
  pthread_mutex_unlock(&mu);
}

/* GDK events of KPPMainApp's main loop: GestureEngine reports a long-press as button 9 (press+release) while the
 * finger stays down with button 1; the finger's lift is the button-1 release. Grip suppression is switched off from
 * the long-press to the lift (or the KPPGRIP_MAX_MS watchdog), i.e. only during a hold-and-drag (text selection). */
typedef void (*GdkEventFunc)(void *event, void *data);
static GdkEventFunc real_handler; static void *real_data;
static void kppgrip_handler(void *event, void *data) {
  if (event) {
    int type = *(int *)event; unsigned button = *(unsigned *)((char *)event + 40);
    if (type == 4 /* GDK_BUTTON_PRESS */ && button == 9) on_selection_json("\"action\":\"started\"");
    else if (type == 7 /* GDK_BUTTON_RELEASE */ && button == 1) on_selection_json("\"action\":\"finalized\"");
  }
  if (real_handler) real_handler(event, data);
}
void gdk_event_handler_set(GdkEventFunc func, void *data, void (*notify)(void *)) {
  static void (*real)(GdkEventFunc, void *, void (*)(void *));
  if (!real) real = (void (*)(GdkEventFunc, void *, void (*)(void *)))dlsym(RTLD_NEXT, "gdk_event_handler_set");
  init();
  if (!real) return;
  if (on == 1 && func) { real_handler = func; real_data = data; logf_("%s handler wrapped %d", "gdk", 0); real(kppgrip_handler, data, notify); return; }
  real(func, data, notify);
}
