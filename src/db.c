#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include "2kewld.h"

int db_open(const char *path, sqlite3 **db_out) {
    int rc = sqlite3_open(path, db_out);
    if (rc != SQLITE_OK) { log_err("db_open: %s", sqlite3_errmsg(*db_out)); return -1; }
    sqlite3_exec(*db_out, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(*db_out, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(*db_out, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    return 0;
}

int db_init_schema(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS threads ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name       TEXT NOT NULL DEFAULT 'Anonymous',"
        "  subject    TEXT NOT NULL DEFAULT '',"
        "  content    TEXT NOT NULL,"
        "  image_hash TEXT NOT NULL DEFAULT '',"
        "  image_ext  TEXT NOT NULL DEFAULT '',"
        "  pgp_sig    TEXT NOT NULL DEFAULT '',"
        "  created_at INTEGER NOT NULL,"
        "  last_bump  INTEGER NOT NULL,"
        "  reply_count INTEGER NOT NULL DEFAULT 0,"
        "  sage       INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS posts ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  thread_id  INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,"
        "  name       TEXT NOT NULL DEFAULT 'Anonymous',"
        "  subject    TEXT NOT NULL DEFAULT '',"
        "  content    TEXT NOT NULL,"
        "  image_hash TEXT NOT NULL DEFAULT '',"
        "  image_ext  TEXT NOT NULL DEFAULT '',"
        "  pgp_sig    TEXT NOT NULL DEFAULT '',"
        "  created_at INTEGER NOT NULL,"
        "  sage       INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_posts_thread ON posts(thread_id);"
        "CREATE INDEX IF NOT EXISTS idx_threads_bump ON threads(last_bump DESC);";
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { log_err("db_init_schema: %s", err); sqlite3_free(err); return -1; }
    /* migrate existing databases that lack pgp_sig */
    sqlite3_exec(db, "ALTER TABLE threads ADD COLUMN pgp_sig TEXT NOT NULL DEFAULT '';", NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE posts   ADD COLUMN pgp_sig TEXT NOT NULL DEFAULT '';", NULL, NULL, NULL);
    return 0;
}

int db_insert_post(sqlite3 *db, kewld_post_t *p) {
    long now = current_unix();
    p->created_at = (time_t)now;
    if (p->thread_id == 0) {
        const char *sql =
            "INSERT INTO threads(name,subject,content,image_hash,image_ext,pgp_sig,created_at,last_bump,sage)"
            " VALUES(?,?,?,?,?,?,?,?,?);";
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_text(st, 1, p->name,       -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, p->subject,    -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, p->content,    -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, p->image_hash, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 5, p->image_ext,  -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 6, p->pgp_sig,    -1, SQLITE_STATIC);
        sqlite3_bind_int64(st,7, now);
        sqlite3_bind_int64(st,8, now);
        sqlite3_bind_int(st, 9, p->sage);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) { log_err("db_insert_post(thread): %s", sqlite3_errmsg(db)); return -1; }
        p->id = sqlite3_last_insert_rowid(db);
        p->thread_id = p->id;
    } else {
        const char *sql =
            "INSERT INTO posts(thread_id,name,subject,content,image_hash,image_ext,pgp_sig,created_at,sage)"
            " VALUES(?,?,?,?,?,?,?,?,?);";
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_int64(st,1, p->thread_id);
        sqlite3_bind_text(st, 2, p->name,       -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, p->subject,    -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, p->content,    -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 5, p->image_hash, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 6, p->image_ext,  -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 7, p->pgp_sig,    -1, SQLITE_STATIC);
        sqlite3_bind_int64(st,8, now);
        sqlite3_bind_int(st, 9, p->sage);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) { log_err("db_insert_post(reply): %s", sqlite3_errmsg(db)); return -1; }
        p->id = sqlite3_last_insert_rowid(db);
        if (!p->sage) db_bump_thread(db, p->thread_id);
        else {
            sqlite3_stmt *uc;
            sqlite3_prepare_v2(db,"UPDATE threads SET reply_count=reply_count+1 WHERE id=?",-1,&uc,NULL);
            sqlite3_bind_int64(uc,1,p->thread_id);
            sqlite3_step(uc); sqlite3_finalize(uc);
        }
    }
    return 0;
}

int db_bump_thread(sqlite3 *db, int64_t thread_id) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "UPDATE threads SET last_bump=?, reply_count=reply_count+1 WHERE id=?",
        -1, &st, NULL);
    sqlite3_bind_int64(st,1,current_unix());
    sqlite3_bind_int64(st,2,thread_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

int db_thread_exists(sqlite3 *db, int64_t thread_id) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,"SELECT 1 FROM threads WHERE id=?",-1,&st,NULL);
    sqlite3_bind_int64(st,1,thread_id);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

int db_get_threads(sqlite3 *db, int page, kewld_post_t **out, int *count_out) {
    int offset = page * KEWLD_PAGE_SIZE;
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "SELECT id,name,subject,content,image_hash,image_ext,pgp_sig,created_at,last_bump,reply_count,sage"
        " FROM threads ORDER BY last_bump DESC LIMIT ? OFFSET ?",
        -1, &st, NULL);
    sqlite3_bind_int(st,1,KEWLD_PAGE_SIZE);
    sqlite3_bind_int(st,2,offset);
    kewld_post_t *arr = calloc(KEWLD_PAGE_SIZE, sizeof(kewld_post_t));
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        kewld_post_t *p = &arr[n++];
        p->id          = sqlite3_column_int64(st,0);
        p->thread_id   = p->id;
        strncpy(p->name,       (const char*)sqlite3_column_text(st,1), sizeof(p->name)-1);
        strncpy(p->subject,    (const char*)sqlite3_column_text(st,2), sizeof(p->subject)-1);
        strncpy(p->content,    (const char*)sqlite3_column_text(st,3), sizeof(p->content)-1);
        strncpy(p->image_hash, (const char*)sqlite3_column_text(st,4), sizeof(p->image_hash)-1);
        strncpy(p->image_ext,  (const char*)sqlite3_column_text(st,5), sizeof(p->image_ext)-1);
        strncpy(p->pgp_sig,    (const char*)sqlite3_column_text(st,6), sizeof(p->pgp_sig)-1);
        p->created_at  = (time_t)sqlite3_column_int64(st,7);
        p->last_bump   = (time_t)sqlite3_column_int64(st,8);
        p->reply_count = sqlite3_column_int64(st,9);
        p->sage        = sqlite3_column_int(st,10);
    }
    sqlite3_finalize(st);
    *out = arr; *count_out = n;
    return 0;
}

int db_get_thread(sqlite3 *db, int64_t thread_id, kewld_post_t **out, int *count_out) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "SELECT id,name,subject,content,image_hash,image_ext,pgp_sig,created_at,last_bump,reply_count,sage"
        " FROM threads WHERE id=?",
        -1, &st, NULL);
    sqlite3_bind_int64(st,1,thread_id);
    kewld_post_t *arr = calloc(KEWLD_DEFAULT_REPLIES + 1, sizeof(kewld_post_t));
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        kewld_post_t *p = &arr[n++];
        p->id          = sqlite3_column_int64(st,0);
        p->thread_id   = p->id;
        strncpy(p->name,       (const char*)sqlite3_column_text(st,1), sizeof(p->name)-1);
        strncpy(p->subject,    (const char*)sqlite3_column_text(st,2), sizeof(p->subject)-1);
        strncpy(p->content,    (const char*)sqlite3_column_text(st,3), sizeof(p->content)-1);
        strncpy(p->image_hash, (const char*)sqlite3_column_text(st,4), sizeof(p->image_hash)-1);
        strncpy(p->image_ext,  (const char*)sqlite3_column_text(st,5), sizeof(p->image_ext)-1);
        strncpy(p->pgp_sig,    (const char*)sqlite3_column_text(st,6), sizeof(p->pgp_sig)-1);
        p->created_at  = (time_t)sqlite3_column_int64(st,7);
        p->last_bump   = (time_t)sqlite3_column_int64(st,8);
        p->reply_count = sqlite3_column_int64(st,9);
        p->sage        = sqlite3_column_int(st,10);
    }
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db,
        "SELECT id,thread_id,name,subject,content,image_hash,image_ext,pgp_sig,created_at,sage"
        " FROM posts WHERE thread_id=? ORDER BY id ASC",
        -1, &st, NULL);
    sqlite3_bind_int64(st,1,thread_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        kewld_post_t *p = &arr[n++];
        p->id          = sqlite3_column_int64(st,0);
        p->thread_id   = sqlite3_column_int64(st,1);
        strncpy(p->name,       (const char*)sqlite3_column_text(st,2), sizeof(p->name)-1);
        strncpy(p->subject,    (const char*)sqlite3_column_text(st,3), sizeof(p->subject)-1);
        strncpy(p->content,    (const char*)sqlite3_column_text(st,4), sizeof(p->content)-1);
        strncpy(p->image_hash, (const char*)sqlite3_column_text(st,5), sizeof(p->image_hash)-1);
        strncpy(p->image_ext,  (const char*)sqlite3_column_text(st,6), sizeof(p->image_ext)-1);
        strncpy(p->pgp_sig,    (const char*)sqlite3_column_text(st,7), sizeof(p->pgp_sig)-1);
        p->created_at  = (time_t)sqlite3_column_int64(st,8);
        p->sage        = sqlite3_column_int(st,9);
    }
    sqlite3_finalize(st);
    *out = arr; *count_out = n;
    return 0;
}

void db_free_posts(kewld_post_t *posts) { free(posts); }
