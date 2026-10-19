/* db.c — SQLite session + interaction event logging */
#include "timer.h"

/* data.db lives in the per-user config dir (see get_config_dir). */
static void resolve_db_path(char *out, int out_sz) {
    char dir[MAX_PATH_LEN];
    get_config_dir(dir, sizeof(dir));
    snprintf(out, out_sz, "%s/data.db", dir);
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

/* ── Insights browser queries ───────────────────────────── */

static int db_scalar_int(App *app, const char *sql) {
    if (!app->db) return 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int v = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

static long db_scalar_long(App *app, const char *sql) {
    if (!app->db) return 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    long v = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        v = (long)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

static void day_offset_to_iso(int offset, char *out, int sz) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_mday -= offset;
    mktime(&tm);
    strftime(out, sz, "%Y-%m-%d", &tm);
}

static void day_offset_to_label(int offset, char *out, int sz) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_mday -= offset;
    mktime(&tm);
    strftime(out, sz, "%a %b %d", &tm);
}

static void db_fill_chart(App *app, DbSummary *sum) {
    memset(sum->day_counts, 0, sizeof(sum->day_counts));
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    for (int i = 0; i < INS_CHART_DAYS; i++) {
        struct tm tm_day = tm_now;
        tm_day.tm_mday -= (INS_CHART_DAYS - 1 - i);
        mktime(&tm_day);
        strftime(sum->day_labels[i], sizeof(sum->day_labels[i]), "%d", &tm_day);
        strftime(sum->day_dates[i], sizeof(sum->day_dates[i]), "%Y-%m-%d", &tm_day);
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM sessions "
                 "WHERE date(started_at, 'localtime') = '%s';",
                 sum->day_dates[i]);
        sum->day_counts[i] = db_scalar_int(app, sql);
    }
}

static void db_fill_day_view(App *app, DbDayView *day, const char *day_iso) {
    memset(day, 0, sizeof(*day));
    snprintf(day->day_iso, sizeof(day->day_iso), "%s", day_iso);

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM sessions "
             "WHERE date(started_at, 'localtime') = '%s';", day_iso);
    day->sessions = db_scalar_int(app, sql);

    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM sessions "
             "WHERE date(started_at, 'localtime') = '%s' AND status='completed';",
             day_iso);
    day->completed = db_scalar_int(app, sql);

    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM sessions "
             "WHERE date(started_at, 'localtime') = '%s' AND status='stopped';",
             day_iso);
    day->stopped = db_scalar_int(app, sql);

    snprintf(sql, sizeof(sql),
             "SELECT COALESCE(SUM(elapsed_s),0) FROM sessions "
             "WHERE date(started_at, 'localtime') = '%s' AND elapsed_s IS NOT NULL;",
             day_iso);
    day->elapsed_s = db_scalar_long(app, sql);

    snprintf(sql, sizeof(sql),
             "SELECT COALESCE(SUM(duration_s),0) FROM sessions "
             "WHERE date(started_at, 'localtime') = '%s';", day_iso);
    day->planned_s = db_scalar_long(app, sql);

    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM events e "
             "INNER JOIN sessions s ON s.id = e.session_id "
             "WHERE date(s.started_at, 'localtime') = '%s';", day_iso);
    day->events = db_scalar_int(app, sql);

    snprintf(sql, sizeof(sql),
             "SELECT COALESCE(AVG(duration_s),0) FROM sessions "
             "WHERE date(started_at, 'localtime') = '%s';", day_iso);
    day->avg_duration_s = (int)db_scalar_long(app, sql);

    snprintf(sql, sizeof(sql),
             "SELECT COALESCE(AVG(elapsed_s),0) FROM sessions "
             "WHERE date(started_at, 'localtime') = '%s' AND elapsed_s IS NOT NULL;",
             day_iso);
    day->avg_elapsed_s = (int)db_scalar_long(app, sql);

    snprintf(sql, sizeof(sql),
             "SELECT CAST(strftime('%%H', started_at, 'localtime') AS INTEGER), "
             "COUNT(*) FROM sessions "
             "WHERE date(started_at, 'localtime') = '%s' "
             "GROUP BY strftime('%%H', started_at, 'localtime');", day_iso);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int hr = sqlite3_column_int(stmt, 0);
            int cnt = sqlite3_column_int(stmt, 1);
            if (hr >= 0 && hr < 24) day->hour_counts[hr] = cnt;
        }
        sqlite3_finalize(stmt);
    }

    const char *bucket_sql =
        "SELECT CASE "
        "  WHEN duration_s < 300 THEN 0 "
        "  WHEN duration_s < 900 THEN 1 "
        "  WHEN duration_s < 1800 THEN 2 "
        "  ELSE 3 END, COUNT(*) FROM sessions "
        "WHERE date(started_at, 'localtime') = ? "
        "GROUP BY 1;";
    if (sqlite3_prepare_v2(app->db, bucket_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, day_iso, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int b = sqlite3_column_int(stmt, 0);
            if (b >= 0 && b < 4) day->dur_buckets[b] = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }
}

void db_insights_load_events(App *app, int64_t session_id) {
    app->ins_n_events = 0;
    app->ins_loaded_session_id = session_id;
    if (!app->db || session_id <= 0) return;

    const char *sql =
        "SELECT id, session_id, ts, event, detail FROM events "
        "WHERE session_id=? ORDER BY id ASC LIMIT ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int(stmt, 2, INS_MAX_EVENTS);

    while (sqlite3_step(stmt) == SQLITE_ROW && app->ins_n_events < INS_MAX_EVENTS) {
        DbEvent *e = &app->ins_events[app->ins_n_events++];
        e->id         = sqlite3_column_int64(stmt, 0);
        e->session_id = sqlite3_column_int64(stmt, 1);
        const char *ts = (const char *)sqlite3_column_text(stmt, 2);
        const char *ev = (const char *)sqlite3_column_text(stmt, 3);
        const char *dt = (const char *)sqlite3_column_text(stmt, 4);
        snprintf(e->ts,     sizeof(e->ts),     "%s", ts ? ts : "");
        snprintf(e->event,  sizeof(e->event),  "%s", ev ? ev : "");
        snprintf(e->detail, sizeof(e->detail), "%s", dt ? dt : "");
    }
    sqlite3_finalize(stmt);
}

void db_insights_load_day(App *app, int day_offset) {
    if (day_offset < 0) day_offset = 0;
    app->ins_day_offset = day_offset;
    app->ins_n_sessions = 0;
    app->ins_n_events   = 0;
    app->ins_loaded_session_id = 0;

    char day_iso[16];
    char day_label[40];
    day_offset_to_iso(day_offset, day_iso, sizeof(day_iso));
    day_offset_to_label(day_offset, day_label, sizeof(day_label));

    if (!app->db) return;

    DbSummary *s = &app->ins_summary;
    s->sessions_total = db_scalar_int(app, "SELECT COUNT(*) FROM sessions;");
    s->completed      = db_scalar_int(app,
        "SELECT COUNT(*) FROM sessions WHERE status='completed';");
    s->stopped        = db_scalar_int(app,
        "SELECT COUNT(*) FROM sessions WHERE status='stopped';");
    s->running        = db_scalar_int(app,
        "SELECT COUNT(*) FROM sessions WHERE status='running';");
    s->mode_alert     = db_scalar_int(app,
        "SELECT COUNT(*) FROM sessions WHERE mode='alert';");
    s->mode_shutdown  = db_scalar_int(app,
        "SELECT COUNT(*) FROM sessions WHERE mode='shutdown';");
    s->elapsed_total_s = db_scalar_long(app,
        "SELECT COALESCE(SUM(elapsed_s),0) FROM sessions WHERE elapsed_s IS NOT NULL;");
    s->planned_total_s = db_scalar_long(app,
        "SELECT COALESCE(SUM(duration_s),0) FROM sessions;");
    s->events_total   = db_scalar_int(app, "SELECT COUNT(*) FROM events;");
    db_fill_chart(app, s);
    db_fill_day_view(app, &app->ins_day, day_iso);
    snprintf(app->ins_day.day_label, sizeof(app->ins_day.day_label), "%s", day_label);

    const char *sql =
        "SELECT id, started_at, ended_at, duration_s, elapsed_s, "
        "task, mode, pausable, fullscreen, status FROM sessions "
        "WHERE date(started_at, 'localtime') = ? "
        "ORDER BY started_at DESC LIMIT ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, day_iso, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, INS_MAX_SESSIONS);

    while (sqlite3_step(stmt) == SQLITE_ROW && app->ins_n_sessions < INS_MAX_SESSIONS) {
        DbSession *row = &app->ins_sessions[app->ins_n_sessions++];
        row->id         = sqlite3_column_int64(stmt, 0);
        row->duration_s = sqlite3_column_int(stmt, 3);
        row->elapsed_s  = sqlite3_column_type(stmt, 4) == SQLITE_NULL
                          ? -1 : sqlite3_column_int(stmt, 4);
        row->pausable   = sqlite3_column_int(stmt, 7);
        row->fullscreen = sqlite3_column_int(stmt, 8);
        const char *sa = (const char *)sqlite3_column_text(stmt, 1);
        const char *ea = (const char *)sqlite3_column_text(stmt, 2);
        const char *tk = (const char *)sqlite3_column_text(stmt, 5);
        const char *md = (const char *)sqlite3_column_text(stmt, 6);
        const char *st = (const char *)sqlite3_column_text(stmt, 9);
        snprintf(row->started_at, sizeof(row->started_at), "%s", sa ? sa : "");
        snprintf(row->ended_at,   sizeof(row->ended_at),   "%s", ea ? ea : "");
        snprintf(row->task,       sizeof(row->task),       "%s", tk ? tk : "");
        snprintf(row->mode,       sizeof(row->mode),       "%s", md ? md : "");
        snprintf(row->status,     sizeof(row->status),     "%s", st ? st : "");
    }
    sqlite3_finalize(stmt);

    if (app->ins_sel >= app->ins_n_sessions)
        app->ins_sel = app->ins_n_sessions > 0 ? 0 : 0;
    if (app->ins_sess_scroll > app->ins_sel)
        app->ins_sess_scroll = app->ins_sel;
    if (app->ins_n_sessions > 0)
        db_insights_load_events(app, app->ins_sessions[app->ins_sel].id);
}

void db_insights_reload(App *app) {
    db_insights_load_day(app, app->ins_day_offset);
}
