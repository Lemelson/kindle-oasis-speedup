/*
 * libkppdb.so: read-path accelerators for KPPMainApp's catalog access (Kindle Oasis 3, FW 5.18.2).
 * LD_PRELOAD shim, two independent switches (both default off; unset = the original code path):
 *
 * KPPDB_STMTCACHE=1  prepared-statement reuse.  ksdk re-prepares the same SQL text for every item (CCDB Assets Query
 *   on cc.db, Nodes lookup on ksdk.asset.db, BEGIN/COMMIT inside sqlite3_exec, ...): ~0.3-1 ms of parse + codegen each.
 *   finalize() of a SELECT/BEGIN/COMMIT/END statement that completed without error resets it, clears its bindings and
 *   parks it (per connection, LRU); a later prepare_v2() of byte-identical text on the same connection gets it back.
 *   A reset statement with cleared bindings is in the same state as a fresh one; schema changes are handled by
 *   SQLite's own auto-reprepare (prepare_v2 semantics). Nothing is written anywhere; results come from SQLite itself.
 *   Parked statements of a connection are finalized before sqlite3_close/close_v2 of that connection.
 *   KPPDB_STMTCACHE_MAX=<n> total parked statements (default 48).
 *
 * KPPDB_QUIETLOG=1  drops one log message: kndk::log::log(level, "Entry2Attr<...>", "Undefined conversion"), which
 *   ksdk emits 21 times per catalog entry it converts (every item of every page: ~190 syslog lines per page turn).
 *   Only the log line is dropped (formatting, syslog() send, syslogd work); the conversion code itself is untouched.
 *
 * KPPDB_STATS=<file>  appends counters at exit-free checkpoints (every 256 prepares) for diagnosis.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <dlfcn.h>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
#define SQLITE_OK 0

static int (*r_prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
static int (*r_finalize)(sqlite3_stmt *);
static int (*r_reset)(sqlite3_stmt *);
static int (*r_clear_bindings)(sqlite3_stmt *);
static int (*r_close)(sqlite3 *);
static int (*r_close_v2)(sqlite3 *);

static int g_cache_on, g_quiet_on, g_inited;
static int g_win_on;
static int g_max = 48;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_hit, g_miss, g_park, g_evict, g_noel, g_err, g_dropped;
static const char *g_stats;

static void init(void)
{
    if (g_inited) return;
    r_prepare_v2 = dlsym(RTLD_NEXT, "sqlite3_prepare_v2");
    r_finalize = dlsym(RTLD_NEXT, "sqlite3_finalize");
    r_reset = dlsym(RTLD_NEXT, "sqlite3_reset");
    r_clear_bindings = dlsym(RTLD_NEXT, "sqlite3_clear_bindings");
    r_close = dlsym(RTLD_NEXT, "sqlite3_close");
    r_close_v2 = dlsym(RTLD_NEXT, "sqlite3_close_v2");
    const char *e = getenv("KPPDB_STMTCACHE");
    g_cache_on = e && e[0] == '1' && r_prepare_v2 && r_finalize && r_reset && r_clear_bindings && r_close;
    e = getenv("KPPDB_STMTCACHE_MAX"); if (e && atoi(e) > 0 && atoi(e) <= 512) g_max = atoi(e);
    e = getenv("KPPDB_QUIETLOG"); g_quiet_on = e && e[0] == '1';
    e = getenv("KPPDB_WINDOW"); g_win_on = e && e[0] == '1' && r_prepare_v2 && r_finalize && r_reset && r_close;
    g_stats = getenv("KPPDB_STATS");
    __sync_synchronize();
    g_inited = 1;
}
__attribute__((constructor)) static void ctor(void) { init(); }

static unsigned long g_w_rewrite, g_w_build, g_w_serve, g_w_refuse, g_w_fallback;
__attribute__((destructor)) static void stats(void)
{
    if (!g_stats) return;
    FILE *f = fopen(g_stats, "a");
    if (!f) return;
    fprintf(f, "kppdb hit=%lu miss=%lu park=%lu evict=%lu noneligible=%lu errfin=%lu logdrop=%lu win: rewrite=%lu build=%lu serve=%lu refuse=%lu fallback=%lu\n",
            g_hit, g_miss, g_park, g_evict, g_noel, g_err, g_dropped, g_w_rewrite, g_w_build, g_w_serve, g_w_refuse, g_w_fallback);
    fclose(f);
}

/* ---------------- statement cache ---------------- */
typedef struct {
    sqlite3 *db; sqlite3_stmt *st; char *sql; int len; int tailoff; unsigned h; unsigned long use; unsigned gen;
} Ent;

/* parked statements (free to hand out) */
#define PMAX 512
static Ent g_park_tab[PMAX];
static int g_npark;
static unsigned long g_clock;

/* live statements that came from an eligible prepare (key kept until finalize) */
#define LMAX 1024
static Ent g_live[LMAX];

static unsigned hsh(const char *s, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h ^ (unsigned)n;
}

static int kw(const char *p, const char *e, const char *w)
{
    int n = strlen(w);
    if (e - p < n) return 0;
    for (int i = 0; i < n; i++) { char c = p[i]; if (c >= 'a' && c <= 'z') c -= 32; if (c != w[i]) return 0; }
    if (e - p == n) return 1;
    char c = p[n];
    return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80);
}

/* first statement keyword after whitespace and comments: SELECT, BEGIN, COMMIT or END */
static int eligible(const char *s, int n)
{
    const char *p = s, *e = s + n;
    for (;;) {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')) p++;
        if (e - p >= 2 && p[0] == '-' && p[1] == '-') { while (p < e && *p != '\n') p++; continue; }
        if (e - p >= 2 && p[0] == '/' && p[1] == '*') {
            const char *q = p + 2; while (q + 1 < e && !(q[0] == '*' && q[1] == '/')) q++;
            if (q + 1 >= e) return 0; p = q + 2; continue;
        }
        break;
    }
    return kw(p, e, "SELECT") || kw(p, e, "BEGIN") || kw(p, e, "COMMIT") || kw(p, e, "END");
}

static Ent *live_find(sqlite3_stmt *st, int create)
{
    unsigned h = ((uintptr_t)st >> 3) % LMAX;
    for (int i = 0; i < LMAX; i++) {
        Ent *x = &g_live[(h + i) % LMAX];
        if (x->st == st) return x;
        if (!x->st) { if (!create) return 0; memset(x, 0, sizeof *x); x->st = st; return x; }
    }
    return 0;
}

static void live_del(Ent *x)
{
    unsigned i = x - g_live; x->st = 0;
    for (unsigned j = (i + 1) % LMAX; g_live[j].st; j = (j + 1) % LMAX) {
        Ent t = g_live[j]; g_live[j].st = 0; Ent *n = live_find(t.st, 1); *n = t;
    }
}

/* sqlite3_exec() prepares through the PLT but finalizes internally (sqlite3VdbeFinalize), so statements prepared
 * inside exec are never tracked or handed out. Any statement pointer returned by any prepare call clears a stale live
 * entry for the same address (defence in depth against a statement freed behind our back). */
static __thread int t_in_exec;

static void clear_stale(sqlite3_stmt *st)
{
    if (!st) return;
    pthread_mutex_lock(&g_mu);
    Ent *l = live_find(st, 0);
    if (l) { char *s = l->sql; live_del(l); free(s); }
    pthread_mutex_unlock(&g_mu);
}

/* per-connection schema generation: bumped whenever SQLite (re)loads the connection's in-memory schema or a
 * collation/function is (re)registered. A statement is parked only if no bump happened since just before its prepare,
 * and handed out only while that still holds. */
#define GMAX 64
static struct { sqlite3 *db; unsigned gen, wgen; } g_gen[GMAX];   /* wgen: window cache generation (see mark_win) */

static unsigned *gen_slot(sqlite3 *db)      /* caller holds g_mu */
{
    int fr = -1;
    for (int i = 0; i < GMAX; i++) { if (g_gen[i].db == db) return &g_gen[i].gen; if (!g_gen[i].db && fr < 0) fr = i; }
    if (fr < 0) return 0;
    g_gen[fr].db = db; g_gen[fr].gen = 1; g_gen[fr].wgen = 1;
    return &g_gen[fr].gen;
}

static unsigned gen_get(sqlite3 *db)
{
    pthread_mutex_lock(&g_mu);
    unsigned *g = gen_slot(db); unsigned v = g ? *g : 0;   /* 0 = untracked: never parked */
    pthread_mutex_unlock(&g_mu);
    return v;
}

static void mark_dead(sqlite3 *db)
{
    pthread_mutex_lock(&g_mu);
    unsigned *g = gen_slot(db); if (g) { (*g)++; if (!*g) *g = 1; }
    pthread_mutex_unlock(&g_mu);
}

/* window cache generation: schema (re)loads, collation (re)registration (ORDER BY), and (re)registration of a
 * function whose name the window gate allows in a WHERE (fn_ok). KPP re-registers its own search function
 * "MatchRank" before every page query: that expires statements (handled by SQLite's reprepare and by mark_dead for
 * the statement cache) but cannot change the rows of a WHERE that does not call it. */
static unsigned *wgen_slot(sqlite3 *db)     /* caller holds g_mu */
{
    unsigned *g = gen_slot(db);
    return g ? g + 1 : 0;
}
static void mark_win(sqlite3 *db)
{
    pthread_mutex_lock(&g_mu);
    unsigned *g = wgen_slot(db); if (g) { (*g)++; if (!*g) *g = 1; }
    pthread_mutex_unlock(&g_mu);
}

/* finalize parked statements of db from an older generation; only called from the application's own prepare_v2 */
static void reap_dead(sqlite3 *db)
{
    sqlite3_stmt *fin[PMAX]; char *sq[PMAX]; int nf = 0;
    pthread_mutex_lock(&g_mu);
    unsigned *g = gen_slot(db); unsigned cur = g ? *g : 0;
    for (int i = 0; i < g_npark;) {
        if (g_park_tab[i].db == db && g_park_tab[i].gen != cur) { fin[nf] = g_park_tab[i].st; sq[nf++] = g_park_tab[i].sql; g_park_tab[i] = g_park_tab[--g_npark]; }
        else i++;
    }
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < nf; i++) { r_finalize(fin[i]); free(sq[i]); }
}

#include "kppdb_win.c"

int sqlite3_exec(sqlite3 *db, const char *sql, int (*cb)(void *, int, char **, char **), void *a, char **err)
{
    static int (*r)(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
    if (!r) r = dlsym(RTLD_NEXT, "sqlite3_exec");
    /* SQLite (re)loads a connection's in-memory schema with sqlite3_exec("SELECT*FROM\"main\".sqlite_master ORDER BY
       rowid") (sqlite3InitOne). Statements parked before that were compiled against the previous schema: a fresh
       prepare would not be, so they must not be handed out again. */
    if ((g_cache_on || g_win_on) && db && sql && !strncmp(sql, "SELECT*FROM\"", 12) && strstr(sql, " ORDER BY rowid")) {
        mark_dead(db); mark_win(db);
    }
    t_in_exec++;
    int rc = r(db, sql, cb, a, err);
    t_in_exec--;
    return rc;
}

#define PASS_PREP(name, ctype) \
int name(sqlite3 *db, const ctype *sql, int n, sqlite3_stmt **pst, const ctype **tail) \
{ \
    static int (*r)(sqlite3 *, const ctype *, int, sqlite3_stmt **, const ctype **); \
    if (!r) r = dlsym(RTLD_NEXT, #name); \
    int rc = r(db, sql, n, pst, tail); \
    if (g_cache_on && pst) clear_stale(*pst); \
    return rc; \
}
PASS_PREP(sqlite3_prepare, char)
PASS_PREP(sqlite3_prepare16, void)
PASS_PREP(sqlite3_prepare16_v2, void)

#define PASS_PREP3(name, ctype) \
int name(sqlite3 *db, const ctype *sql, int n, unsigned f, sqlite3_stmt **pst, const ctype **tail) \
{ \
    static int (*r)(sqlite3 *, const ctype *, int, unsigned, sqlite3_stmt **, const ctype **); \
    if (!r) r = dlsym(RTLD_NEXT, #name); \
    int rc = r(db, sql, n, f, pst, tail); \
    if (g_cache_on && pst) clear_stale(*pst); \
    return rc; \
}
PASS_PREP3(sqlite3_prepare_v3, char)
PASS_PREP3(sqlite3_prepare16_v3, void)

static int park_find(sqlite3 *db, unsigned h, const char *sql, int n)     /* caller holds g_mu */
{
    unsigned *g = gen_slot(db); unsigned cur = g ? *g : 0;
    for (int i = 0; i < g_npark; i++) {
        Ent *x = &g_park_tab[i];
        if (x->db == db && x->gen == cur && cur && x->h == h && x->len == n && !memcmp(x->sql, sql, n)) return i;
    }
    return -1;
}

static int park_has(sqlite3 *db, unsigned h, const char *sql, int n)
{
    pthread_mutex_lock(&g_mu);
    int i = park_find(db, h, sql, n);
    pthread_mutex_unlock(&g_mu);
    return i >= 0;
}

/* remove a parked statement and register it as live again */
static sqlite3_stmt *park_take(sqlite3 *db, unsigned h, const char *sql, int n, int *toff)
{
    sqlite3_stmt *got = 0;
    pthread_mutex_lock(&g_mu);
    int i = park_find(db, h, sql, n);
    if (i >= 0) {
        Ent x = g_park_tab[i];
        g_park_tab[i] = g_park_tab[--g_npark];
        got = x.st; *toff = x.tailoff;
        Ent *l = live_find(got, 1);
        if (l) *l = x; else free(x.sql);
        g_hit++;
    }
    pthread_mutex_unlock(&g_mu);
    return got;
}

int sqlite3_prepare_v2(sqlite3 *db, const char *sql, int nbyte, sqlite3_stmt **pst, const char **tail)
{
    if (!g_inited) init();
    if (g_win_on && db && sql && pst && !t_in_exec) {
        int wrc;
        if (win_try(db, sql, nbyte, pst, tail, &wrc)) { if (g_cache_on) clear_stale(*pst); return wrc; }
    }
    if (!g_cache_on || !db || !sql || !pst) return r_prepare_v2(db, sql, nbyte, pst, tail);
    int n = nbyte < 0 ? (int)strlen(sql) : (int)strnlen(sql, nbyte);
    if (t_in_exec || !eligible(sql, n)) {
        g_noel++;
        int rc = r_prepare_v2(db, sql, nbyte, pst, tail);
        clear_stale(*pst);
        return rc;
    }
    /* A fresh prepare would (re)load the connection's in-memory schema if an earlier statement found it stale and
       reset it, and it leaves the connection's error state at SQLITE_OK. Preparing a tiny statement that names
       sqlite_master does both (a reload goes through sqlite3_exec -> mark_dead() above) for ~1/20 of the cost. */
    reap_dead(db);
    unsigned h = hsh(sql, n);
    sqlite3_stmt *got = 0; int toff = 0;
    if (park_has(db, h, sql, n)) {
        static const char probe[] = "SELECT 0 FROM sqlite_master LIMIT 0";
        sqlite3_stmt *dummy = 0;
        int prc = r_prepare_v2(db, probe, sizeof probe, &dummy, 0);
        if (dummy) r_finalize(dummy);
        reap_dead(db);
        if (prc == SQLITE_OK) got = park_take(db, h, sql, n, &toff);
    }
    if (got) {
        *pst = got;
        if (tail) *tail = sql + toff;
        if (g_stats && (g_hit & 31) == 0) stats();
        return SQLITE_OK;
    }
    const char *t = 0;
    unsigned g0 = gen_get(db);
    int rc = r_prepare_v2(db, sql, nbyte, pst, &t);
    if (tail) *tail = t;
    if (rc == SQLITE_OK && *pst) {
        int off = t ? (int)(t - sql) : -1;
        char *cp = malloc(n + 1);
        if (cp) {
            memcpy(cp, sql, n); cp[n] = 0;
            pthread_mutex_lock(&g_mu);
            Ent *l = live_find(*pst, 1);
            if (l) { free(l->sql); l->db = db; l->sql = cp; l->len = n; l->h = h; l->tailoff = off; l->gen = g0; cp = 0; }
            g_miss++;
            pthread_mutex_unlock(&g_mu);
            free(cp);
        }
    }
    return rc;
}

/* finalize: park eligible statements instead of destroying them */
int sqlite3_finalize(sqlite3_stmt *st)
{
    if (!g_inited) init();
    if (!g_cache_on || !st) return r_finalize(st);
    pthread_mutex_lock(&g_mu);
    Ent *l = live_find(st, 0);
    Ent x; int have = 0;
    if (l) { x = *l; live_del(l); have = 1; }
    pthread_mutex_unlock(&g_mu);
    if (!have) return r_finalize(st);
    if (x.tailoff < 0) { free(x.sql); return r_finalize(st); }
    int rc = r_reset(st);           /* same return code and connection error state as finalize would produce */
    if (rc != SQLITE_OK) { g_err++; free(x.sql); r_finalize(st); return rc; }
    r_clear_bindings(st);
    sqlite3_stmt *evict = 0; char *evsql = 0;
    pthread_mutex_lock(&g_mu);
    x.use = ++g_clock;
    { unsigned *g = gen_slot(x.db); if (!g || !x.gen || *g != x.gen) { pthread_mutex_unlock(&g_mu); free(x.sql); r_finalize(st); return rc; } }
    if (g_npark >= g_max || g_npark >= PMAX) {
        int lo = 0;
        for (int i = 1; i < g_npark; i++) if (g_park_tab[i].use < g_park_tab[lo].use) lo = i;
        evict = g_park_tab[lo].st; evsql = g_park_tab[lo].sql;
        g_park_tab[lo] = x; g_evict++;
    } else g_park_tab[g_npark++] = x;
    g_park++;
    pthread_mutex_unlock(&g_mu);
    if (evict) { r_finalize(evict); free(evsql); }
    return rc;
}

static void drop_db(sqlite3 *db)
{
    sqlite3_stmt *fin[PMAX]; char *s[PMAX]; int nf = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npark;) {
        if (g_park_tab[i].db == db) { fin[nf] = g_park_tab[i].st; s[nf++] = g_park_tab[i].sql; g_park_tab[i] = g_park_tab[--g_npark]; }
        else i++;
    }
    /* live statements of this connection: never park them after close (close_v2 zombie case) */
    for (int i = 0; i < LMAX; i++) if (g_live[i].st && g_live[i].db == db) g_live[i].tailoff = -1;
    for (int i = 0; i < GMAX; i++) if (g_gen[i].db == db) { g_gen[i].db = 0; g_gen[i].gen = 0; g_gen[i].wgen = 0; }
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < nf; i++) { r_finalize(fin[i]); free(s[i]); }
}

int sqlite3_close(sqlite3 *db)
{
    if (!g_inited) init();
    if (g_cache_on && db) drop_db(db);
    if (!g_win_on || !db) return r_close(db);
    win_drop_stmts(db);
    int rc = r_close(db);
    if (rc == SQLITE_OK) win_forget(db, 1);
    return rc;
}

int sqlite3_close_v2(sqlite3 *db)
{
    if (!g_inited) init();
    if (g_cache_on && db) drop_db(db);
    if (!g_win_on || !db) return r_close_v2(db);
    win_drop_stmts(db);
    int rc = r_close_v2(db);
    win_forget(db, 0);
    return rc;
}


/* (re)registering a collation or function expires the connection's statements: statement-cache generation.
   Collations also change ORDER BY results; functions only if the window gate lets them appear in a WHERE. */
static int fn_in_whitelist(const char *n)
{
    if (!n) return 1;
    for (int k = 0; fn_ok[k]; k++) if (ieq(n, strlen(n), fn_ok[k])) return 1;
    return 0;
}
#define MARK_ON(name, proto, args, iscoll) \
int name proto \
{ \
    static int (*r) proto; \
    if (!r) r = dlsym(RTLD_NEXT, #name); \
    if ((g_cache_on || g_win_on) && db) { mark_dead(db); if (iscoll || fn_in_whitelist(n)) mark_win(db); } \
    return r args; \
}
MARK_ON(sqlite3_create_collation, (sqlite3 *db, const char *n, int e, void *a, int (*c)(void *, int, const void *, int, const void *)), (db, n, e, a, c), 1)
MARK_ON(sqlite3_create_collation_v2, (sqlite3 *db, const char *n, int e, void *a, int (*c)(void *, int, const void *, int, const void *), void (*d)(void *)), (db, n, e, a, c, d), 1)
MARK_ON(sqlite3_create_function, (sqlite3 *db, const char *n, int na, int e, void *a, void (*f)(void *, int, void **), void (*st)(void *, int, void **), void (*fi)(void *)), (db, n, na, e, a, f, st, fi), 0)
MARK_ON(sqlite3_create_function_v2, (sqlite3 *db, const char *n, int na, int e, void *a, void (*f)(void *, int, void **), void (*st)(void *, int, void **), void (*fi)(void *), void (*d)(void *)), (db, n, na, e, a, f, st, fi, d), 0)

/* ---------------- log filter ---------------- */
/* kndk::log::log(LogLevel, const char *tag, const char *fmt, ...) is variadic: an ARM-mode trampoline asks
 * kppdb_log_decide() and either returns (drop) or tail-calls the real function with r0-r3, lr and the stack untouched. */
static void *g_real_log;

__attribute__((used, visibility("hidden"))) void *kppdb_log_decide(int level, const char *tag, const char *fmt)
{
    if (!g_real_log) {
        g_real_log = dlsym(RTLD_NEXT, "_ZN4kndk3log3logENS0_8LogLevelEPKcS3_z");
        if (!g_inited) init();
    }
    if (g_quiet_on && level == 3 && fmt && tag && fmt[0] == 'U' && tag[0] == 'E' &&
        !strcmp(fmt, "Undefined conversion") && !strncmp(tag, "Entry2Attr<", 11)) {
        g_dropped++;
        return 0;
    }
    return g_real_log;
}

void kppdb_log_tramp(void) __asm__("_ZN4kndk3log3logENS0_8LogLevelEPKcS3_z");
__attribute__((naked, target("arm"))) void kppdb_log_tramp(void)
{
    __asm__ volatile(
        "push {r0, r1, r2, r3, r4, lr}\n\t"
        "bl kppdb_log_decide\n\t"
        "mov r12, r0\n\t"
        "pop {r0, r1, r2, r3, r4, lr}\n\t"
        "cmp r12, #0\n\t"
        "bxeq lr\n\t"
        "bx r12\n\t");
}
