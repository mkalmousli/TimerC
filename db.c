/* db.c — SQLite session logging */
#include "timer.h"

void db_init(App *app) {
    if (sqlite3_open(app->db_path, &app->db) != SQLITE_OK) {
        app_log(app, "WARN", "SQLite open failed: %s", sqlite3_errmsg(app->db));
        app->db = NULL;
        return;
    }
    const char *sql =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  started_at TEXT NOT NULL,"
        "  ended_at TEXT,"
        "  duration_seconds INTEGER NOT NULL,"
        "  elapsed_seconds INTEGER,"
        "  task_name TEXT,"
        "  mode TEXT NOT NULL,"
        "  pausable INTEGER NOT NULL,"
        "  fullscreen INTEGER NOT NULL,"
        "  status TEXT NOT NULL"
        ");";
    char *err = NULL;
    if (sqlite3_exec(app->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        app_log(app, "WARN", "DB create table failed: %s", err);
        sqlite3_free(err);
    }
}

void db_start_session(App *app) {
    if (!app->db || app->session_id > 0) return;
    time_t t = time(NULL);
    struct tm *tm_p = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_p);
    const char *sql =
        "INSERT INTO sessions "
        "(started_at, duration_seconds, task_name, mode, pausable, fullscreen, status)"
        " VALUES (?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, app->total_seconds);
    sqlite3_bind_text(stmt, 3, app->task, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, app->mode == MODE_SHUTDOWN ? "shutdown" : "alert",
                      -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, app->pausable);
    sqlite3_bind_int(stmt, 6, !app->mini);
    sqlite3_bind_text(stmt, 7, "running", -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    app->session_id = sqlite3_last_insert_rowid(app->db);
    sqlite3_finalize(stmt);
}

void db_finish_session(App *app, const char *status) {
    if (!app->db || app->session_id <= 0 || app->session_finalized) return;
    time_t t = time(NULL);
    struct tm *tm_p = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_p);
    int elapsed = app->total_seconds - app->remaining_seconds;
    if (elapsed < 0) elapsed = 0;
    const char *sql =
        "UPDATE sessions SET ended_at=?, status=?, elapsed_seconds=? WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, elapsed);
    sqlite3_bind_int64(stmt, 4, app->session_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    app->session_finalized = 1;
}
