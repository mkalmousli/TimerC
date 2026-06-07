/* db.c — SQLite session + interaction event logging */
#include "timer.h"

/* Resolve data.db to live next to the running binary.
   Falls back to the current working directory if /proc is unavailable. */
static void resolve_db_path(char *out, int out_sz) {
    char exe[MAX_PATH_LEN];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            size_t dlen = strlen(exe);
            if ((int)(dlen + 9) < out_sz) {
                memcpy(out, exe, dlen);
                memcpy(out + dlen, "/data.db", 9); /* 8 chars + '\0' */
                return;
            }
        }
    }
    memcpy(out, "data.db", 8);
}

void db_init(App *app) {
    resolve_db_path(app->db_path, sizeof(app->db_path));

    if (sqlite3_open(app->db_path, &app->db) != SQLITE_OK) {
        app_log(app, "WARN", "SQLite open failed: %s", sqlite3_errmsg(app->db));
        app->db = NULL;
        return;
    }

    /* WAL mode: better resilience under concurrent reads */
    sqlite3_exec(app->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    const char *sql =
        /* One row per timer run */
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  started_at TEXT    NOT NULL,"
        "  ended_at   TEXT,"
        "  duration_s INTEGER NOT NULL,"
        "  elapsed_s  INTEGER,"
        "  task       TEXT,"
        "  mode       TEXT    NOT NULL,"
        "  pausable   INTEGER NOT NULL,"
        "  fullscreen INTEGER NOT NULL,"
        "  status     TEXT    NOT NULL"
        ");"
        /* Every meaningful user interaction */
        "CREATE TABLE IF NOT EXISTS events ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER,"
        "  ts         TEXT    NOT NULL,"
        "  event      TEXT    NOT NULL,"
        "  detail     TEXT"
        ");";

    char *err = NULL;
    if (sqlite3_exec(app->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        app_log(app, "WARN", "DB schema error: %s", err);
        sqlite3_free(err);
    }
}

static void ts_now(char *out, int sz) {
    time_t t = time(NULL);
    struct tm *tm_p = localtime(&t);
    strftime(out, sz, "%Y-%m-%dT%H:%M:%S", tm_p);
}

/* Log a single interaction event.  detail may be NULL. */
void db_log_event(App *app, const char *event, const char *detail) {
    if (!app->db) return;
    char ts[32];
    ts_now(ts, sizeof(ts));
    const char *sql =
        "INSERT INTO events (session_id, ts, event, detail) VALUES (?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    if (app->session_id > 0)
        sqlite3_bind_int64(stmt, 1, app->session_id);
    else
        sqlite3_bind_null(stmt, 1);
    sqlite3_bind_text(stmt, 2, ts,     -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, event,  -1, SQLITE_STATIC);
    if (detail && detail[0])
        sqlite3_bind_text(stmt, 4, detail, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_start_session(App *app) {
    if (!app->db || app->session_id > 0) return;
    char ts[32];
    ts_now(ts, sizeof(ts));
    const char *sql =
        "INSERT INTO sessions "
        "(started_at, duration_s, task, mode, pausable, fullscreen, status)"
        " VALUES (?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 2, app->total_seconds);
    sqlite3_bind_text(stmt, 3, app->task, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, app->mode == MODE_SHUTDOWN ? "shutdown" : "alert",
                      -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 5, app->pausable);
    sqlite3_bind_int (stmt, 6, !app->mini);
    sqlite3_bind_text(stmt, 7, "running", -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    app->session_id = sqlite3_last_insert_rowid(app->db);
    sqlite3_finalize(stmt);
}

void db_finish_session(App *app, const char *status) {
    if (!app->db || app->session_id <= 0 || app->session_finalized) return;
    char ts[32];
    ts_now(ts, sizeof(ts));
    int elapsed = app->total_seconds - app->remaining_seconds;
    if (elapsed < 0) elapsed = 0;
    const char *sql =
        "UPDATE sessions SET ended_at=?, status=?, elapsed_s=? WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text (stmt, 1, ts,     -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 3, elapsed);
    sqlite3_bind_int64(stmt, 4, app->session_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    app->session_finalized = 1;
}
