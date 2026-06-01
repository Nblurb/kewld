/*
 * kewld — The Kewl Onion Daemon
 * admin.c — LOCAL-ONLY media approval interface
 *
 * SECURITY:  This server binds ONLY to 127.0.0.1.
 *            It is NEVER exposed through Tor or any other network interface.
 *            All endpoints require a Bearer token (admin_token in config).
 *
 * Purpose:   Operators review submitted media before it becomes visible to
 *            board users, providing a safeguard against illegal content.
 */

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
#include "kewld.h"

struct kewld_admin_server {
    kewld_config_t *cfg;
    sqlite3        *db;
    int             fd;
};

typedef struct {
    int                   client_fd;
    kewld_admin_server_t *srv;
} admin_conn_ctx_t;

/* ── response helpers ── */
static void admin_send(int fd, int status, const char *ctype,
                       const char *body, size_t blen) {
    char hdr[512];
    const char *reason =
        (status==200)?"OK":(status==201)?"Created":
        (status==400)?"Bad Request":(status==401)?"Unauthorized":
        (status==403)?"Forbidden":(status==404)?"Not Found":
        (status==405)?"Method Not Allowed":"Internal Server Error";
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "X-Frame-Options: DENY\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n"
        "\r\n", status, reason, ctype, blen);
    write(fd, hdr, hlen);
    if (blen > 0) write(fd, body, blen);
}

static void admin_json(int fd, int status, const char *json) {
    admin_send(fd, status, "application/json", json, strlen(json));
}

static void admin_err(int fd, int status, const char *msg) {
    char buf[256], esc[512];
    json_escape(msg, esc, sizeof(esc));
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esc);
    admin_json(fd, status, buf);
}

/* ── request reader (reuses same approach as http.c) ── */
static int admin_read_request(int fd,
                               char **method_out, char **path_out,
                               char **headers_out,
                               char **body_out, size_t *blen_out) {
    size_t cap = 65536;
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t total = 0;
    ssize_t n;
    while (total < cap-1) {
        n = read(fd, buf+total, cap-1-total);
        if (n<=0) break;
        total += n;
        if (memmem(buf, total, "\r\n\r\n", 4)) break;
    }
    buf[total] = '\0';
    char *hdr_end = memmem(buf, total, "\r\n\r\n", 4);
    if (!hdr_end) { free(buf); return -1; }
    size_t hlen = (size_t)(hdr_end - buf);
    char *hdrs = malloc(hlen+1);
    if (!hdrs) { free(buf); return -1; }
    memcpy(hdrs, buf, hlen); hdrs[hlen]='\0';

    char *le = strstr(hdrs, "\r\n");
    char req[512];
    if (le) { size_t l=(size_t)(le-hdrs); if(l>=sizeof(req)) l=sizeof(req)-1; memcpy(req,hdrs,l); req[l]='\0'; }
    else     { strncpy(req,hdrs,sizeof(req)-1); req[sizeof(req)-1]='\0'; }
    char *s1=strchr(req,' ');
    if (!s1) { free(buf); free(hdrs); return -1; }
    *s1='\0';
    char *s2=strchr(s1+1,' ');
    if (s2) *s2='\0';
    *method_out  = strdup(req);
    *path_out    = strdup(s1+1);
    *headers_out = hdrs;

    size_t cl=0;
    char *clp=strcasestr(hdrs,"content-length:");
    if (clp) cl=(size_t)atol(clp+15);
    if (cl>0 && cl<cap) {
        *body_out=malloc(cl+1);
        if (!*body_out) { free(buf); free(*method_out); free(*path_out); free(hdrs); return -1; }
        size_t got=total-hlen-4; if(got>cl) got=cl;
        memcpy(*body_out, buf+hlen+4, got);
        size_t rem=cl-got;
        while (rem>0) {
            n=read(fd,*body_out+(cl-rem),rem);
            if(n<=0) break;
            rem-=(size_t)n;
        }
        (*body_out)[cl]='\0';
        *blen_out=cl;
    } else { *body_out=NULL; *blen_out=0; }
    free(buf);
    return 0;
}

/* ── Bearer token check ── */
static int check_auth(const char *headers, const char *token) {
    if (!token || !token[0]) return 1;  /* no token configured → open (not recommended) */
    char *auth = strcasestr(headers, "authorization:");
    if (!auth) return 0;
    auth += 14;
    while (*auth==' ') auth++;
    if (strncasecmp(auth,"bearer ",7)!=0) return 0;
    auth += 7;
    while (*auth==' ') auth++;
    size_t tlen = strlen(token);
    return strncmp(auth, token, tlen)==0 && (auth[tlen]=='\r'||auth[tlen]=='\n'||auth[tlen]=='\0'||auth[tlen]==' ');
}

/* ── HTML helpers ── */
static const char *ADMIN_CSS =
    "body{font-family:monospace;background:#111;color:#eee;margin:2rem}"
    "h1{color:#f90}h2{color:#aaa;font-size:1rem}"
    "table{border-collapse:collapse;width:100%}"
    "th{background:#222;padding:8px;text-align:left;border:1px solid #333}"
    "td{padding:6px 8px;border:1px solid #333;vertical-align:top}"
    "tr:hover td{background:#1a1a1a}"
    ".approve{background:#2a6;color:#fff;border:none;padding:4px 12px;cursor:pointer;border-radius:3px}"
    ".reject{background:#a22;color:#fff;border:none;padding:4px 12px;cursor:pointer;border-radius:3px}"
    ".badge-p{background:#555;color:#fff;padding:2px 6px;border-radius:3px;font-size:.75rem}"
    ".none{color:#555;font-style:italic}"
    "img,video{max-width:180px;max-height:120px;display:block}"
    "form{display:inline}";

/* Serve admin dashboard page */
static void handle_admin_index(int fd, kewld_admin_server_t *srv) {
    kewld_post_t *posts = NULL; int count = 0;
    db_get_pending_media(srv->db, &posts, &count);

    /* Build HTML dynamically */
    size_t cap = 65536 + (size_t)count * 2048;
    char *html = malloc(cap);
    size_t pos = 0;

    pos += snprintf(html+pos, cap-pos,
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'><title>kewld — Media Approval</title>"
        "<style>%s</style></head><body>"
        "<h1>&#x1F6E1; kewld Media Approval Queue</h1>"
        "<h2>LOCAL ADMIN INTERFACE — NOT FOR NETWORK EXPOSURE</h2>",
        ADMIN_CSS);

    if (count == 0) {
        pos += snprintf(html+pos, cap-pos,
            "<p class='none'>No media pending approval.</p>");
    } else {
        pos += snprintf(html+pos, cap-pos,
            "<p>%d item(s) awaiting review.</p>"
            "<table><tr>"
            "<th>Post ID</th><th>Type</th><th>Media</th>"
            "<th>Content</th><th>Submitted</th><th>Actions</th>"
            "</tr>", count);

        for (int i = 0; i < count; i++) {
            kewld_post_t *p = &posts[i];
            int is_thread = (p->thread_id == p->id || p->thread_id == 0);
            char time_buf[64];
            struct tm *tm_info = gmtime(&p->created_at);
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M UTC", tm_info);

            /* media preview element */
            char preview[512];
            const char *ext = p->media_ext;
            int is_video = (strcmp(ext,".mp4")==0||strcmp(ext,".webm")==0||
                            strcmp(ext,".mov")==0||strcmp(ext,".avi")==0||
                            strcmp(ext,".mkv")==0);
            if (is_video) {
                snprintf(preview, sizeof(preview),
                    "<video controls src='/admin/media/%s%s'></video>",
                    p->media_hash, ext);
            } else {
                snprintf(preview, sizeof(preview),
                    "<img src='/admin/media/%s%s' alt='pending media'>",
                    p->media_hash, ext);
            }

            /* truncate content for display */
            char content_disp[128];
            strncpy(content_disp, p->content, 120);
            content_disp[120] = '\0';
            if (strlen(p->content) > 120) strcat(content_disp, "...");

            pos += snprintf(html+pos, cap-pos,
                "<tr>"
                "<td>%lld</td>"
                "<td>%s</td>"
                "<td>%s</td>"
                "<td>%s</td>"
                "<td>%s</td>"
                "<td>"
                  "<form method='POST' action='/admin/approve'>"
                    "<input type='hidden' name='id' value='%lld'>"
                    "<input type='hidden' name='is_thread' value='%d'>"
                    "<button class='approve' type='submit'>&#x2714; Approve</button>"
                  "</form> "
                  "<form method='POST' action='/admin/reject'>"
                    "<input type='hidden' name='id' value='%lld'>"
                    "<input type='hidden' name='is_thread' value='%d'>"
                    "<button class='reject' type='submit'>&#x2718; Reject</button>"
                  "</form>"
                "</td>"
                "</tr>",
                (long long)p->id,
                is_thread ? "Thread OP" : "Reply",
                preview,
                content_disp,
                time_buf,
                (long long)p->id, is_thread,
                (long long)p->id, is_thread);
        }
        pos += snprintf(html+pos, cap-pos, "</table>");
    }
    pos += snprintf(html+pos, cap-pos, "</body></html>");

    admin_send(fd, 200, "text/html; charset=utf-8", html, pos);
    free(html);
    db_free_posts(posts);
}

/* Parse a simple urlencoded body for id and is_thread */
static void parse_approval_body(const char *body, int64_t *id_out, int *is_thread_out) {
    *id_out = 0; *is_thread_out = 0;
    if (!body) return;
    const char *p = strstr(body, "id=");
    if (p) *id_out = (int64_t)atoll(p+3);
    const char *t = strstr(body, "is_thread=");
    if (t) *is_thread_out = atoi(t+10);
}

static void handle_admin_action(int fd, kewld_admin_server_t *srv,
                                 const char *body, int status) {
    int64_t id = 0; int is_thread = 0;
    parse_approval_body(body, &id, &is_thread);
    if (id <= 0) { admin_err(fd, 400, "invalid id"); return; }
    db_set_media_approval(srv->db, id, is_thread, status);
    /* Redirect back to dashboard */
    const char *redir =
        "HTTP/1.1 303 See Other\r\nLocation: /admin/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    write(fd, redir, strlen(redir));
}

/* Serve media file for admin preview — bypass approval check */
static void handle_admin_media(int fd, kewld_admin_server_t *srv,
                                const char *hash, const char *ext) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/media/%s%s", srv->cfg->data_dir, hash, ext);
    int imgfd = open(path, O_RDONLY);
    if (imgfd < 0) { admin_err(fd, 404, "not found"); return; }
    struct stat st; fstat(imgfd, &st);

    const char *ctype = "application/octet-stream";
    if (strcmp(ext,".jpg")==0||strcmp(ext,".jpeg")==0) ctype="image/jpeg";
    else if (strcmp(ext,".png")==0)  ctype="image/png";
    else if (strcmp(ext,".gif")==0)  ctype="image/gif";
    else if (strcmp(ext,".webp")==0) ctype="image/webp";
    else if (strcmp(ext,".mp4")==0)  ctype="video/mp4";
    else if (strcmp(ext,".webm")==0) ctype="video/webm";
    else if (strcmp(ext,".mov")==0)  ctype="video/quicktime";
    else if (strcmp(ext,".avi")==0)  ctype="video/x-msvideo";
    else if (strcmp(ext,".mkv")==0)  ctype="video/x-matroska";

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        ctype, (long long)st.st_size);
    write(fd, hdr, hlen);
    char fbuf[8192]; ssize_t nr;
    while ((nr = read(imgfd, fbuf, sizeof(fbuf))) > 0) write(fd, fbuf, nr);
    close(imgfd);
}

/* ── dispatcher ── */
static void admin_dispatch(int fd, kewld_admin_server_t *srv,
                            const char *method, const char *raw_path,
                            const char *headers,
                            const char *body) {
    /* Auth gate */
    if (!check_auth(headers, srv->cfg->admin_token)) {
        const char *unauth =
            "HTTP/1.1 401 Unauthorized\r\n"
            "WWW-Authenticate: Bearer realm=\"kewld-admin\"\r\n"
            "Content-Length: 29\r\nConnection: close\r\n\r\n"
            "{\"error\":\"unauthorized\"}";
        write(fd, unauth, strlen(unauth));
        return;
    }

    /* Strip query string */
    char path[512];
    strncpy(path, raw_path, sizeof(path)-1); path[sizeof(path)-1]='\0';
    char *q = strchr(path, '?'); if (q) *q='\0';

    if (strncmp(path,"/admin/",7)==0 || strcmp(path,"/admin")==0) {
        if (strcmp(path,"/admin/")==0 || strcmp(path,"/admin")==0) {
            if (strcmp(method,"GET")==0) { handle_admin_index(fd, srv); return; }
        }
        if (strcmp(path,"/admin/approve")==0 && strcmp(method,"POST")==0) {
            handle_admin_action(fd, srv, body, KEWLD_MEDIA_APPROVED); return;
        }
        if (strcmp(path,"/admin/reject")==0 && strcmp(method,"POST")==0) {
            handle_admin_action(fd, srv, body, KEWLD_MEDIA_REJECTED); return;
        }
        /* Media preview for admin */
        if (strncmp(path,"/admin/media/",13)==0 && strcmp(method,"GET")==0) {
            const char *mp = path+13;
            char hash[65]={0}, ext[8]={0};
            const char *dot=strrchr(mp,'.');
            if (dot) {
                size_t hl=(size_t)(dot-mp); if(hl>64) hl=64;
                memcpy(hash,mp,hl);
                strncpy(ext,dot,sizeof(ext)-1);
            } else strncpy(hash,mp,64);
            handle_admin_media(fd, srv, hash, ext); return;
        }
    }

    admin_err(fd, 404, "not found");
}

/* ── connection thread ── */
static void *admin_conn_thread(void *arg) {
    admin_conn_ctx_t *ctx = (admin_conn_ctx_t *)arg;
    char *method=NULL, *path=NULL, *headers=NULL, *body=NULL;
    size_t blen=0;
    if (admin_read_request(ctx->client_fd, &method, &path, &headers,
                           &body, &blen) == 0) {
        admin_dispatch(ctx->client_fd, ctx->srv,
                       method, path, headers, body);
    }
    free(method); free(path); free(headers); free(body);
    close(ctx->client_fd); free(ctx);
    return NULL;
}

/* ── public API ── */
kewld_admin_server_t *admin_server_create(kewld_config_t *cfg, sqlite3 *db) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd<0) { log_err("admin socket: %s", strerror(errno)); return NULL; }
    int opt=1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET;
    addr.sin_port=htons(cfg->admin_port);
    /* CRITICAL: bind ONLY to loopback — never exposed to network */
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(fd,(struct sockaddr*)&addr,sizeof(addr))!=0) {
        log_err("admin bind: %s", strerror(errno)); close(fd); return NULL;
    }
    if (listen(fd,16)!=0) { log_err("admin listen: %s",strerror(errno)); close(fd); return NULL; }
    kewld_admin_server_t *srv = malloc(sizeof(kewld_admin_server_t));
    srv->cfg=cfg; srv->db=db; srv->fd=fd;
    log_info("admin UI listening on 127.0.0.1:%d (local only — NOT on Tor)", cfg->admin_port);
    return srv;
}

int admin_server_run(kewld_admin_server_t *srv) {
    struct sockaddr_in ca; socklen_t clen=sizeof(ca);
    while (1) {
        int cfd=accept(srv->fd,(struct sockaddr*)&ca,&clen);
        if (cfd<0) { if(errno==EINTR) continue; break; }
        /* Double-check the peer is local (belt-and-suspenders) */
        if (ca.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            log_warn("admin: rejected non-loopback connection from %s",
                     inet_ntoa(ca.sin_addr));
            close(cfd); continue;
        }
        admin_conn_ctx_t *ctx=malloc(sizeof(admin_conn_ctx_t));
        ctx->client_fd=cfd; ctx->srv=srv;
        pthread_t tid;
        if (pthread_create(&tid,NULL,admin_conn_thread,ctx)!=0) { close(cfd); free(ctx); }
        else pthread_detach(tid);
    }
    return 0;
}

void admin_server_destroy(kewld_admin_server_t *srv) { close(srv->fd); free(srv); }
