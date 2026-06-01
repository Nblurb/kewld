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
#define KEWLD_DEFAULT_ADMIN_PORT 18081   /* local-only admin approval interface */
#define KEWLD_DEFAULT_SOCKS_PORT 19050
#define KEWLD_DEFAULT_CTRL_PORT  19051
#define KEWLD_INDEX_HOST         "kewl.cc"
#define KEWLD_INDEX_PORT         443
#define KEWLD_INDEX_REGISTER_PATH "/api/register"

/* ── limits ── */
#define KEWLD_MAX_TAG            32
#define KEWLD_MAX_TITLE          128
#define KEWLD_MAX_DESC           512
#define KEWLD_MAX_NAME           64
#define KEWLD_MAX_SUBJECT        256
#define KEWLD_MAX_CONTENT        65536   /* 64 KiB */
#define KEWLD_MAX_PGP_SIG        8192    /* armored PGP signature */
#define KEWLD_MAX_BODY_BYTES     (32 * 1024 * 1024) /* 32 MiB — video needs room */
#define KEWLD_ONION_ADDR_LEN     64      /* v3 .onion incl NUL */
#define KEWLD_DEFAULT_THREADS    200
#define KEWLD_DEFAULT_REPLIES    1000
#define KEWLD_PAGE_SIZE          20

/* ── media approval states ── */
#define KEWLD_MEDIA_PENDING   0
#define KEWLD_MEDIA_APPROVED  1
#define KEWLD_MEDIA_REJECTED  2

/* ── config ── */
typedef struct {
    char     tag[KEWLD_MAX_TAG];
    char     title[KEWLD_MAX_TITLE];
    char     desc[KEWLD_MAX_DESC];
    char     data_dir[512];
    char     onion_addr[KEWLD_ONION_ADDR_LEN];  /* filled at runtime */
    char     admin_token[128];                   /* secret token for admin UI */
    uint16_t http_port;
    uint16_t admin_port;
    int      tor_socks_port;
    int      tor_ctrl_port;
    int      register_index;    /* auto-register with kewl index */
    int      allow_media;       /* allow photo/video uploads */
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
    char     media_hash[65];     /* sha256 hex string, empty if none */
    char     media_ext[8];       /* e.g. ".jpg", ".mp4" */
    int      media_approved;     /* KEWLD_MEDIA_PENDING / APPROVED / REJECTED */
    char     pgp_sig[KEWLD_MAX_PGP_SIG];
    time_t   created_at;
    int      sage;
    int64_t  reply_count;        /* only valid for OP rows */
    time_t   last_bump;          /* only valid for OP rows */
} kewld_post_t;

/* ── opaque server handles ── */
typedef struct kewld_server       kewld_server_t;
typedef struct kewld_admin_server kewld_admin_server_t;

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
/* media approval */
int  db_get_pending_media(sqlite3 *db, kewld_post_t **out, int *count_out);
int  db_set_media_approval(sqlite3 *db, int64_t post_id, int is_thread, int status);
void db_free_posts(kewld_post_t *posts);

/* ───────────────────────────────────────────
   http.c  — public (Tor-facing) server
   ─────────────────────────────────────────── */
kewld_server_t *http_server_create(kewld_config_t *cfg, sqlite3 *db);
int             http_server_run(kewld_server_t *s);   /* blocks */
void            http_server_destroy(kewld_server_t *s);

/* ───────────────────────────────────────────
   admin.c — local-only admin approval server
   ─────────────────────────────────────────── */
kewld_admin_server_t *admin_server_create(kewld_config_t *cfg, sqlite3 *db);
int                   admin_server_run(kewld_admin_server_t *s);   /* blocks */
void                  admin_server_destroy(kewld_admin_server_t *s);

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
int  sha256_buf(const unsigned char *data, size_t len, char *hex_out);
char *url_decode(const char *in, char *out, size_t outlen);
char *json_escape(const char *in, char *out, size_t outlen);
long  current_unix(void);

#endif /* KEWLD_H */
