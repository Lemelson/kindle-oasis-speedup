/* unit test of libkppgrip.so v2 against fake X/GDK (see griptest.sh). Run as KPPMainAppgriptest <scenario>. */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef void (*GdkEventFunc)(void *event, void *data);
void gdk_event_handler_set(GdkEventFunc, void *, void (*)(void *));
void fakegdk_dispatch(void *);
int fakex_get(void), fakex_writes(void), fakex_opens(void), fakex_selected(void); void fakex_set_external(int), fakex_title(int, const char *), fakex_echo(int);
extern int fakex_fail_title;
static int fails, seen;
static void app_handler(void *e, void *d) { seen++; }
#define CHK(c, msg) do { if (c) printf("PASS %s\n", msg); else { printf("FAIL %s (line %d)\n", msg, __LINE__); fails++; } } while (0)
static unsigned long W_READER = 1, W_LIB = 2, W_NONE = 3;
static void button(int type, unsigned b, unsigned long *win) { unsigned char ev[96] = {0}; *(int *)ev = type; *(void **)(ev + 4) = win; *(unsigned *)(ev + 40) = b; fakegdk_dispatch(ev); }
static void other(int type, unsigned junk40) { unsigned char ev[96] = {0}; *(int *)ev = type; *(unsigned *)(ev + 40) = junk40; fakegdk_dispatch(ev); }
static void focus(int in) { unsigned char ev[96] = {0}; *(int *)ev = 12; *(short *)(ev + 10) = in; fakegdk_dispatch(ev); }
static void lp(unsigned long *w) { button(4, 1, w); button(4, 9, w); button(7, 9, w); }    /* finger down, long-press reported */
static void up(unsigned long *w) { button(7, 1, w); }
static int state_exists(void) { return access(getenv("KPPGRIP_STATE"), F_OK) == 0; }
static long ctxsw_others(int *nthreads) { /* voluntary context switches of all threads but the main one */
    DIR *d = opendir("/proc/self/task"); struct dirent *e; long sum = 0; *nthreads = 0;
    while ((e = readdir(d))) { int tid = atoi(e->d_name); if (tid <= 0 || tid == getpid()) continue; char p[80], l[200]; snprintf(p, sizeof p, "/proc/self/task/%d/status", tid);
        FILE *f = fopen(p, "r"); if (!f) continue; (*nthreads)++; while (fgets(l, sizeof l, f)) if (!strncmp(l, "voluntary_ctxt_switches:", 24)) sum += atol(l + 24); fclose(f); }
    closedir(d); return sum;
}
int main(int argc, char **argv) {
    const char *sc = argc > 1 ? argv[1] : "main";
    fakex_title(1, "L:A_N:application_PC:N_ID:com.lab126.krpp_A:mainWindow"); fakex_title(2, "L:A_N:application_ID:com.lab126.KPPMainApp_A:KPPMainApp");
    gdk_event_handler_set(app_handler, 0, 0);
    if (!strcmp(sc, "crash")) { lp(&W_READER); printf("crash child: property %d, state file %d\n", fakex_get(), state_exists()); fflush(stdout); abort(); }
    if (!strcmp(sc, "recover")) { CHK(fakex_get() == 1, "restart after a crash with grip off: property restored at startup"); CHK(!state_exists(), "state file removed"); return fails; }
    if (!strcmp(sc, "recover-ext")) { CHK(fakex_get() == 2 && !state_exists(), "restart after a crash, value already rewritten by someone else: left alone, state file removed"); return fails; }
    if (!strcmp(sc, "exit")) { lp(&W_READER); return fakex_get() == 0 ? 0 : 9; }      /* normal exit with the finger down: destructor restores */
    if (!strcmp(sc, "off") || !strcmp(sc, "other")) {
        lp(&W_READER); CHK(fakex_get() == 1 && fakex_opens() == 0, "switch off / other program: pass-through, no X connection"); up(&W_READER); CHK(seen == 4, "events still reach the app"); return fails; }
    if (!strcmp(sc, "nogate")) { lp(&W_LIB); CHK(fakex_get() == 0, "KPPGRIP_GATE=0: long-press in the Library switches grip off (v1 behaviour)"); up(&W_LIB); CHK(fakex_get() == 1, "restored"); return fails; }
    if (!strcmp(sc, "xev")) {
        lp(&W_READER); CHK(fakex_get() == 0 && fakex_selected(), "xev: grip off, property events selected"); up(&W_READER);
        CHK(fakex_get() == 1 && !fakex_selected(), "xev: plain gesture restores and deselects");
        lp(&W_READER); fakex_set_external(0); up(&W_READER); CHK(fakex_get() == 0, "xev: someone else wrote 0 during the gesture -> left at 0");
        fakex_set_external(1); fakex_echo(2);
        lp(&W_READER); up(&W_READER); CHK(fakex_get() == 1, "xev: a driver that echoes extra events per write does not look like a foreign writer");
        return fails; }

    /* ---- main scenario ---- */
    int n0 = seen;
    other(3 /* motion */, 9); other(8, 9); other(4, 3); up(&W_READER);
    CHK(fakex_opens() == 0 && fakex_get() == 1, "non-button events with 9 at offset 40, other buttons, lifts without a gesture: nothing, no X connection");
    lp(&W_LIB); CHK(fakex_get() == 1 && !state_exists(), "long-press in the Library window: grip untouched"); up(&W_LIB);
    lp(&W_NONE); CHK(fakex_get() == 1, "long-press in a window without a title: grip untouched"); up(&W_NONE);
    fakex_fail_title = 1; lp(&W_READER); CHK(fakex_get() == 1, "title unreadable: grip untouched"); up(&W_READER); fakex_fail_title = 0;
    int w0 = fakex_writes();
    lp(&W_READER); CHK(fakex_get() == 0 && state_exists(), "long-press in the reader: grip off, state file written");
    lp(&W_READER); CHK(fakex_writes() == w0 + 1, "second long-press while off: no second write");
    up(&W_READER); CHK(fakex_get() == 1 && !state_exists(), "finger lift: restored, state file removed");
    CHK(fakex_writes() == w0 + 2, "exactly two property writes per gesture");
    up(&W_READER); CHK(fakex_writes() == w0 + 2, "another lift: no write");
    /* external writer during the gesture */
    lp(&W_READER); fakex_set_external(1); up(&W_READER); CHK(fakex_get() == 1 && fakex_writes() == w0 + 3, "winmgr re-enabled grip during the gesture: its value stays, no write from us");
    fakex_set_external(2); lp(&W_READER); CHK(fakex_get() == 0, "saved value 2"); fakex_set_external(1); up(&W_READER); CHK(fakex_get() == 1, "winmgr's newer 1 is not overwritten by our saved 2");
    fakex_set_external(0); int w1 = fakex_writes(); lp(&W_READER); up(&W_READER); CHK(fakex_get() == 0 && fakex_writes() == w1 && !state_exists(), "grip already 0 (winmgr): nothing written at all");
    fakex_set_external(1);
    /* watchdog */
    lp(&W_READER); CHK(fakex_get() == 0, "gesture without a lift"); usleep(600000); CHK(fakex_get() == 0, "still off before the deadline (max 1000 ms)");
    usleep(700000); CHK(fakex_get() == 1 && !state_exists(), "deadline: restored by the helper thread");
    up(&W_READER); CHK(fakex_get() == 1, "late lift after the timeout: nothing");
    /* focus-out shortens the deadline */
    lp(&W_READER); focus(1); usleep(250000); CHK(fakex_get() == 0, "focus-in does nothing"); focus(0); usleep(100000); CHK(fakex_get() == 0, "focus-out: not restored at once");
    usleep(300000); CHK(fakex_get() == 1, "focus-out: restored after KPPGRIP_FOCUS_MS (200)");
    /* idle: the helper thread exists and does not wake up */
    int nt; long c0 = ctxsw_others(&nt); usleep(1600000); long c1 = ctxsw_others(&nt);
    printf("helper threads %d, voluntary context switches while idle for 1.6 s: %ld\n", nt, c1 - c0);
    CHK(nt >= 1 && nt <= 2 /* + qemu-user's own thread in the test container */ && c1 == c0, "no wakeups while idle (v1: one every 500 ms)");
    CHK(fakex_opens() == 1, "one X connection in total");
    CHK(seen - n0 > 30, "every event was passed on to the app's handler");
    printf("FAILS: %d\n", fails); return fails;
}
