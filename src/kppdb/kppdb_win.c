/*
 * kppdb_win.c (included by kppdb.c): KPPDB_WINDOW=1, the Library page query without the OFFSET walk.
 *
 * KPP asks cc.db for one page at a time with literal paging:
 *     SELECT <plain Entries columns> FROM Entries <WHERE ... ORDER BY ...> LIMIT n [OFFSET m]
 * SQLite answers by walking the sort index from the start and filtering every row up to m + n, and the statement text
 * (31 columns, a big WHERE with correlated subqueries) costs ~2.5 ms to prepare each time.
 *
 * At prepare time such a statement (cc.db connection, autocommit, no parameters, only plain identifiers in the select
 * list, FROM exactly "Entries", no GROUP BY/HAVING/compound/window, only whitelisted functions, ORDER BY present, and
 * an EXPLAIN QUERY PLAN identical to that of "SELECT Entries.rowid FROM Entries <same tail>" without TEMP B-TREE) is
 * replaced by
 *     SELECT <same select list> FROM kppdb_win(<slot>,<serial>,m,n) AS kppdb_w
 *            CROSS JOIN Entries ON Entries.rowid = kppdb_w.kppdb_rid ORDER BY kppdb_w.kppdb_pos
 * kppdb_win is an eponymous virtual table registered on that connection. Its xFilter runs INSIDE the read transaction
 * of the statement being stepped: it reads PRAGMA data_version (+ the connection's own total_changes, the schema
 * generation and the autocommit state) and, if anything differs from when the cached ordered rowid list was built,
 * rebuilds the list right there with "SELECT Entries.rowid FROM Entries <tail>" (same plan, same snapshot). It then
 * emits rowids m .. m+n-1 of the list; the join fetches those rows by rowid. Same snapshot, same rows, same order,
 * same column expressions and names as the original statement.
 * Anything unexpected at prepare time -> the original SQL is prepared unchanged.
 */

typedef long long i64;
typedef struct sqlite3_vtab { const void *pModule; int nRef; char *zErrMsg; } sqlite3_vtab;
typedef struct sqlite3_vtab_cursor { sqlite3_vtab *pVtab; } sqlite3_vtab_cursor;
typedef struct sqlite3_context sqlite3_context;
typedef struct sqlite3_value sqlite3_value;
struct ic { int iColumn; unsigned char op; unsigned char usable; int iTermOffset; };
struct iob { int iColumn; unsigned char desc; };
struct icu_ { int argvIndex; unsigned char omit; };
typedef struct sqlite3_index_info {
    int nConstraint; struct ic *aConstraint; int nOrderBy; struct iob *aOrderBy; struct icu_ *aConstraintUsage;
    int idxNum; char *idxStr; int needToFreeIdxStr; int orderByConsumed; double estimatedCost;
    i64 estimatedRows; int idxFlags; unsigned long long colUsed;
} sqlite3_index_info;
typedef struct sqlite3_module {
    int iVersion;
    int (*xCreate)(sqlite3 *, void *, int, const char *const *, sqlite3_vtab **, char **);
    int (*xConnect)(sqlite3 *, void *, int, const char *const *, sqlite3_vtab **, char **);
    int (*xBestIndex)(sqlite3_vtab *, sqlite3_index_info *);
    int (*xDisconnect)(sqlite3_vtab *);
    int (*xDestroy)(sqlite3_vtab *);
    int (*xOpen)(sqlite3_vtab *, sqlite3_vtab_cursor **);
    int (*xClose)(sqlite3_vtab_cursor *);
    int (*xFilter)(sqlite3_vtab_cursor *, int, const char *, int, sqlite3_value **);
    int (*xNext)(sqlite3_vtab_cursor *);
    int (*xEof)(sqlite3_vtab_cursor *);
    int (*xColumn)(sqlite3_vtab_cursor *, sqlite3_context *, int);
    int (*xRowid)(sqlite3_vtab_cursor *, i64 *);
    void *xUpdate, *xBegin, *xSync, *xCommit, *xRollback, *xFindFunction, *xRename;
} sqlite3_module;
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_ERROR 1
#define SQLITE_NOMEM 7
#define SQLITE_CONSTRAINT 19
#define SQLITE_INDEX_CONSTRAINT_EQ 2

int sqlite3_step(sqlite3_stmt *);
i64 sqlite3_column_int64(sqlite3_stmt *, int);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
int sqlite3_total_changes(sqlite3 *);
int sqlite3_get_autocommit(sqlite3 *);
const char *sqlite3_db_filename(sqlite3 *, const char *);
int sqlite3_declare_vtab(sqlite3 *, const char *);
int sqlite3_create_module_v2(sqlite3 *, const char *, const sqlite3_module *, void *, void (*)(void *));
i64 sqlite3_value_int64(sqlite3_value *);
void sqlite3_result_int64(sqlite3_context *, i64);
void sqlite3_result_null(sqlite3_context *);
void *sqlite3_malloc(int);
void sqlite3_free(void *);
int sqlite3_bind_parameter_count(sqlite3_stmt *);


/* ---- per connection ---- */
#define WDB 32
static struct wdb { sqlite3 *db; int state; /* 0 unknown, 1 cc.db + module ok, 2 not eligible */ sqlite3_stmt *dv; } g_wdb[WDB];

/* ---- per (connection, tail) entry; never freed while the connection lives (statements refer to the slot) ---- */
#define WMAX 64
typedef struct {
    sqlite3 *db; char *tail; int tlen; unsigned h; unsigned serial;
    int state;              /* 1 = rewrite allowed, 2 = refused (plan gate) */
    unsigned gen; int built; i64 dv; int tc;
    i64 *rows; int nrows, cap;
} WEnt;
static WEnt g_went[WMAX];
static unsigned g_serial;

static struct wdb *wdb_get(sqlite3 *db, int create)     /* caller holds g_mu */
{
    int fr = -1;
    for (int i = 0; i < WDB; i++) { if (g_wdb[i].db == db) return &g_wdb[i]; if (!g_wdb[i].db && fr < 0) fr = i; }
    if (!create || fr < 0) return 0;
    memset(&g_wdb[fr], 0, sizeof g_wdb[fr]); g_wdb[fr].db = db;
    return &g_wdb[fr];
}

/* ---------- SQL shape check ---------- */
static int idch(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }
static int sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int ieq(const char *a, int n, const char *w)
{
    if ((int)strlen(w) != n) return 0;
    for (int i = 0; i < n; i++) { char c = a[i]; if (c >= 'a' && c <= 'z') c -= 32; if (c != w[i]) return 0; }
    return 1;
}

static const char *const fn_ok[] = { "IN", "EXISTS", "AND", "OR", "NOT", "WHERE", "THEN", "ELSE", "WHEN", "CASE", "END",
    "IS", "NULL", "ON", "SELECT", "FROM", "BY", "ASC", "DESC", "LIKE", "GLOB", "BETWEEN", "COLLATE", "ICU",
    "IFNULL", "COALESCE", "NULLIF", "LOWER", "UPPER", "LENGTH", "SUBSTR", "INSTR", "ABS", "TRIM", "LTRIM", "RTRIM",
    "REPLACE", "TYPEOF", "MIN", "MAX", 0 };
static const char *const kw_bad[] = { "GROUP", "HAVING", "UNION", "INTERSECT", "EXCEPT", "WINDOW", "OVER", "JOIN",
    "LIMIT", "OFFSET", "RANDOM", "RANDOMBLOB", "CHANGES", "TOTAL_CHANGES", "LAST_INSERT_ROWID", "DATE", "TIME",
    "DATETIME", "JULIANDAY", "STRFTIME", "UNIXEPOCH", "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP", "ROWID",
    "OID", "_ROWID_", "KPPDB_WIN", "KPPDB_W", "MATCH", "REGEXP", "PRAGMA", 0 };

/* Lexical scan of the tail: strings/identifiers in quotes skipped; rejects parameters, bad keywords, any
 * identifier followed by '(' that is not whitelisted, ';' and comments. Requires a top-level-ish ORDER BY. */
static int tail_ok(const char *p, int n)
{
    int order = 0;
    for (int i = 0; i < n;) {
        char c = p[i];
        if (c == '\'' || c == '"' || c == '`' || c == '[') {
            char q = c == '[' ? ']' : c; i++;
            while (i < n) { if (p[i] == q) { if (q != ']' && i + 1 < n && p[i + 1] == q) { i += 2; continue; } break; } i++; }
            if (i >= n) return 0;
            i++; continue;
        }
        if (c == '?' || c == ':' || c == '@' || c == '$' || c == ';') return 0;
        if (c == '-' && i + 1 < n && p[i + 1] == '-') return 0;
        if (c == '/' && i + 1 < n && p[i + 1] == '*') return 0;
        if (idch(c)) {
            int s = i; while (i < n && idch(p[i])) i++;
            int len = i - s;
            if (c >= '0' && c <= '9') continue;
            for (int k = 0; kw_bad[k]; k++) if (ieq(p + s, len, kw_bad[k])) return 0;
            if (ieq(p + s, len, "ORDER")) { int j = i; while (j < n && sp(p[j])) j++; if (j + 2 <= n && ieq(p + j, 2, "BY")) order = 1; }
            int j = i; while (j < n && sp(p[j])) j++;
            if (j < n && p[j] == '(') {
                int ok = 0;
                for (int k = 0; fn_ok[k]; k++) if (ieq(p + s, len, fn_ok[k])) { ok = 1; break; }
                if (!ok) return 0;
            }
            continue;
        }
        i++;
    }
    return order;
}

/* SELECT <idents> FROM Entries <tail> LIMIT n [OFFSET m] -> offsets of select list and tail, n, m */
static int win_match(const char *s, int n, int *sl0, int *sl1, int *t0, int *t1, long *lim, long *off)
{
    int i = 0;
    while (i < n && sp(s[i])) i++;
    if (n - i < 7 || !ieq(s + i, 6, "SELECT") || !sp(s[i + 6])) return 0;
    i += 6; while (i < n && sp(s[i])) i++;
    *sl0 = i;
    /* select list: identifiers separated by commas */
    int items = 0;
    for (;;) {
        int b = i; while (i < n && idch(s[i])) i++;
        if (i == b) return 0;
        if (ieq(s + b, i - b, "DISTINCT") || ieq(s + b, i - b, "ALL") || ieq(s + b, i - b, "FROM") ||
            ieq(s + b, i - b, "ROWID") || ieq(s + b, i - b, "OID") || ieq(s + b, i - b, "_ROWID_") ||
            !strncmp(s + b, "kppdb", 5)) return 0;
        items++;
        int e = i; while (i < n && sp(s[i])) i++;
        if (i < n && s[i] == ',') { i++; while (i < n && sp(s[i])) i++; continue; }
        *sl1 = e;
        break;
    }
    if (items < 1 || i >= n || !ieq(s + i, 4, "FROM") || !sp(s[i + 4])) return 0;
    i += 4; while (i < n && sp(s[i])) i++;
    if (n - i < 7 || strncmp(s + i, "Entries", 7) || (i + 7 < n && !sp(s[i + 7]))) return 0;
    i += 7;
    *t0 = i;
    /* trailing LIMIT n [OFFSET m] */
    int e = n; while (e > i && sp(s[e - 1])) e--;
    int d = e; while (d > i && s[d - 1] >= '0' && s[d - 1] <= '9') d--;
    if (d == e || e - d > 9) return 0;
    long v1 = atol(s + d);
    int k = d; while (k > i && sp(s[k - 1])) k--;
    if (k - 6 >= i && ieq(s + k - 6, 6, "OFFSET") && sp(s[k - 7])) {
        *off = v1;
        int e2 = k - 6; while (e2 > i && sp(s[e2 - 1])) e2--;
        int d2 = e2; while (d2 > i && s[d2 - 1] >= '0' && s[d2 - 1] <= '9') d2--;
        if (d2 == e2 || e2 - d2 > 9) return 0;
        *lim = atol(s + d2);
        k = d2; while (k > i && sp(s[k - 1])) k--;
    } else { *off = 0; *lim = v1; }
    if (k - 5 < i || !ieq(s + k - 5, 5, "LIMIT") || !sp(s[k - 6])) return 0;
    *t1 = k - 5;
    if (*t1 <= *t0 || *lim < 0 || *off < 0) return 0;
    { int j = *t0; while (j < *t1 && sp(s[j])) j++;
      if (!(*t1 - j >= 5 && ieq(s + j, 5, "WHERE") && !idch(s[j + 5])) && !(*t1 - j >= 5 && ieq(s + j, 5, "ORDER") && !idch(s[j + 5]))) return 0; }
    return tail_ok(s + *t0, *t1 - *t0);
}

/* ---------- nested SQL helpers (run on the app's connection, bypassing our own hooks) ---------- */
static int eqp(sqlite3 *db, const char *sql, char *out, int cap)
{
    sqlite3_stmt *st = 0; int len = 0;
    char *q = malloc(strlen(sql) + 32); if (!q) return -1;
    strcpy(q, "EXPLAIN QUERY PLAN "); strcat(q, sql);
    int rc = r_prepare_v2(db, q, -1, &st, 0); free(q);
    if (rc != SQLITE_OK || !st) return -1;
    out[0] = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *d = (const char *)sqlite3_column_text(st, 3);
        int l = d ? strlen(d) : 0;
        if (len + l + 2 >= cap) { r_finalize(st); return -1; }
        memcpy(out + len, d, l); len += l; out[len++] = '\n'; out[len] = 0;
    }
    r_finalize(st);
    return rc == SQLITE_DONE ? len : -1;
}

static char *builder_sql(const char *tail, int tlen)
{
    char *b = malloc(tlen + 64); if (!b) return 0;
    memcpy(b, "SELECT Entries.rowid FROM Entries", 33); memcpy(b + 33, tail, tlen); b[33 + tlen] = 0;
    return b;
}

/* plan gate: identical plans, no temp b-tree for ordering */
static int plan_gate(sqlite3 *db, const char *orig, const char *tail, int tlen)
{
    char a[2048], b[2048];
    char *bs = builder_sql(tail, tlen); if (!bs) return 0;
    int la = eqp(db, orig, a, sizeof a), lb = eqp(db, bs, b, sizeof b);
    free(bs);
    if (la <= 0 || lb <= 0 || la != lb || memcmp(a, b, la)) return 0;
    if (strstr(a, "TEMP B-TREE") || strstr(a, "MULTI-INDEX") || strstr(a, "AUTOMATIC")) return 0;
    if (strncmp(a, "SCAN Entries USING INDEX ", 25) && strncmp(a, "SEARCH Entries USING INDEX ", 27)) return 0;
    if (strstr(a, "COVERING INDEX")) return 0;
    return 1;
}

static int read_dv(struct wdb *w, sqlite3 *db, i64 *v)
{
    if (!w->dv && r_prepare_v2(db, "PRAGMA data_version", -1, &w->dv, 0) != SQLITE_OK) { w->dv = 0; return -1; }
    int rc = sqlite3_step(w->dv);
    if (rc == SQLITE_ROW) *v = sqlite3_column_int64(w->dv, 0);
    r_reset(w->dv);
    return rc == SQLITE_ROW ? 0 : -1;
}

/* ---------- virtual table ---------- */
typedef struct { sqlite3_vtab base; sqlite3 *db; } WVtab;
typedef struct { sqlite3_vtab_cursor base; const i64 *rows; i64 *own; int i, end; } WCur;

static int w_connect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **pp, char **err)
{
    int rc = sqlite3_declare_vtab(db, "CREATE TABLE x(kppdb_rid INTEGER, kppdb_pos INTEGER, kppdb_k HIDDEN, kppdb_s HIDDEN, kppdb_o HIDDEN, kppdb_l HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    WVtab *v = sqlite3_malloc(sizeof *v); if (!v) return SQLITE_NOMEM;
    memset(v, 0, sizeof *v); v->db = db; *pp = &v->base;
    return SQLITE_OK;
}
static int w_disconnect(sqlite3_vtab *v) { sqlite3_free(v); return SQLITE_OK; }

static int w_best(sqlite3_vtab *tab, sqlite3_index_info *ii)
{
    int idx[4] = { -1, -1, -1, -1 };
    for (int i = 0; i < ii->nConstraint; i++) {
        struct ic *c = &ii->aConstraint[i];
        if (c->iColumn >= 2 && c->iColumn <= 5 && c->op == SQLITE_INDEX_CONSTRAINT_EQ) {
            if (!c->usable) return SQLITE_CONSTRAINT;
            idx[c->iColumn - 2] = i;
        }
    }
    for (int k = 0; k < 4; k++) if (idx[k] < 0) return SQLITE_CONSTRAINT;
    for (int k = 0; k < 4; k++) { ii->aConstraintUsage[idx[k]].argvIndex = k + 1; ii->aConstraintUsage[idx[k]].omit = 1; }
    if (ii->nOrderBy == 1 && ii->aOrderBy[0].iColumn == 1 && !ii->aOrderBy[0].desc) ii->orderByConsumed = 1;
    ii->estimatedCost = 1.0; ii->estimatedRows = 18;
    return SQLITE_OK;
}

static int w_open(sqlite3_vtab *v, sqlite3_vtab_cursor **pp)
{
    WCur *c = sqlite3_malloc(sizeof *c); if (!c) return SQLITE_NOMEM;
    memset(c, 0, sizeof *c); *pp = &c->base; return SQLITE_OK;
}
static int w_close(sqlite3_vtab_cursor *cur) { WCur *c = (WCur *)cur; free(c->own); sqlite3_free(c); return SQLITE_OK; }

/* run the builder inside the current read transaction; returns malloc'd rowid array */
static int build_rows(sqlite3 *db, const char *tail, int tlen, i64 **out, int *nout)
{
    char *bs = builder_sql(tail, tlen); if (!bs) return SQLITE_NOMEM;
    sqlite3_stmt *st = 0;
    int rc = r_prepare_v2(db, bs, -1, &st, 0); free(bs);
    if (rc != SQLITE_OK || !st) return rc ? rc : SQLITE_ERROR;
    int cap = 256, n = 0; i64 *r = malloc(cap * sizeof *r);
    if (!r) { r_finalize(st); return SQLITE_NOMEM; }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) { i64 *t = realloc(r, (cap *= 2) * sizeof *r); if (!t) { free(r); r_finalize(st); return SQLITE_NOMEM; } r = t; }
        r[n++] = sqlite3_column_int64(st, 0);
    }
    r_finalize(st);
    if (rc != SQLITE_DONE) { free(r); return rc; }
    *out = r; *nout = n;
    return SQLITE_OK;
}

static int w_filter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv)
{
    WCur *c = (WCur *)cur; WVtab *vt = (WVtab *)cur->pVtab; sqlite3 *db = vt->db;
    free(c->own); c->own = 0; c->rows = 0; c->i = c->end = 0;
    if (argc != 4) return SQLITE_ERROR;
    i64 slot = sqlite3_value_int64(argv[0]), serial = sqlite3_value_int64(argv[1]);
    i64 off = sqlite3_value_int64(argv[2]), lim = sqlite3_value_int64(argv[3]);
    if (slot < 0 || slot >= WMAX) return SQLITE_ERROR;
    WEnt *e = &g_went[slot];
    pthread_mutex_lock(&g_mu);
    int okent = e->db == db && e->serial == (unsigned)serial && e->tail;
    struct wdb *w = wdb_get(db, 0);
    unsigned gen = 0; { unsigned *g = wgen_slot(db); gen = g ? *g : 0; }
    pthread_mutex_unlock(&g_mu);
    if (!okent || !w) { vt->base.zErrMsg = 0; g_w_fallback++; return SQLITE_ERROR; }
    /* validation inside this statement's read transaction */
    i64 dv = 0; int tc = sqlite3_total_changes(db); int ac = sqlite3_get_autocommit(db);
    if (read_dv(w, db, &dv) != 0) return SQLITE_ERROR;
    if (!(ac && e->built && e->dv == dv && e->tc == tc && e->gen == gen && gen)) {
        i64 *r = 0; int n = 0;
        int rc = build_rows(db, e->tail, e->tlen, &r, &n);
        if (rc != SQLITE_OK) return rc;
        g_w_build++;
        if (g_stats) {
            FILE *f = fopen(g_stats, "a");
            if (f) { fprintf(f, "win build slot=%d db=%p rows=%d ac=%d built=%d dv=%lld/%lld tc=%d/%d gen=%u/%u off=%lld lim=%lld\n",
                             (int)slot, (void *)db, n, ac, e->built, e->dv, dv, e->tc, tc, e->gen, gen, off, lim); fclose(f); }
        }
        if (ac && gen) {           /* cache only in autocommit mode (no own uncommitted changes that a ROLLBACK could undo) */
            free(e->rows); e->rows = r; e->nrows = n; e->dv = dv; e->tc = tc; e->gen = gen; e->built = 1;
        } else {
            c->own = r; c->rows = r;
            c->i = off < n ? (int)off : n; c->end = (off + lim < n) ? (int)(off + lim) : n;
            return SQLITE_OK;
        }
    }
    g_w_serve++;
    if (g_stats && (g_w_serve & 7) == 0) stats();
    c->rows = e->rows;
    int n = e->nrows;
    c->i = off < n ? (int)off : n;
    c->end = (off + lim < n) ? (int)(off + lim) : n;
    return SQLITE_OK;
}
static int w_next(sqlite3_vtab_cursor *cur) { ((WCur *)cur)->i++; return SQLITE_OK; }
static int w_eof(sqlite3_vtab_cursor *cur) { WCur *c = (WCur *)cur; return c->i >= c->end; }
static int w_column(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i)
{
    WCur *c = (WCur *)cur;
    if (i == 0) sqlite3_result_int64(ctx, c->rows[c->i]);
    else if (i == 1) sqlite3_result_int64(ctx, c->i);
    else sqlite3_result_null(ctx);
    return SQLITE_OK;
}
static int w_rowid(sqlite3_vtab_cursor *cur, i64 *r) { *r = ((WCur *)cur)->i; return SQLITE_OK; }

static const sqlite3_module g_wmod = { 0, 0, w_connect, w_best, w_disconnect, w_disconnect, w_open, w_close, w_filter, w_next,
                                       w_eof, w_column, w_rowid, 0, 0, 0, 0, 0, 0, 0 };

/* ---------- prepare-time decision ---------- */
static int is_ccdb(sqlite3 *db)
{
    const char *f = sqlite3_db_filename(db, "main");
    int l = f ? strlen(f) : 0;
    return l >= 6 && !strcmp(f + l - 6, "/cc.db");
}

/* returns 1 if it prepared something (rc in *prc), 0 to let the caller prepare the original */
static int win_try(sqlite3 *db, const char *sql, int nbyte, sqlite3_stmt **pst, const char **tail, int *prc)
{
    int n = nbyte < 0 ? (int)strlen(sql) : (int)strnlen(sql, nbyte);
    if (n < 40 || n > 60000) return 0;
    int sl0, sl1, t0, t1; long lim, off;
    if (!win_match(sql, n, &sl0, &sl1, &t0, &t1, &lim, &off)) return 0;
    if (!sqlite3_get_autocommit(db)) return 0;
    pthread_mutex_lock(&g_mu);
    struct wdb *w = wdb_get(db, 1);
    int st = w ? w->state : 2;
    pthread_mutex_unlock(&g_mu);
    if (st == 2) return 0;
    if (st == 0) {
        int ok = is_ccdb(db) && sqlite3_create_module_v2(db, "kppdb_win", &g_wmod, 0, 0) == SQLITE_OK;
        pthread_mutex_lock(&g_mu); w->state = ok ? 1 : 2; pthread_mutex_unlock(&g_mu);
        if (!ok) return 0;
    }
    /* entry for (db, tail) */
    const char *tl = sql + t0; int tlen = t1 - t0;
    unsigned h = hsh(tl, tlen);
    pthread_mutex_lock(&g_mu);
    int slot = -1, fr = -1;
    for (int i = 0; i < WMAX; i++) {
        WEnt *e = &g_went[i];
        if (e->db == db && e->h == h && e->tlen == tlen && !memcmp(e->tail, tl, tlen)) { slot = i; break; }
        if (!e->db && !e->tail && fr < 0) fr = i;
    }
    int state = slot >= 0 ? g_went[slot].state : 0;
    pthread_mutex_unlock(&g_mu);
    if (slot < 0) {
        if (fr < 0) return 0;
        char *orig = malloc(n + 1); if (!orig) return 0;
        memcpy(orig, sql, n); orig[n] = 0;
        int ok = plan_gate(db, orig, tl, tlen);
        free(orig);
        char *tc = malloc(tlen + 1); if (!tc) return 0;
        memcpy(tc, tl, tlen); tc[tlen] = 0;
        pthread_mutex_lock(&g_mu);
        if (g_went[fr].db || g_went[fr].tail) { pthread_mutex_unlock(&g_mu); free(tc); return 0; }
        WEnt *e = &g_went[fr];
        memset(e, 0, sizeof *e); e->db = db; e->tail = tc; e->tlen = tlen; e->h = h; e->serial = ++g_serial;
        e->state = ok ? 1 : 2;
        slot = fr; state = e->state;
        pthread_mutex_unlock(&g_mu);
        if (!ok) g_w_refuse++;
    }
    if (state != 1) return 0;
    /* rewritten statement */
    int sln = sl1 - sl0;
    char *q = malloc(sln + 200); if (!q) return 0;
    int k = snprintf(q, sln + 200, "SELECT %.*s FROM kppdb_win(%d,%u,%ld,%ld) AS kppdb_w CROSS JOIN Entries ON "
                     "Entries.rowid=kppdb_w.kppdb_rid ORDER BY kppdb_w.kppdb_pos", sln, sql + sl0, slot,
                     g_went[slot].serial, off, lim);
    if (k <= 0 || k >= sln + 200) { free(q); return 0; }
    sqlite3_stmt *s = 0;
    int rc = r_prepare_v2(db, q, k + 1, &s, 0);
    free(q);
    if (rc != SQLITE_OK || !s || sqlite3_bind_parameter_count(s) != 0) { if (s) r_finalize(s); g_w_fallback++; return 0; }
    *pst = s;
    if (tail) *tail = sql + n;     /* the whole text was one statement (no ';' allowed in the tail) */
    *prc = SQLITE_OK;
    g_w_rewrite++;
    return 1;
}

/* connection closed: forget its per-connection state; entries keep their memory only if statements may survive */
static void win_drop_stmts(sqlite3 *db)
{
    sqlite3_stmt *dv = 0;
    pthread_mutex_lock(&g_mu);
    struct wdb *w = wdb_get(db, 0);
    if (w) { dv = w->dv; w->dv = 0; }
    pthread_mutex_unlock(&g_mu);
    if (dv) r_finalize(dv);
}

static void win_forget(sqlite3 *db, int free_entries)
{
    pthread_mutex_lock(&g_mu);
    struct wdb *w = wdb_get(db, 0);
    if (w) memset(w, 0, sizeof *w);
    for (int i = 0; i < WMAX; i++) if (g_went[i].db == db) {
        WEnt *e = &g_went[i];
        e->db = 0; e->state = 2;
        if (free_entries) { free(e->rows); e->rows = 0; free(e->tail); e->tail = 0; e->nrows = 0; e->built = 0; }
        /* else: leaked on purpose (close_v2 zombie: a surviving statement may still reach xFilter, which then sees
           e->db != its db and fails instead of touching freed memory) */
    }
    pthread_mutex_unlock(&g_mu);
}
