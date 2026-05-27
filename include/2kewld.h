#ifndef KEWLD_H
#define KEWLD_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sqlite3.h>

/* ── version ── */
#define KEWLD_VERSION            "0.1.0"

/* ── network ── */
#define KEWLD_DEFAULT_HTTP_PORT  18080
#define KEWLD_DEFAULT_SOCKS_PORT 19050
#define KEWLD_DEFAULT_CTRL_PORT  19051
#define KEWLD_INDEX_HOST         "204.168.222.184"
#define KEWLD_INDEX_REGISTER_PATH "/api/register"

/* ── limits ── */
#define KEWLD_MAX_TAG            32
#define KEWLD_MAX_TITLE          128
#define KEWLD_MAX_DESC           512
#define KEWLD_MAX_NAME           64
#define KEWLD_MAX_SUBJECT        256
#define KEWLD_MAX_CONTENT        65536   /* 64 KiB */
#define KEWLD_MAX_BODY_BYTES     (4 * 1024 * 1024)
#define KEWLD_ONION_ADDR_LEN     64      /* v3 .onion incl NUL */
#define KEWLD_DEFAULT_THREADS    200
#define KEWLD_DEFAULT_REPLIES    1000
#define KEWLD_PAGE_SIZE          20

/* ── config ── */
typedef struct {
    char     tag[KEWLD_MAX_TAG];
    char     title[KEWLD_MAX_TITLE];
    char     desc[KEWLD_MAX_DESC];
    char     data_dir[512];
    char     onion_addr[KEWLD_ONION_ADDR_LEN];  /* filled at runtime */
    uint16_t http_port;
    int      tor_socks_port;
    int      tor_ctrl_port;
    int      register_index;    /* auto-register with 2kewl index */
    int      allow_images;
    int      nsfw;
    int      daemon;            /* fork to background */
    char     pid_file[512];
    int      max_threads;
    int      max_replies;
    int      verbose;
} kewld_config_t;

/* ── data model ── */
typedef struct {
    int64_t  id;
    int64_t  thread_id;          /* 0 means this post IS the thread OP */
    char     name[KEWLD_MAX_NAME];
    char     subject[KEWLD_MAX_SUBJECT];
    char     content[KEWLD_MAX_CONTENT];
    char     image_hash[65];     /* sha256 hex string, empty if none */
    char     image_ext[8];       /* e.g. ".jpg" */
    time_t   created_at;
    int      sage;
    int64_t  reply_count;        /* only valid for OP rows */
    time_t   last_bump;          /* only valid for OP rows */
} kewld_post_t;

/* ── opaque server handle ── */
typedef struct kewld_server kewld_server_t;

/* ───────────────────────────────────────────
   db.c
   ─────────────────────────────────────────── */
int  db_open(const char *path, sqlite3 **db_out);
int  db_init_schema(sqlite3 *db);
int  db_insert_post(sqlite3 *db, kewld_post_t *p);  /* fills p->id */
int  db_get_threads(sqlite3 *db, int page,
                    kewld_post_t **out, int *count_out);
int  db_get_thread(sqlite3 *db, int64_t thread_id,
                   kewld_post_t **out, int *count_out);
int  db_thread_exists(sqlite3 *db, int64_t thread_id);
int  db_bump_thread(sqlite3 *db, int64_t thread_id);
void db_free_posts(kewld_post_t *posts);

/* ───────────────────────────────────────────
   http.c
   ─────────────────────────────────────────── */
kewld_server_t *http_server_create(kewld_config_t *cfg, sqlite3 *db);
int             http_server_run(kewld_server_t *s);   /* blocks */
void            http_server_destroy(kewld_server_t *s);

/* ───────────────────────────────────────────
   tor.c
   ─────────────────────────────────────────── */
int  tor_launch(kewld_config_t *cfg);
int  tor_wait_ready(int ctrl_port, int timeout_sec);
int  tor_read_onion(const char *data_dir, char *out, size_t outlen);
void tor_shutdown(void);

/* ───────────────────────────────────────────
   index.c
   ─────────────────────────────────────────── */
int  index_register(const kewld_config_t *cfg);

/* ───────────────────────────────────────────
   util.c
   ─────────────────────────────────────────── */
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_err (const char *fmt, ...);
void daemonize(const char *pid_file);
int  sha256_file(const char *path, char *hex_out);  /* hex_out: 65 bytes */
char *url_decode(const char *in, char *out, size_t outlen);
char *json_escape(const char *in, char *out, size_t outlen);
long  current_unix(void);

#endif /* KEWLD_H */
