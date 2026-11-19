/*
 * wintest: randomized differential test for KPPDB_WINDOW on a COPY of a content catalog (DIR/cc.db, the name must end
 * in /cc.db). Connection A runs KPP-style page queries (templates file: one query per line, "@LIMIT@" placeholder for
 * the paging clause) at random limits/offsets; connection B (and sometimes A itself, also inside BEGIN..ROLLBACK)
 * changes rows: lastAccess, titles, visibility/archived flags, inserts copies, deletes. Every query result is printed
 * as row count + FNV hash over all column names/types/values (no titles in the output).
 * Collation "icu" = firmware ICU 65 numeric collation, the same function for reads and writes.
 * usage: wintest DIR SEED STEPS TEMPLATES
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
int sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
int sqlite3_close(sqlite3 *);
int sqlite3_exec(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
int sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
int sqlite3_step(sqlite3_stmt *);
int sqlite3_finalize(sqlite3_stmt *);
int sqlite3_column_count(sqlite3_stmt *);
const char *sqlite3_column_name(sqlite3_stmt *, int);
int sqlite3_column_type(sqlite3_stmt *, int);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
int sqlite3_column_bytes(sqlite3_stmt *, int);
const char *sqlite3_errmsg(sqlite3 *);
int sqlite3_create_collation(sqlite3 *, const char *, int, void *, int (*)(void *, int, const void *, int, const void *));
int sqlite3_busy_timeout(sqlite3 *, int);
void sqlite3_free(void *);

typedef uint16_t UChar; typedef int UErrorCode;
void *ucol_open_65(const char *, UErrorCode *);
void ucol_setAttribute_65(void *, int, int, UErrorCode *);
int ucol_strcoll_65(void *, const UChar *, int32_t, const UChar *, int32_t);
UChar *u_strFromJavaModifiedUTF8WithSub_65(UChar *, int32_t, int32_t *, const char *, int32_t, int32_t, int32_t *, UErrorCode *);
static void *g_coll;
static int to16(const void *s, int n, UChar *buf, int cap, UChar **out)
{
    UErrorCode e = 0; int32_t len = 0;
    u_strFromJavaModifiedUTF8WithSub_65(buf, cap, &len, (const char *)s, n, 0xFFFD, NULL, &e);
    if (e > 0 || len > cap) { UChar *h = malloc(sizeof(UChar) * (len + 1)); e = 0;
        u_strFromJavaModifiedUTF8WithSub_65(h, len + 1, &len, (const char *)s, n, 0xFFFD, NULL, &e); *out = h; return len; }
    *out = buf; return len;
}
static int icu_cmp(void *u, int n1, const void *s1, int n2, const void *s2)
{
    UChar b1[256], b2[256]; UChar *p1, *p2;
    int l1 = to16(s1, n1, b1, 256, &p1), l2 = to16(s2, n2, b2, 256, &p2);
    int r = ucol_strcoll_65(g_coll, p1, l1, p2, l2);
    if (p1 != b1) free(p1); if (p2 != b2) free(p2);
    return r;
}
static sqlite3 *opendb(const char *p)
{
    sqlite3 *db; UErrorCode e = 0;
    if (sqlite3_open_v2(p, &db, 2, 0)) { printf("open failed\n"); exit(2); }
    if (!g_coll) { g_coll = ucol_open_65("en_US", &e); ucol_setAttribute_65(g_coll, 7, 17, &e); }
    sqlite3_create_collation(db, "icu", 1, 0, icu_cmp);
    sqlite3_busy_timeout(db, 0);
    return db;
}

static unsigned long long rs;
static unsigned rnd(unsigned n) { rs = rs * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(rs >> 33) % (n ? n : 1); }

static sqlite3 *A, *B;
static int nrows_total;

static void run_query(const char *tpl, int qi)
{
    char lim[64], *sql = malloc(strlen(tpl) + 64);
    int l = (int[]){ 9, 18, 6, 1, 100, 0 }[rnd(6)], o = rnd(nrows_total + 40);
    int form = rnd(4);
    if (form == 0) snprintf(lim, sizeof lim, "LIMIT %d", l);
    else if (form == 1) snprintf(lim, sizeof lim, "  LIMIT %d  OFFSET %d  ", l, o);
    else snprintf(lim, sizeof lim, "LIMIT %d OFFSET %d", l, o);
    const char *p = strstr(tpl, "@LIMIT@");
    memcpy(sql, tpl, p - tpl); strcpy(sql + (p - tpl), lim); strcat(sql, p + 7);
    sqlite3_stmt *st = 0;
    int rc = sqlite3_prepare_v2(A, sql, rnd(2) ? -1 : (int)strlen(sql) + 1, &st, 0);
    if (rc) { printf("q%d %s prep rc=%d %s\n", qi, lim, rc, sqlite3_errmsg(A)); free(sql); return; }
    unsigned long long h = 1469598103934665603ULL; int rows = 0, nc = sqlite3_column_count(st);
    for (int i = 0; i < nc; i++) for (const char *c = sqlite3_column_name(st, i); *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211ULL;
    while ((rc = sqlite3_step(st)) == 100) {
        rows++;
        for (int i = 0; i < nc; i++) {
            int t = sqlite3_column_type(st, i);
            const unsigned char *v = sqlite3_column_text(st, i); int n = sqlite3_column_bytes(st, i);
            h = (h ^ (unsigned)t) * 1099511628211ULL;
            for (int k = 0; k < n; k++) h = (h ^ v[k]) * 1099511628211ULL;
            h = (h ^ 0xff) * 1099511628211ULL;
        }
    }
    printf("q%d %s cols=%d rows=%d rc=%d hash=%016llx\n", qi, lim, nc, rows, rc, h);
    sqlite3_finalize(st); free(sql);
}

static void wexec(sqlite3 *db, const char *what, const char *sql)
{
    char *em = 0; int rc = sqlite3_exec(db, sql, 0, 0, &em);
    printf("%s rc=%d\n", what, rc); sqlite3_free(em);
}

int main(int argc, char **argv)
{
    if (argc < 5) return 2;
    char path[512]; snprintf(path, sizeof path, "%s/cc.db", argv[1]);
    rs = strtoull(argv[2], 0, 10); int steps = atoi(argv[3]);
    int wp = getenv("WT_WRITES") ? atoi(getenv("WT_WRITES")) : 30;
    FILE *f = fopen(argv[4], "r"); if (!f) return 2;
    static char *tpl[64]; int nt = 0; static char line[65536];
    while (nt < 64 && fgets(line, sizeof line, f)) { if (strstr(line, "@LIMIT@")) { line[strcspn(line, "\n")] = 0; tpl[nt++] = strdup(line); } }
    fclose(f);
    A = opendb(path); B = opendb(path);
    sqlite3_stmt *st; sqlite3_prepare_v2(B, "SELECT count(*) FROM Entries", -1, &st, 0); sqlite3_step(st);
    nrows_total = atoi((const char *)sqlite3_column_text(st, 0)); sqlite3_finalize(st);
    printf("templates=%d rows=%d\n", nt, nrows_total);
    char sql[1024];
    for (int it = 0; it < steps; it++) {
        int op = rnd(100);
        if (op < 100 - wp) { int q = rnd(nt); run_query(tpl[q], q); continue; }
        op = 70 + (op - (100 - wp)) * 30 / (wp ? wp : 1);
        sqlite3 *w = rnd(5) ? B : A;
        const char *who = w == A ? "A" : "B";
        int r = 1 + rnd(nrows_total + 10);
        if (op < 78) snprintf(sql, sizeof sql, "UPDATE Entries SET p_lastAccess=%u WHERE rowid=%d", rnd(3) ? rnd(2000000000) : 0, r);
        else if (op < 83) snprintf(sql, sizeof sql, "UPDATE Entries SET p_titles_0_collation=(SELECT p_titles_0_collation FROM Entries WHERE rowid=%d) WHERE rowid=%d", 1 + rnd(nrows_total), r);
        else if (op < 87) snprintf(sql, sizeof sql, "UPDATE Entries SET p_isVisibleInHome=NOT p_isVisibleInHome, p_isArchived=%d WHERE rowid=%d", rnd(2), r);
        else if (op < 90) snprintf(sql, sizeof sql, "DELETE FROM Entries WHERE rowid=%d", r);
        else if (op < 93) snprintf(sql, sizeof sql, "INSERT INTO Entries SELECT * FROM Entries WHERE rowid=%d", r);   /* fails on unique p_uuid: kept as an error case */
        else if (op < 96) {
            snprintf(sql, sizeof sql, "INSERT INTO Entries(p_uuid, p_type, p_cdeType, p_isVisibleInHome, p_isArchived, p_lastAccess, p_titles_0_collation, p_titles_0_nominal, p_originType) "
                     "VALUES('test-%d-%d','Entry:Item','PDOC',1,0,%u,'T%u','T',0)", it, r, rnd(2000000000), rnd(1000));
        } else {      /* own-connection transaction with a query inside, then rollback or commit */
            wexec(A, "A begin", "BEGIN");
            snprintf(sql, sizeof sql, "UPDATE Entries SET p_lastAccess=%u WHERE rowid=%d", rnd(2000000000), r);
            wexec(A, "A upd", sql);
            int q = rnd(nt); run_query(tpl[q], q);
            wexec(A, rnd(2) ? "A rollback" : "A commit", rnd(2) ? "ROLLBACK" : "COMMIT");
            sqlite3_exec(A, "ROLLBACK", 0, 0, 0);
            continue;
        }
        wexec(w, who, sql);
    }
    sqlite3_close(A); sqlite3_close(B);
    puts("done");
    return 0;
}
