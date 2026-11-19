/* test only: the few gdk entry points libkppgrip uses. A "GdkWindow" here is a pointer to its X id. */
typedef void (*GdkEventFunc)(void *event, void *data);
static GdkEventFunc g_f; static void *g_d; int fakegdk_seen;
void gdk_event_handler_set(GdkEventFunc f, void *d, void (*n)(void *)) { g_f = f; g_d = d; }
void *gdk_window_get_toplevel(void *w) { return w; }
unsigned long gdk_x11_drawable_get_xid(void *w) { return *(unsigned long *)w; }
void gdk_error_trap_push(void) {}
int gdk_error_trap_pop(void) { return 0; }
void fakegdk_dispatch(void *ev) { if (g_f) g_f(ev, g_d); }
