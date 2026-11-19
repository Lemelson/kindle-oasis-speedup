/* kinject: evdev injector + per-turn CPU/settle meter for KPPMainApp on the Kindle (armv7, static).
 *
 *   kinject info  DEV                         print name + ABS ranges of an evdev node
 *   kinject key   DEV CODE [N [GAP_MS]]       N press/release pairs (default 1)
 *   kinject tap   DEV X Y [HOLD_MS]           one-finger tap, coordinates in DEVICE units (see info)
 *   kinject swipe DEV X0 Y0 X1 Y1 [DUR_MS [STEPS]]
 *   kinject now                               CLOCK_MONOTONIC in ms (busybox date has no ms)
 *   kinject turn  [opts] -- ACTION...         run ACTION (key/tap/swipe args as above, or "none"),
 *                                             then sample KPPMainApp threads until quiet; one JSON line.
 *      opts: -p PID (KPPMainApp pid, required)   -x XPID (X server pid, optional)
 *            -q QUIET_MS (150)  -i PERIOD_MS (5)  -t TIMEOUT_MS (8000)  -m MIN_ACTIVE_US (300)
 *            -I IRQNAME (substring of a /proc/interrupts line to track, e.g. epdc; optional)
 *            -l LABEL (free text stored in "label")
 *   Settle rule: a sample is "active" if MAIN+JSQueue on-CPU time grew by >= MIN_ACTIVE_US in that
 *   period (schedstat ns; if schedstat is missing, any utime+stime tick increase). Settled = no active
 *   sample for QUIET_MS. settle_ms = end of last active sample - injection time.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <linux/input.h>

struct ev32 { uint32_t sec, usec; uint16_t type, code; int32_t value; };  /* armv7 struct input_event */

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void msleep(double ms) { if (ms <= 0) return; struct timespec t = { (time_t)(ms / 1000), (long)((ms - (long)(ms / 1000) * 1000) * 1e6) }; nanosleep(&t, 0); }

static int ev_fd = -1;
static struct ev32 ebuf[64]; static int en;
static void ev(int type, int code, int value) { ebuf[en].sec = ebuf[en].usec = 0; ebuf[en].type = type; ebuf[en].code = code; ebuf[en].value = value; en++; }
static void syn(void) { ev(EV_SYN, SYN_REPORT, 0); if (write(ev_fd, ebuf, en * sizeof(struct ev32)) < 0) perror("write"); en = 0; }
static int opendev(const char *d) { ev_fd = open(d, O_WRONLY); if (ev_fd < 0) { perror(d); exit(2); } return ev_fd; }

static void do_key(char **a, int n) {
  if (n < 2) { fprintf(stderr, "key DEV CODE [N [GAP]]\n"); exit(1); }
  opendev(a[0]); int code = atoi(a[1]), cnt = n > 2 ? atoi(a[2]) : 1; double gap = n > 3 ? atof(a[3]) : 300;
  for (int i = 0; i < cnt; i++) { ev(EV_KEY, code, 1); syn(); msleep(60); ev(EV_KEY, code, 0); syn(); if (i + 1 < cnt) msleep(gap); }
}
static int trk = 100;
static void touch_down(int x, int y) {
  ev(EV_KEY, BTN_TOUCH, 1); ev(EV_ABS, ABS_MT_TRACKING_ID, trk++ & 0xffff); ev(EV_ABS, ABS_MT_POSITION_X, x); ev(EV_ABS, ABS_MT_POSITION_Y, y);
  ev(EV_ABS, ABS_MT_PRESSURE, 120); syn(); ev(EV_KEY, BTN_TOOL_FINGER, 1); syn();
}
static void touch_move(int x, int y) { ev(EV_ABS, ABS_MT_POSITION_X, x); ev(EV_ABS, ABS_MT_POSITION_Y, y); syn(); }
static void touch_up(void) { ev(EV_ABS, ABS_MT_TRACKING_ID, -1); syn(); ev(EV_KEY, BTN_TOUCH, 0); ev(EV_KEY, BTN_TOOL_FINGER, 0); syn(); }
static void do_tap(char **a, int n) {
  if (n < 3) { fprintf(stderr, "tap DEV X Y [HOLD]\n"); exit(1); }
  opendev(a[0]); int x = atoi(a[1]), y = atoi(a[2]); double hold = n > 3 ? atof(a[3]) : 80;
  touch_down(x, y); msleep(hold / 2); touch_move(x, y); msleep(hold / 2); touch_up();
}
static void do_swipe(char **a, int n) {
  if (n < 5) { fprintf(stderr, "swipe DEV X0 Y0 X1 Y1 [DUR [STEPS]]\n"); exit(1); }
  opendev(a[0]); int x0 = atoi(a[1]), y0 = atoi(a[2]), x1 = atoi(a[3]), y1 = atoi(a[4]);
  double dur = n > 5 ? atof(a[5]) : 200; int st = n > 6 ? atoi(a[6]) : 15;
  touch_down(x0, y0);
  for (int i = 1; i <= st; i++) { msleep(dur / st); touch_move(x0 + (x1 - x0) * i / st, y0 + (y1 - y0) * i / st); }
  msleep(13); touch_up();
}
static void do_info(char **a, int n) {
  if (n < 1) exit(1);
  int fd = open(a[0], O_RDONLY); if (fd < 0) { perror(a[0]); exit(2); }
  char name[128] = ""; ioctl(fd, EVIOCGNAME(sizeof name), name); printf("name=%s\n", name);
  for (int c = 0; c < ABS_CNT; c++) { struct input_absinfo ai; if (ioctl(fd, EVIOCGABS(c), &ai) == 0 && (ai.maximum || ai.minimum)) printf("abs 0x%02x min=%d max=%d res=%d\n", c, ai.minimum, ai.maximum, ai.resolution); }
}
static int run_action(char **a, int n) {
  if (n < 1) return -1;
  if (!strcmp(a[0], "key")) do_key(a + 1, n - 1); else if (!strcmp(a[0], "tap")) do_tap(a + 1, n - 1);
  else if (!strcmp(a[0], "swipe")) do_swipe(a + 1, n - 1); else if (!strcmp(a[0], "none")) ; else return -1;
  return 0;
}

/* ---------- sampling ---------- */
static int rd(const char *p, char *buf, int sz) { int fd = open(p, O_RDONLY); if (fd < 0) return -1; int r = read(fd, buf, sz - 1); close(fd); if (r < 0) r = 0; buf[r] = 0; return r; }
static long ticks_of(const char *path) {           /* utime+stime of a stat file */
  char b[1024]; if (rd(path, b, sizeof b) <= 0) return -1; char *p = strrchr(b, ')'); if (!p) return -1; p += 2;
  long v[16]; int i = 0; char *s = p; while (i < 13 && s) { s = strchr(s, ' '); if (!s) break; s++; i++; if (i >= 11) v[i] = strtol(s, 0, 10); }
  return v[11] + v[12];                              /* after state: fields 14,15 of stat = index 11,12 after ")" */
}
static long long sched_ns(const char *path) { char b[128]; if (rd(path, b, sizeof b) <= 0) return -1; return strtoll(b, 0, 10); }
static long long irq_count(const char *name) {
  static char b[16384]; if (!name || rd("/proc/interrupts", b, sizeof b) <= 0) return -1;
  for (char *l = b; l && *l; ) { char *e = strchr(l, '\n'); if (e) *e = 0;
    if (strstr(l, name)) { char *s = strchr(l, ':'); long long sum = 0; if (s) { s++; for (;;) { char *q; long long v = strtoll(s, &q, 10); if (q == s) break; sum += v; s = q; } } return sum; }
    l = e ? e + 1 : 0; }
  return -1;
}
static long sys_busy(void) { char b[512]; if (rd("/proc/stat", b, sizeof b) <= 0) return -1; long u, ni, sy, id, io, irq, sirq; sscanf(b, "cpu %ld %ld %ld %ld %ld %ld %ld", &u, &ni, &sy, &id, &io, &irq, &sirq); return u + ni + sy + irq + sirq; }

#define MAXT 512
struct th { int tid; char comm[20]; long t0, t1; };
static int snap_threads(int pid, struct th *t, int first) {
  char p[64]; snprintf(p, sizeof p, "/proc/%d/task", pid); DIR *d = opendir(p); if (!d) return 0; struct dirent *de; int n = 0;
  if (first) { while ((de = readdir(d)) && n < MAXT) { if (de->d_name[0] == '.') continue; t[n].tid = atoi(de->d_name);
      char q[96]; snprintf(q, sizeof q, "/proc/%d/task/%d/comm", pid, t[n].tid); if (rd(q, t[n].comm, sizeof t[n].comm) > 0) t[n].comm[strcspn(t[n].comm, "\n")] = 0;
      snprintf(q, sizeof q, "/proc/%d/task/%d/stat", pid, t[n].tid); t[n].t0 = ticks_of(q); t[n].t1 = t[n].t0; n++; }
  }
  closedir(d); return n;
}
static void snap_end(int pid, struct th *t, int n) { for (int i = 0; i < n; i++) { char q[96]; snprintf(q, sizeof q, "/proc/%d/task/%d/stat", pid, t[i].tid); long v = ticks_of(q); t[i].t1 = v < 0 ? t[i].t0 : v; } }
static int find_tid(int pid, const char *comm) {
  char p[64]; snprintf(p, sizeof p, "/proc/%d/task", pid); DIR *d = opendir(p); if (!d) return -1; struct dirent *de; int r = -1;
  while ((de = readdir(d))) { if (de->d_name[0] == '.') continue; char q[96], c[32]; snprintf(q, sizeof q, "%s/%s/comm", p, de->d_name);
    if (rd(q, c, sizeof c) > 0) { c[strcspn(c, "\n")] = 0; if (!strcmp(c, comm)) { r = atoi(de->d_name); break; } } }
  closedir(d); return r;
}

static int do_turn(int argc, char **argv) {
  int pid = 0, xpid = 0; double quiet = 150, per = 5, tmo = 8000, minact_us = 300; const char *irq = 0, *label = "";
  int i = 0;
  for (; i < argc; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if (i + 1 >= argc) break;
    char *o = argv[i], *v = argv[++i];
    if (!strcmp(o, "-p")) pid = atoi(v); else if (!strcmp(o, "-x")) xpid = atoi(v); else if (!strcmp(o, "-q")) quiet = atof(v);
    else if (!strcmp(o, "-i")) per = atof(v); else if (!strcmp(o, "-t")) tmo = atof(v); else if (!strcmp(o, "-m")) minact_us = atof(v);
    else if (!strcmp(o, "-I")) irq = v; else if (!strcmp(o, "-l")) label = v;
  }
  if (!pid) { fprintf(stderr, "turn needs -p PID\n"); return 1; }
  int main_tid = pid, js_tid = find_tid(pid, "JSQueue");
  char ms[64], js[64], mn[64], jn[64], ps[64], xs[64];
  snprintf(ms, sizeof ms, "/proc/%d/task/%d/stat", pid, main_tid); snprintf(js, sizeof js, "/proc/%d/task/%d/stat", pid, js_tid);
  snprintf(mn, sizeof mn, "/proc/%d/task/%d/schedstat", pid, main_tid); snprintf(jn, sizeof jn, "/proc/%d/task/%d/schedstat", pid, js_tid);
  snprintf(ps, sizeof ps, "/proc/%d/stat", pid); snprintf(xs, sizeof xs, "/proc/%d/stat", xpid);
  static struct th T[MAXT]; int nt = snap_threads(pid, T, 1);
  long m0 = ticks_of(ms), j0 = ticks_of(js), p0 = ticks_of(ps), x0 = xpid ? ticks_of(xs) : -1, s0 = sys_busy();
  long long mn0 = sched_ns(mn), jn0 = sched_ns(jn), ir0 = irq_count(irq);
  int have_ns = mn0 >= 0 && jn0 >= 0;
  double t0 = now_ms();
  if (run_action(argv + i, argc - i) < 0) { fprintf(stderr, "bad action\n"); return 1; }
  double t_inj_end = now_ms();
  long mp = m0, jp = j0; long long mnp = mn0, jnp = jn0, irp = ir0;
  double last_act = -1, first_act = -1, last_irq = -1, t; int nsamp = 0, nact = 0, nirq = 0, timeout = 0;
  for (;;) {
    msleep(per); t = now_ms(); nsamp++;
    long m = ticks_of(ms), j = ticks_of(js); int act;
    if (have_ns) { long long a = sched_ns(mn), b = sched_ns(jn); act = (a - mnp) + (b - jnp) >= minact_us * 1000; mnp = a; jnp = b; }
    else act = (m != mp) || (j != jp);
    mp = m; jp = j;
    if (act) { nact++; last_act = t; if (first_act < 0) first_act = t; }
    if (irq) { long long c = irq_count(irq); if (c != irp) { last_irq = t; nirq += (int)(c - irp); irp = c; } }
    double ref = last_act >= 0 ? last_act : t_inj_end;
    if (t - ref >= quiet && (last_act >= 0 || t - t0 >= 1500)) break;
    if (t - t0 >= tmo) { timeout = 1; break; }
  }
  double t_end = now_ms();
  long m1 = ticks_of(ms), j1 = ticks_of(js), p1 = ticks_of(ps), x1 = xpid ? ticks_of(xs) : -1, s1 = sys_busy();
  long long mn1 = sched_ns(mn), jn1 = sched_ns(jn);
  snap_end(pid, T, nt);
  printf("{\"label\":\"%s\",\"t0_ms\":%.1f,\"inject_ms\":%.1f,\"settle_ms\":%.1f,\"first_act_ms\":%.1f,\"wall_ms\":%.1f,"
         "\"main_ticks\":%ld,\"js_ticks\":%ld,\"proc_ticks\":%ld,\"x_ticks\":%ld,\"sys_busy_ticks\":%ld,"
         "\"main_ns\":%lld,\"js_ns\":%lld,\"have_ns\":%d,\"samples\":%d,\"active\":%d,\"timeout\":%d,"
         "\"irq_count\":%d,\"last_irq_ms\":%.1f,\"quiet_ms\":%.0f,\"period_ms\":%.0f,\"min_active_us\":%.0f,\"threads\":{",
         label, t0, t_inj_end - t0, last_act >= 0 ? last_act - t0 : -1, first_act >= 0 ? first_act - t0 : -1, t_end - t0,
         m1 - m0, j1 - j0, p1 - p0, xpid ? x1 - x0 : -1, s1 - s0,
         have_ns ? mn1 - mn0 : -1, have_ns ? jn1 - jn0 : -1, have_ns, nsamp, nact, timeout,
         nirq, last_irq >= 0 ? last_irq - t0 : -1, quiet, per, minact_us);
  /* per-thread ticks, aggregated by comm, nonzero only */
  int first = 1;   /* main shares comm "KPPMainApp" with ~50 workers: MAIN is reported on its own */
  for (int a = 0; a < nt; a++) if (T[a].tid == pid) { printf("\"MAIN\":%ld", T[a].t1 - T[a].t0); first = 0; T[a].comm[0] = 0; }
  for (int a = 0; a < nt; a++) { if (!T[a].comm[0]) continue; long sum = 0; char nm[20]; strcpy(nm, T[a].comm);
    for (int b = a; b < nt; b++) if (T[b].comm[0] && !strcmp(nm, T[b].comm)) { sum += T[b].t1 - T[b].t0; T[b].comm[0] = 0; }
    for (char *q = nm; *q; q++) if (*q == '"' || *q == '\\') *q = '_';
    if (sum) { printf("%s\"%s\":%ld", first ? "" : ",", nm, sum); first = 0; } }
  printf("}}\n"); fflush(stdout);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: kinject info|key|tap|swipe|now|turn ...\n"); return 1; }
  const char *c = argv[1];
  if (!strcmp(c, "now")) { printf("%.1f\n", now_ms()); return 0; }
  if (!strcmp(c, "turn")) return do_turn(argc - 2, argv + 2);
  if (!strcmp(c, "info")) { do_info(argv + 2, argc - 2); return 0; }
  if (run_action(argv + 1, argc - 1) < 0) { fprintf(stderr, "unknown command %s\n", c); return 1; }
  return 0;
}
