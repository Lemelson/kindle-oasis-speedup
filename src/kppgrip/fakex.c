/* test only: fake libX11.so.6 (+ the XI calls; libXi.so.6 is an empty library that depends on this one).
 * The "device property" lives in a file (KPPFAKEX_PROP, default /tmp/fakex.prop: one byte) so that it survives the
 * test processes like the real one survives KPPMainApp. Window titles: fakex_title(xid, title). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned long Atom, XID;
typedef struct { int deviceid; char *name; int use; int attachment; int enabled; int num_classes; void **classes; } XIDeviceInfo;
typedef struct { int deviceid; int mask_len; unsigned char *mask; } XIEventMask;
typedef struct { int type; unsigned long serial; int send_event; void *display; int extension; int evtype; unsigned int cookie; void *data; } XGenericEventCookie;
typedef union { int type; XGenericEventCookie xcookie; long pad[24]; } XEvent;
typedef struct { int type; unsigned long serial; int send_event; void *display; int extension; int evtype; unsigned long time; int deviceid; Atom property; int what; } XIPropertyEvent;
#define DEV 7
#define PROP 100
static int g_sel, g_q, g_writes, g_opens, g_echo; static char g_titles[8][200];
int fakex_fail_title;
static const char *pf(void) { const char *e = getenv("KPPFAKEX_PROP"); return e ? e : "/tmp/fakex.prop"; }
int fakex_get(void) { FILE *f = fopen(pf(), "rb"); int c = f ? fgetc(f) : -1; if (f) fclose(f); return c; }
void fakex_set_external(int v) { FILE *f = fopen(pf(), "wb"); fputc(v, f); fclose(f); if (g_sel) g_q++; }   /* another client (winmgr) */
int fakex_writes(void) { return g_writes; }
int fakex_opens(void) { return g_opens; }
int fakex_selected(void) { return g_sel; }
void fakex_echo(int n) { g_echo = n; }      /* driver echoes n extra property events per write */
void fakex_title(int xid, const char *t) { snprintf(g_titles[xid & 7], 200, "%s", t); }
void *XOpenDisplay(const char *n) { g_opens++; return &g_sel; }
Atom XInternAtom(void *d, const char *n, int only) { return !strcmp(n, "Multitouch Grip Suppression") ? PROP : !strcmp(n, "_NET_WM_NAME") ? 101 : 0; }
int XFlush(void *d) { return 1; }
int XSync(void *d, int x) { return 1; }
int XFree(void *p) { free(p); return 1; }
int XPending(void *d) { return g_q; }
int XNextEvent(void *d, XEvent *e) { memset(e, 0, sizeof *e); e->type = 35; e->xcookie.evtype = 12; g_q--; return 0; }
int XGetEventData(void *d, XGenericEventCookie *c) { XIPropertyEvent *p = calloc(1, sizeof *p); p->deviceid = DEV; p->property = PROP; c->data = p; return 1; }
void XFreeEventData(void *d, XGenericEventCookie *c) { free(c->data); c->data = 0; }
XID XDefaultRootWindow(void *d) { return 1; }
int XGetWindowProperty(void *d, XID w, Atom a, long off, long len, int del, Atom req, Atom *t, int *f, unsigned long *n, unsigned long *after, unsigned char **data) {
    *t = 0; *f = 0; *n = 0; *after = 0; *data = 0;
    if (fakex_fail_title || a != 39 || !g_titles[w & 7][0]) return 0;       /* Success, property absent */
    *t = 31; *f = 8; *n = strlen(g_titles[w & 7]); *data = (unsigned char *)strdup(g_titles[w & 7]); return 0;
}
XIDeviceInfo *XIQueryDevice(void *d, int id, int *n) { XIDeviceInfo *di = calloc(2, sizeof *di); di[0].deviceid = 3; di[0].name = "keys"; di[1].deviceid = DEV; di[1].name = "multitouch"; *n = 2; return di; }
void XIFreeDeviceInfo(XIDeviceInfo *di) { free(di); }
int XIGetProperty(void *d, int dev, Atom a, long off, long len, int del, Atom req, Atom *t, int *f, unsigned long *n, unsigned long *after, unsigned char **data) {
    *t = 0; *f = 0; *n = 0; *after = 0; *data = 0;
    if (dev != DEV || a != PROP) return 0;
    int v = fakex_get(); if (v < 0) return 0;
    *t = 19; *f = 8; *n = 1; *data = malloc(4); (*data)[0] = v; return 0;
}
void XIChangeProperty(void *d, int dev, Atom a, Atom t, int f, int mode, unsigned char *data, int n) {
    if (dev != DEV || a != PROP || f != 8 || n != 1) { fprintf(stderr, "fakex: bad XIChangeProperty\n"); abort(); }
    FILE *fp = fopen(pf(), "wb"); fputc(data[0], fp); fclose(fp); g_writes++; if (g_sel) g_q += 1 + g_echo;
}
int XISelectEvents(void *d, XID w, XIEventMask *m, int n) { g_sel = m->mask_len > 1 && (m->mask[1] & 0x10) != 0; return 0; }
