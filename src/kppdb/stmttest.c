/*
 * stmttest: randomized differential test for libkppdb's statement cache. Runs a seeded random sequence of
 * prepare_v2 / bind / step / column / reset / finalize / exec / close calls on two connections to one database
 * (plus writes and schema changes through the second connection) and prints every return code, errcode, errmsg,
 * column count/name/type/value and tail offset. Run it with and without LD_PRELOAD=libkppdb.so KPPDB_STMTCACHE=1:
 * the outputs must be byte-identical.  usage: stmttest DBFILE SEED STEPS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
int sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
int sqlite3_close(sqlite3 *);
int sqlite3_close_v2(sqlite3 *);
int sqlite3_exec(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
int sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
int sqlite3_step(sqlite3_stmt *);
int sqlite3_reset(sqlite3_stmt *);
int sqlite3_finalize(sqlite3_stmt *);
int sqlite3_clear_bindings(sqlite3_stmt *);
int sqlite3_bind_int(sqlite3_stmt *, int, int);
int sqlite3_bind_text(sqlite3_stmt *, int, const char *, int, void (*)(void *));
int sqlite3_bind_parameter_count(sqlite3_stmt *);
int sqlite3_column_count(sqlite3_stmt *);
const char *sqlite3_column_name(sqlite3_stmt *, int);
int sqlite3_column_type(sqlite3_stmt *, int);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
int sqlite3_errcode(sqlite3 *);
int sqlite3_extended_errcode(sqlite3 *);
const char *sqlite3_errmsg(sqlite3 *);
char *sqlite3_expanded_sql(sqlite3_stmt *);
void sqlite3_free(void *);
int sqlite3_changes(sqlite3 *);
int sqlite3_total_changes(sqlite3 *);
int sqlite3_busy_timeout(sqlite3 *, int);

static unsigned long long rs;
static unsigned rnd(unsigned n) { rs = rs * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(rs >> 33) % n; }

static const char *Q[] = {
    "SELECT a, b, c FROM t WHERE a > ? ORDER BY a LIMIT 5",
    "  -- comment\n  SELECT * FROM t WHERE b = ?",
    "SELECT count(*) FROM t",
    "SELECT a, b FROM t ORDER BY b COLLATE NOCASE, a LIMIT 3 OFFSET 2",
    "SELECT abs(-9223372036854775807 - 1)",          /* integer overflow: error at step */
    "SELECT a FROM t WHERE a = 1; SELECT 2",          /* tail not at end */
    "SELECT * FROM nosuchtable",                       /* prepare error */
    "BEGIN",
    "COMMIT",
    "END",
    "BEGIN; SELECT 1; COMMIT;",
    "SELECT a, b, c FROM t WHERE a IN (?, ?, ?)",
    "/* x */ select b from t where a between ? and ? order by a desc",
    "SELECT changes(), total_changes()",
    "UPDATE t SET c = c + 1 WHERE a = ?",              /* not eligible: write */
    "",
    "   ",
};
#define NQ (int)(sizeof Q / sizeof Q[0])

static sqlite3 *db, *db2;

static void pr_err(const char *w, int rc)
{
    printf("%s rc=%d err=%d ext=%d msg=%s chg=%d tot=%d\n", w, rc, sqlite3_errcode(db), sqlite3_extended_errcode(db),
           sqlite3_errmsg(db), sqlite3_changes(db), sqlite3_total_changes(db));
}

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    const char *f = argv[1]; rs = strtoull(argv[2], 0, 10); int steps = atoi(argv[3]);
    unlink(f);
    sqlite3_open_v2(f, &db, 6, 0); sqlite3_open_v2(f, &db2, 6, 0);
    sqlite3_busy_timeout(db, 0); sqlite3_busy_timeout(db2, 0);
    sqlite3_exec(db2, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT, c INT)", 0, 0, 0);
    for (int i = 1; i <= 40; i++) {
        char s[200]; snprintf(s, sizeof s, "INSERT INTO t VALUES(%d, '%c%d', %d)", i, 'a' + rnd(26), rnd(5), rnd(100));
        sqlite3_exec(db2, s, 0, 0, 0);
    }
    sqlite3_stmt *open[4] = {0}; int ncols_added = 0;
    for (int it = 0; it < steps; it++) {
        int op = rnd(100);
        printf("#%d op=%d ", it, op);
        if (op < 30) {                 /* prepare into a slot (finalize what was there) */
            int k = rnd(4);
            if (open[k]) { pr_err("fin-old", sqlite3_finalize(open[k])); open[k] = 0; }
            int q = rnd(NQ); const char *tail = 0; int nb = rnd(3) == 0 ? (int)strlen(Q[q]) + 1 : -1;
            int rc = sqlite3_prepare_v2(db, Q[q], nb, &open[k], rnd(4) ? &tail : 0);
            printf("prep q=%d k=%d tail=%d null=%d ", q, k, tail ? (int)(tail - Q[q]) : -1, open[k] == 0);
            pr_err("", rc);
            if (open[k]) printf("  cols=%d params=%d\n", sqlite3_column_count(open[k]), sqlite3_bind_parameter_count(open[k]));
        } else if (op < 40) {          /* bind */
            int k = rnd(4); if (!open[k]) { puts("nobind"); continue; }
            int n = sqlite3_bind_parameter_count(open[k]);
            for (int i = 1; i <= n + 1; i++) {
                int rc = rnd(2) ? sqlite3_bind_int(open[k], i, rnd(45)) : sqlite3_bind_text(open[k], i, "b1", -1, (void (*)(void *))-1);
                printf("bind%d=%d ", i, rc);
            }
            char *x = sqlite3_expanded_sql(open[k]); printf("exp=%s\n", x ? x : "null"); sqlite3_free(x);
        } else if (op < 70) {          /* step a few rows */
            int k = rnd(4); if (!open[k]) { puts("nostep"); continue; }
            int m = 1 + rnd(4);
            for (int i = 0; i < m; i++) {
                int rc = sqlite3_step(open[k]);
                printf("step rc=%d ", rc);
                if (rc == 100) {
                    int c = sqlite3_column_count(open[k]);
                    for (int j = 0; j < c; j++) {
                        const unsigned char *v = sqlite3_column_text(open[k], j);
                        printf("[%s:%d:%s]", sqlite3_column_name(open[k], j), sqlite3_column_type(open[k], j), v ? (const char *)v : "NULL");
                    }
                } else { pr_err(" end", rc); break; }
            }
            puts("");
        } else if (op < 76) {
            int k = rnd(4); if (!open[k]) { puts("noreset"); continue; }
            pr_err("reset", sqlite3_reset(open[k]));
            if (rnd(2)) printf("clear=%d\n", sqlite3_clear_bindings(open[k]));
        } else if (op < 86) {
            int k = rnd(4);
            pr_err("fin", sqlite3_finalize(open[k])); open[k] = 0;
        } else if (op < 91) {          /* exec on db */
            int q = rnd(NQ); char *em = 0;
            int rc = sqlite3_exec(db, Q[q], 0, 0, &em);
            printf("exec q=%d em=%s ", q, em ? em : "-"); sqlite3_free(em); pr_err("", rc);
        } else if (op < 96) {          /* write via the other connection */
            char s[200]; snprintf(s, sizeof s, "UPDATE t SET b='%c%d' WHERE a=%d", 'a' + rnd(26), rnd(5), 1 + rnd(40));
            int rc = sqlite3_exec(db2, s, 0, 0, 0);
            printf("w2 rc=%d\n", rc);
        } else if (op < 98) {          /* schema change via the other connection */
            if (ncols_added < 3) {
                char s[100]; snprintf(s, sizeof s, "ALTER TABLE t ADD COLUMN x%d INT DEFAULT %d", ncols_added, ncols_added);
                printf("alter rc=%d\n", sqlite3_exec(db2, s, 0, 0, 0)); ncols_added++;
            } else puts("noalter");
        } else {                       /* close + reopen db (finalize open statements first) */
            for (int k = 0; k < 4; k++) if (open[k]) { pr_err("fin-close", sqlite3_finalize(open[k])); open[k] = 0; }
            printf("close rc=%d\n", rnd(2) ? sqlite3_close(db) : sqlite3_close_v2(db));
            sqlite3_open_v2(f, &db, 6, 0); sqlite3_busy_timeout(db, 0);
        }
    }
    for (int k = 0; k < 4; k++) if (open[k]) sqlite3_finalize(open[k]);
    printf("final close rc=%d\n", sqlite3_close(db));
    sqlite3_close(db2);
    return 0;
}
