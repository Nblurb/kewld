#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "2kewld.h"

struct kewld_server {
    kewld_config_t *cfg;
    sqlite3        *db;
    int             fd;
};

typedef struct {
    int              client_fd;
    kewld_server_t  *srv;
} conn_ctx_t;

static void send_response(int fd, int status, const char *ctype, const char *body, size_t blen) {
    char hdr[512];
    const char *reason = (status == 200) ? "OK" :
                         (status == 201) ? "Created" :
                         (status == 400) ? "Bad Request" :
                         (status == 404) ? "Not Found" :
                         (status == 405) ? "Method Not Allowed" :
                         (status == 413) ? "Payload Too Large" : "Internal Server Error";
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n", status, reason, ctype, blen);
    write(fd, hdr, hlen);
    if (blen > 0) write(fd, body, blen);
}

static void send_json(int fd, int status, const char *json) {
    send_response(fd, status, "application/json", json, strlen(json));
}

static void send_err(int fd, int status, const char *msg) {
    char buf[256];
    char esc[512];
    json_escape(msg, esc, sizeof(esc));
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esc);
    send_json(fd, status, buf);
}

static int read_request(int fd, char **method_out, char **path_out, char **body_out, size_t *blen_out) {
    char *buf = malloc(KEWLD_MAX_BODY_BYTES);
    if (!buf) return -1;
    size_t total = 0;
    ssize_t n;
    while (total < KEWLD_MAX_BODY_BYTES - 1) {
        n = read(fd, buf + total, KEWLD_MAX_BODY_BYTES - 1 - total);
        if (n <= 0) break;
        total += n;
        if (memmem(buf, total, "\r\n\r\n", 4)) break;
    }
    buf[total] = '\0';
    char *header_end = strstr(buf, "\r\n\r\n");
    if (!header_end) { free(buf); return -1; }
    *header_end = '\0';
    char *headers = buf;
    char *body_start = header_end + 4;
    char *line_end = strstr(headers, "\r\n");
    if (line_end) *line_end = '\0';
    char *sp1 = strchr(headers, ' ');
    if (!sp1) { free(buf); return -1; }
    *sp1 = '\0';
    char *sp2 = strchr(sp1+1, ' ');
    if (sp2) *sp2 = '\0';
    *method_out = strdup(headers);
    *path_out   = strdup(sp1+1);
    size_t content_length = 0;
    char *cl = strcasestr(buf + strlen(headers) + 1, "content-length:");
    if (!cl && line_end) cl = strcasestr(line_end + 1, "content-length:");
    if (cl) content_length = (size_t)atol(cl + 15);
    if (content_length > 0) {
        size_t body_in_buf = total - (size_t)(body_start - buf);
        *body_out = malloc(content_length + 1);
        if (!*body_out) { free(buf); free(*method_out); free(*path_out); return -1; }
        memcpy(*body_out, body_start, body_in_buf < content_length ? body_in_buf : content_length);
        if (body_in_buf < content_length) {
            size_t remaining = content_length - body_in_buf;
            while (remaining > 0) {
                n = read(fd, *body_out + (content_length - remaining), remaining);
                if (n <= 0) break;
                remaining -= n;
            }
        }
        (*body_out)[content_length] = '\0';
        *blen_out = content_length;
    } else {
        *body_out = NULL;
        *blen_out = 0;
    }
    free(buf);
    return 0;
}

static char *parse_form_field(const char *body, const char *key, char *out, size_t outlen) {
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *end = strchr(p, '&');
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            if (vlen >= outlen) vlen = outlen - 1;
            char tmp[KEWLD_MAX_BODY_BYTES];
            memcpy(tmp, p, vlen); tmp[vlen] = '\0';
            url_decode(tmp, out, outlen);
            return out;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    out[0] = '\0';
    return NULL;
}

static char *post_to_json(const kewld_post_t *p, char *buf, size_t buflen) {
    char name[256], subject[512], content[KEWLD_MAX_CONTENT*2], ihash[130], iext[16];
    json_escape(p->name,       name,    sizeof(name));
    json_escape(p->subject,    subject, sizeof(subject));
    json_escape(p->content,    content, sizeof(content));
    json_escape(p->image_hash, ihash,   sizeof(ihash));
    json_escape(p->image_ext,  iext,    sizeof(iext));
    snprintf(buf, buflen,
        "{\"id\":%lld,\"thread_id\":%lld,\"name\":\"%s\",\"subject\":\"%s\","
        "\"content\":\"%s\",\"image_hash\":\"%s\",\"image_ext\":\"%s\","
        "\"created_at\":%ld,\"reply_count\":%lld,\"last_bump\":%ld,\"sage\":%d}",
        (long long)p->id, (long long)p->thread_id,
        name, subject, content, ihash, iext,
        (long)p->created_at, (long long)p->reply_count, (long)p->last_bump, p->sage);
    return buf;
}

static void handle_get_board_info(int fd, kewld_server_t *srv) {
    char buf[1024], tag[64], title[256], desc[1024];
    json_escape(srv->cfg->tag,        tag,   sizeof(tag));
    json_escape(srv->cfg->title,      title, sizeof(title));
    json_escape(srv->cfg->desc,       desc,  sizeof(desc));
    snprintf(buf, sizeof(buf),
        "{\"tag\":\"/%s/\",\"title\":\"%s\",\"desc\":\"%s\","
        "\"nsfw\":%d,\"allow_images\":%d,\"version\":\"%s\"}",
        tag, title, desc, srv->cfg->nsfw, srv->cfg->allow_images, KEWLD_VERSION);
    send_json(fd, 200, buf);
}

static void handle_get_threads(int fd, kewld_server_t *srv, const char *query) {
    int page = 0;
    if (query) {
        const char *p = strstr(query, "page=");
        if (p) page = atoi(p + 5);
    }
    kewld_post_t *posts = NULL;
    int count = 0;
    if (db_get_threads(srv->db, page, &posts, &count) != 0) {
        send_err(fd, 500, "database error"); return;
    }
    size_t bufsz = (size_t)(count + 1) * (KEWLD_MAX_CONTENT * 3);
    char *out = malloc(bufsz);
    size_t pos = 0;
    out[pos++] = '[';
    for (int i = 0; i < count; i++) {
        char pbuf[KEWLD_MAX_CONTENT * 3];
        post_to_json(&posts[i], pbuf, sizeof(pbuf));
        size_t plen = strlen(pbuf);
        memcpy(out + pos, pbuf, plen); pos += plen;
        if (i < count - 1) out[pos++] = ',';
    }
    out[pos++] = ']';
    out[pos]   = '\0';
    send_json(fd, 200, out);
    free(out);
    db_free_posts(posts);
}

static void handle_get_thread(int fd, kewld_server_t *srv, int64_t thread_id) {
    if (!db_thread_exists(srv->db, thread_id)) {
        send_err(fd, 404, "thread not found"); return;
    }
    kewld_post_t *posts = NULL;
    int count = 0;
    if (db_get_thread(srv->db, thread_id, &posts, &count) != 0) {
        send_err(fd, 500, "database error"); return;
    }
    size_t bufsz = (size_t)(count + 1) * (KEWLD_MAX_CONTENT * 3);
    char *out = malloc(bufsz);
    size_t pos = 0;
    out[pos++] = '[';
    for (int i = 0; i < count; i++) {
        char pbuf[KEWLD_MAX_CONTENT * 3];
        post_to_json(&posts[i], pbuf, sizeof(pbuf));
        size_t plen = strlen(pbuf);
        memcpy(out + pos, pbuf, plen); pos += plen;
        if (i < count - 1) out[pos++] = ',';
    }
    out[pos++] = ']';
    out[pos]   = '\0';
    send_json(fd, 200, out);
    free(out);
    db_free_posts(posts);
}

static void handle_post_thread(int fd, kewld_server_t *srv, const char *body) {
    if (!body) { send_err(fd, 400, "empty body"); return; }
    kewld_post_t p; memset(&p, 0, sizeof(p));
    parse_form_field(body, "name",    p.name,    sizeof(p.name));
    parse_form_field(body, "subject", p.subject, sizeof(p.subject));
    parse_form_field(body, "content", p.content, sizeof(p.content));
    if (p.name[0] == '\0') strncpy(p.name, "Anonymous", sizeof(p.name)-1);
    if (p.content[0] == '\0') { send_err(fd, 400, "content required"); return; }
    p.thread_id = 0;
    if (db_insert_post(srv->db, &p) != 0) { send_err(fd, 500, "database error"); return; }
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"id\":%lld,\"thread_id\":%lld}", (long long)p.id, (long long)p.id);
    send_json(fd, 201, buf);
}

static void handle_post_reply(int fd, kewld_server_t *srv, int64_t thread_id, const char *body) {
    if (!db_thread_exists(srv->db, thread_id)) {
        send_err(fd, 404, "thread not found"); return;
    }
    if (!body) { send_err(fd, 400, "empty body"); return; }
    kewld_post_t p; memset(&p, 0, sizeof(p));
    parse_form_field(body, "name",    p.name,    sizeof(p.name));
    parse_form_field(body, "subject", p.subject, sizeof(p.subject));
    parse_form_field(body, "content", p.content, sizeof(p.content));
    if (p.name[0] == '\0') strncpy(p.name, "Anonymous", sizeof(p.name)-1);
    if (p.content[0] == '\0') { send_err(fd, 400, "content required"); return; }
    char sage_val[8];
    parse_form_field(body, "sage", sage_val, sizeof(sage_val));
    p.sage = (sage_val[0] == '1' || strcasecmp(sage_val, "true") == 0) ? 1 : 0;
    p.thread_id = thread_id;
    if (db_insert_post(srv->db, &p) != 0) { send_err(fd, 500, "database error"); return; }
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"id\":%lld,\"thread_id\":%lld}", (long long)p.id, (long long)thread_id);
    send_json(fd, 201, buf);
}

static void handle_get_image(int fd, kewld_server_t *srv, const char *hash, const char *ext) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/images/%s%s", srv->cfg->data_dir, hash, ext);
    int imgfd = open(path, O_RDONLY);
    if (imgfd < 0) { send_err(fd, 404, "image not found"); return; }
    struct stat st;
    fstat(imgfd, &st);
    const char *ctype = "application/octet-stream";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) ctype = "image/jpeg";
    else if (strcmp(ext, ".png") == 0) ctype = "image/png";
    else if (strcmp(ext, ".gif") == 0) ctype = "image/gif";
    else if (strcmp(ext, ".webp") == 0) ctype = "image/webp";
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        ctype, (long long)st.st_size);
    write(fd, hdr, hlen);
    char fbuf[8192]; ssize_t nr;
    while ((nr = read(imgfd, fbuf, sizeof(fbuf))) > 0) write(fd, fbuf, nr);
    close(imgfd);
}

static void handle_health(int fd, kewld_server_t *srv) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\",\"tag\":\"/%s/\",\"version\":\"%s\"}",
        srv->cfg->tag, KEWLD_VERSION);
    send_json(fd, 200, buf);
}

static void dispatch(int fd, kewld_server_t *srv, const char *method, const char *raw_path, const char *body) {
    char path[1024];
    char *q = strchr(raw_path, '?');
    const char *query = q ? q + 1 : NULL;
    strncpy(path, raw_path, sizeof(path)-1);
    if (q) path[q - raw_path] = '\0';
    if (strcmp(path, "/health") == 0) { handle_health(fd, srv); return; }
    if (strcmp(path, "/api/board") == 0 && strcmp(method,"GET")==0) {
        handle_get_board_info(fd, srv); return;
    }
    char tag_threads[128], tag_post[128];
    snprintf(tag_threads, sizeof(tag_threads), "/api/%s/threads", srv->cfg->tag);
    snprintf(tag_post,    sizeof(tag_post),    "/api/%s/post",    srv->cfg->tag);
    if (strcmp(path, tag_threads) == 0) {
        if (strcmp(method,"GET")==0) { handle_get_threads(fd, srv, query); return; }
        send_err(fd, 405, "method not allowed"); return;
    }
    if (strcmp(path, tag_post) == 0) {
        if (strcmp(method,"POST")==0) { handle_post_thread(fd, srv, body); return; }
        send_err(fd, 405, "method not allowed"); return;
    }
    char thread_prefix[128];
    snprintf(thread_prefix, sizeof(thread_prefix), "/api/%s/thread/", srv->cfg->tag);
    if (strncmp(path, thread_prefix, strlen(thread_prefix)) == 0) {
        const char *rest = path + strlen(thread_prefix);
        char *slash = strchr(rest, '/');
        if (!slash) {
            int64_t tid = (int64_t)atoll(rest);
            if (strcmp(method,"GET")==0) { handle_get_thread(fd, srv, tid); return; }
            send_err(fd, 405, "method not allowed"); return;
        }
        int64_t tid = (int64_t)atoll(rest);
        if (strcmp(slash, "/post") == 0 && strcmp(method,"POST")==0) {
            handle_post_reply(fd, srv, tid, body); return;
        }
        send_err(fd, 404, "not found"); return;
    }
    char img_prefix[128];
    snprintf(img_prefix, sizeof(img_prefix), "/api/%s/image/", srv->cfg->tag);
    if (strncmp(path, img_prefix, strlen(img_prefix)) == 0 && strcmp(method,"GET")==0) {
        const char *rest = path + strlen(img_prefix);
        char hash[65] = {0}, ext[8] = {0};
        const char *dot = strrchr(rest, '.');
        if (dot) {
            size_t hlen = (size_t)(dot - rest);
            if (hlen > 64) hlen = 64;
            memcpy(hash, rest, hlen);
            strncpy(ext, dot, sizeof(ext)-1);
        } else {
            strncpy(hash, rest, 64);
        }
        handle_get_image(fd, srv, hash, ext); return;
    }
    send_err(fd, 404, "not found");
}

static void *conn_thread(void *arg) {
    conn_ctx_t *ctx = (conn_ctx_t *)arg;
    char *method = NULL, *path = NULL, *body = NULL;
    size_t blen = 0;
    if (read_request(ctx->client_fd, &method, &path, &body, &blen) == 0)
        dispatch(ctx->client_fd, ctx->srv, method, path, body);
    free(method); free(path); free(body);
    close(ctx->client_fd);
    free(ctx);
    return NULL;
}

kewld_server_t *http_server_create(kewld_config_t *cfg, sqlite3 *db) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { log_err("socket: %s", strerror(errno)); return NULL; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg->http_port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_err("bind: %s", strerror(errno)); close(fd); return NULL;
    }
    if (listen(fd, 64) != 0) {
        log_err("listen: %s", strerror(errno)); close(fd); return NULL;
    }
    kewld_server_t *srv = malloc(sizeof(kewld_server_t));
    srv->cfg = cfg; srv->db = db; srv->fd = fd;
    return srv;
}

int http_server_run(kewld_server_t *srv) {
    struct sockaddr_in client_addr;
    socklen_t clen = sizeof(client_addr);
    while (1) {
        int cfd = accept(srv->fd, (struct sockaddr *)&client_addr, &clen);
        if (cfd < 0) { if (errno == EINTR) continue; break; }
        conn_ctx_t *ctx = malloc(sizeof(conn_ctx_t));
        ctx->client_fd = cfd; ctx->srv = srv;
        pthread_t tid;
        if (pthread_create(&tid, NULL, conn_thread, ctx) != 0) {
            close(cfd); free(ctx);
        } else {
            pthread_detach(tid);
        }
    }
    return 0;
}

void http_server_destroy(kewld_server_t *srv) { close(srv->fd); free(srv); }
