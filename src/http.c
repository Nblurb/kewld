/*
 * kewld — The Kewl Onion Daemon
 * http.c — public (Tor-facing) HTTP server
 *
 * Media is held for admin approval.  Posts with pending or rejected media
 * receive a synthetic "pending" media_hash so the client shows a placeholder.
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

/* Placeholder hash sent to clients when media is not yet approved */
#define PENDING_MEDIA_HASH  "pending"
#define PENDING_MEDIA_EXT   ".png"

struct kewld_server {
    kewld_config_t *cfg;
    sqlite3        *db;
    int             fd;
};

typedef struct {
    int              client_fd;
    kewld_server_t  *srv;
} conn_ctx_t;

/* ── multipart field ── */
typedef struct {
    char  name[128];
    char  filename[256];
    char  content_type[128];
    unsigned char *data;
    size_t        data_len;
} mp_part_t;

#define MAX_MP_PARTS 16

/* ── response helpers ── */
static void send_response(int fd, int status, const char *ctype,
                          const char *body, size_t blen) {
    char hdr[512];
    const char *reason =
        (status == 200) ? "OK"                  :
        (status == 201) ? "Created"             :
        (status == 400) ? "Bad Request"         :
        (status == 404) ? "Not Found"           :
        (status == 405) ? "Method Not Allowed"  :
        (status == 413) ? "Payload Too Large"   : "Internal Server Error";
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
    char buf[256], esc[512];
    json_escape(msg, esc, sizeof(esc));
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esc);
    send_json(fd, status, buf);
}

/* ── request reader ── */
static int read_request(int fd, char **method_out, char **path_out,
                        char **headers_out,
                        unsigned char **body_out, size_t *blen_out) {
    size_t cap = KEWLD_MAX_BODY_BYTES;
    unsigned char *buf = malloc(cap);
    if (!buf) return -1;
    size_t total = 0;
    ssize_t n;
    while (total < cap - 1) {
        n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += n;
        if (memmem(buf, total, "\r\n\r\n", 4)) break;
    }
    buf[total] = '\0';
    unsigned char *header_end = memmem(buf, total, (const void*)"\r\n\r\n", 4);
    if (!header_end) { free(buf); return -1; }
    size_t header_len = (size_t)(header_end - buf);
    char *hdrs = malloc(header_len + 1);
    if (!hdrs) { free(buf); return -1; }
    memcpy(hdrs, buf, header_len); hdrs[header_len] = '\0';

    char *line_end = strstr(hdrs, "\r\n");
    char req_line[512];
    if (line_end) {
        size_t ll = (size_t)(line_end - hdrs);
        if (ll >= sizeof(req_line)) ll = sizeof(req_line)-1;
        memcpy(req_line, hdrs, ll); req_line[ll] = '\0';
    } else {
        strncpy(req_line, hdrs, sizeof(req_line)-1);
        req_line[sizeof(req_line)-1] = '\0';
    }
    char *sp1 = strchr(req_line, ' ');
    if (!sp1) { free(buf); free(hdrs); return -1; }
    *sp1 = '\0';
    char *sp2 = strchr(sp1+1, ' ');
    if (sp2) *sp2 = '\0';
    *method_out  = strdup(req_line);
    *path_out    = strdup(sp1+1);
    *headers_out = hdrs;

    size_t content_length = 0;
    char *cl = strcasestr(hdrs, "content-length:");
    if (cl) content_length = (size_t)atol(cl + 15);

    if (content_length > 0) {
        if (content_length > cap - 1) content_length = cap - 1;
        *body_out = malloc(content_length + 1);
        if (!*body_out) { free(buf); free(*method_out); free(*path_out); free(hdrs); return -1; }
        size_t body_in_buf = total - header_len - 4;
        if (body_in_buf > content_length) body_in_buf = content_length;
        memcpy(*body_out, buf + header_len + 4, body_in_buf);
        size_t remaining = content_length - body_in_buf;
        while (remaining > 0) {
            n = read(fd, *body_out + (content_length - remaining), remaining);
            if (n <= 0) break;
            remaining -= (size_t)n;
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

/* ── urlencoded form field parser ── */
static char *parse_form_field(const char *body, const char *key,
                               char *out, size_t outlen) {
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *end = strchr(p, '&');
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            if (vlen >= outlen) vlen = outlen - 1;
            char tmp[KEWLD_MAX_BODY_BYTES];
            if (vlen >= sizeof(tmp)) vlen = sizeof(tmp)-1;
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

/* ── multipart parser ── */
static int parse_multipart(const unsigned char *body, size_t body_len,
                            const char *boundary,
                            mp_part_t *parts, int max_parts) {
    char delim[256];
    snprintf(delim, sizeof(delim), "\r\n--%s", boundary);
    size_t dlen = strlen(delim);

    char first[256];
    snprintf(first, sizeof(first), "--%s\r\n", boundary);
    size_t flen = strlen(first);

    const unsigned char *p = memmem(body, body_len, first, flen);
    if (!p) return 0;
    p += flen;
    size_t remaining = body_len - (size_t)(p - body);

    int count = 0;
    while (count < max_parts && remaining > 0) {
        const unsigned char *hdr_end = memmem(p, remaining, (const void*)"\r\n\r\n", 4);
        if (!hdr_end) break;
        size_t hdr_len = (size_t)(hdr_end - p);
        char *hdr = malloc(hdr_len + 1);
        if (!hdr) break;
        memcpy(hdr, p, hdr_len); hdr[hdr_len] = '\0';

        mp_part_t *part = &parts[count];
        memset(part, 0, sizeof(*part));

        char *cd = strcasestr(hdr, "content-disposition:");
        if (cd) {
            char *nm = strstr(cd, "name=\"");
            if (nm) {
                nm += 6;
                char *eq = strchr(nm, '"');
                if (eq) { size_t l=(size_t)(eq-nm); if(l>=sizeof(part->name)) l=sizeof(part->name)-1; memcpy(part->name,nm,l); part->name[l]='\0'; }
            }
            char *fn = strstr(cd, "filename=\"");
            if (fn) {
                fn += 10;
                char *eq = strchr(fn, '"');
                if (eq) { size_t l=(size_t)(eq-fn); if(l>=sizeof(part->filename)) l=sizeof(part->filename)-1; memcpy(part->filename,fn,l); part->filename[l]='\0'; }
            }
        }
        char *ct = strcasestr(hdr, "content-type:");
        if (ct) {
            ct += 13;
            while (*ct == ' ') ct++;
            char *nl = strstr(ct, "\r\n");
            size_t l = nl ? (size_t)(nl-ct) : strlen(ct);
            if (l >= sizeof(part->content_type)) l = sizeof(part->content_type)-1;
            memcpy(part->content_type, ct, l); part->content_type[l] = '\0';
        }
        free(hdr);

        const unsigned char *data_start = hdr_end + 4;
        size_t data_remaining = body_len - (size_t)(data_start - body);
        const unsigned char *next_delim = memmem(data_start, data_remaining, delim, dlen);
        size_t data_len = next_delim ? (size_t)(next_delim - data_start) : data_remaining;

        part->data = malloc(data_len + 1);
        if (!part->data) break;
        memcpy(part->data, data_start, data_len);
        part->data[data_len] = '\0';
        part->data_len = data_len;
        count++;

        if (!next_delim) break;
        p = next_delim + dlen;
        remaining = body_len - (size_t)(p - body);
        if (remaining >= 2 && p[0] == '-' && p[1] == '-') break;
        if (remaining >= 2) { p += 2; remaining -= 2; }
        else break;
    }
    return count;
}

static void free_mp_parts(mp_part_t *parts, int count) {
    for (int i = 0; i < count; i++) free(parts[i].data);
}

static int extract_boundary(const char *headers, char *boundary_out, size_t outlen) {
    char *ct = strcasestr(headers, "content-type:");
    if (!ct) return -1;
    char *b = strstr(ct, "boundary=");
    if (!b) return -1;
    b += 9;
    if (*b == '"') {
        b++;
        char *eq = strchr(b, '"');
        size_t l = eq ? (size_t)(eq-b) : strlen(b);
        if (l >= outlen) l = outlen-1;
        memcpy(boundary_out, b, l); boundary_out[l] = '\0';
    } else {
        size_t l = strcspn(b, "\r\n ;");
        if (l >= outlen) l = outlen-1;
        memcpy(boundary_out, b, l); boundary_out[l] = '\0';
    }
    return boundary_out[0] ? 0 : -1;
}

/* Derive media extension from content-type or filename.
   Supports images (jpg/png/gif/webp) and videos (mp4/webm/mov/avi/mkv). */
static void media_ext_from_ct(const char *ct, const char *filename, char *ext_out) {
    ext_out[0] = '\0';
    /* Images */
    if (strstr(ct,"jpeg")||strstr(ct,"jpg"))  { strcpy(ext_out,".jpg");  return; }
    if (strstr(ct,"png"))                      { strcpy(ext_out,".png");  return; }
    if (strstr(ct,"gif"))                      { strcpy(ext_out,".gif");  return; }
    if (strstr(ct,"webp"))                     { strcpy(ext_out,".webp"); return; }
    /* Videos */
    if (strstr(ct,"mp4")||strstr(ct,"mpeg4"))  { strcpy(ext_out,".mp4");  return; }
    if (strstr(ct,"webm"))                     { strcpy(ext_out,".webm"); return; }
    if (strstr(ct,"quicktime"))                { strcpy(ext_out,".mov");  return; }
    if (strstr(ct,"x-msvideo"))                { strcpy(ext_out,".avi");  return; }
    if (strstr(ct,"x-matroska"))               { strcpy(ext_out,".mkv");  return; }
    /* Fall back to filename extension */
    if (filename && *filename) {
        const char *dot = strrchr(filename, '.');
        if (dot && strlen(dot) <= 5) {
            strncpy(ext_out, dot, 7); ext_out[7] = '\0';
            for (char *q = ext_out; *q; q++) if (*q>='A'&&*q<='Z') *q+=32;
        }
    }
}

static int is_allowed_ext(const char *ext) {
    return (strcmp(ext,".jpg")==0  || strcmp(ext,".jpeg")==0 ||
            strcmp(ext,".png")==0  || strcmp(ext,".gif")==0  ||
            strcmp(ext,".webp")==0 || strcmp(ext,".mp4")==0  ||
            strcmp(ext,".webm")==0 || strcmp(ext,".mov")==0  ||
            strcmp(ext,".avi")==0  || strcmp(ext,".mkv")==0);
}

/* Save media bytes to data_dir/media/<hash><ext> */
static int save_media(const char *data_dir,
                      const unsigned char *data, size_t data_len,
                      const char *ext,
                      char *hash_out) {
    if (sha256_buf(data, data_len, hash_out) != 0) return -1;
    char path[1024];
    snprintf(path, sizeof(path), "%s/media/%s%s", data_dir, hash_out, ext);
    struct stat st;
    if (stat(path, &st) == 0) return 0;  /* already exists (dedup) */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return -1;
    size_t written = 0;
    while (written < data_len) {
        ssize_t w = write(fd, data + written, data_len - written);
        if (w < 0) { close(fd); unlink(path); return -1; }
        written += (size_t)w;
    }
    close(fd);
    return 0;
}

/* ── JSON serialisation ──
   Media with pending/rejected approval returns the PENDING_MEDIA_HASH
   placeholder so the client can display a "waiting for approval" image. */
static char *post_to_json(const kewld_post_t *p, char *buf, size_t buflen) {
    char name[256], subject[512], content[KEWLD_MAX_CONTENT*2];
    char mhash[130], mext[16], pgpsig[KEWLD_MAX_PGP_SIG*2];
    json_escape(p->name,    name,    sizeof(name));
    json_escape(p->subject, subject, sizeof(subject));
    json_escape(p->content, content, sizeof(content));
    json_escape(p->pgp_sig, pgpsig,  sizeof(pgpsig));

    /* Mask unapproved media */
    const char *visible_hash = p->media_hash;
    const char *visible_ext  = p->media_ext;
    if (p->media_hash[0] != '\0' && p->media_approved != KEWLD_MEDIA_APPROVED) {
        visible_hash = PENDING_MEDIA_HASH;
        visible_ext  = PENDING_MEDIA_EXT;
    }
    json_escape(visible_hash, mhash, sizeof(mhash));
    json_escape(visible_ext,  mext,  sizeof(mext));

    snprintf(buf, buflen,
        "{\"id\":%lld,\"thread_id\":%lld,\"name\":\"%s\",\"subject\":\"%s\","
        "\"content\":\"%s\",\"media_hash\":\"%s\",\"media_ext\":\"%s\","
        "\"media_pending\":%d,"
        "\"pgp_sig\":\"%s\","
        "\"created_at\":%ld,\"reply_count\":%lld,\"last_bump\":%ld,\"sage\":%d}",
        (long long)p->id, (long long)p->thread_id,
        name, subject, content, mhash, mext,
        (p->media_hash[0]!='\0' && p->media_approved!=KEWLD_MEDIA_APPROVED) ? 1 : 0,
        pgpsig,
        (long)p->created_at, (long long)p->reply_count, (long)p->last_bump, p->sage);
    return buf;
}

/* ── GET /api/board ── */
static void handle_get_board_info(int fd, kewld_server_t *srv) {
    char buf[1024], tag[64], title[256], desc[1024];
    json_escape(srv->cfg->tag,   tag,   sizeof(tag));
    json_escape(srv->cfg->title, title, sizeof(title));
    json_escape(srv->cfg->desc,  desc,  sizeof(desc));
    snprintf(buf, sizeof(buf),
        "{\"tag\":\"/%s/\",\"title\":\"%s\",\"desc\":\"%s\","
        "\"nsfw\":%d,\"allow_media\":%d,\"version\":\"%s\"}",
        tag, title, desc, srv->cfg->nsfw, srv->cfg->allow_media, KEWLD_VERSION);
    send_json(fd, 200, buf);
}

/* ── GET /api/<tag>/threads ── */
static void handle_get_threads(int fd, kewld_server_t *srv, const char *query) {
    int page = 0;
    if (query) { const char *p = strstr(query, "page="); if (p) page = atoi(p+5); }
    kewld_post_t *posts = NULL; int count = 0;
    if (db_get_threads(srv->db, page, &posts, &count) != 0) {
        send_err(fd, 500, "database error"); return;
    }
    size_t bufsz = (size_t)(count+1) * (KEWLD_MAX_CONTENT*3 + KEWLD_MAX_PGP_SIG*2);
    char *out = malloc(bufsz);
    size_t pos = 0; out[pos++] = '[';
    for (int i = 0; i < count; i++) {
        char pbuf[KEWLD_MAX_CONTENT*3 + KEWLD_MAX_PGP_SIG*2];
        post_to_json(&posts[i], pbuf, sizeof(pbuf));
        size_t plen = strlen(pbuf);
        memcpy(out+pos, pbuf, plen); pos += plen;
        if (i < count-1) out[pos++] = ',';
    }
    out[pos++] = ']'; out[pos] = '\0';
    send_json(fd, 200, out);
    free(out); db_free_posts(posts);
}

/* ── GET /api/<tag>/thread/<id> ── */
static void handle_get_thread(int fd, kewld_server_t *srv, int64_t thread_id) {
    if (!db_thread_exists(srv->db, thread_id)) { send_err(fd, 404, "thread not found"); return; }
    kewld_post_t *posts = NULL; int count = 0;
    if (db_get_thread(srv->db, thread_id, &posts, &count) != 0) {
        send_err(fd, 500, "database error"); return;
    }
    size_t bufsz = (size_t)(count+1) * (KEWLD_MAX_CONTENT*3 + KEWLD_MAX_PGP_SIG*2);
    char *out = malloc(bufsz);
    size_t pos = 0; out[pos++] = '[';
    for (int i = 0; i < count; i++) {
        char pbuf[KEWLD_MAX_CONTENT*3 + KEWLD_MAX_PGP_SIG*2];
        post_to_json(&posts[i], pbuf, sizeof(pbuf));
        size_t plen = strlen(pbuf);
        memcpy(out+pos, pbuf, plen); pos += plen;
        if (i < count-1) out[pos++] = ',';
    }
    out[pos++] = ']'; out[pos] = '\0';
    send_json(fd, 200, out);
    free(out); db_free_posts(posts);
}

/* ── shared: fill post fields from multipart parts ── */
static void fill_post_from_parts(kewld_post_t *p, mp_part_t *parts, int nparts,
                                  kewld_server_t *srv, int is_op,
                                  int *media_err_out) {
    *media_err_out = 0;
    strncpy(p->name, "Anonymous", sizeof(p->name)-1);
    for (int i = 0; i < nparts; i++) {
        mp_part_t *pt = &parts[i];
        if (strcmp(pt->name, "subject") == 0 && p->subject[0] == '\0')
            strncpy(p->subject, (char*)pt->data, sizeof(p->subject)-1);
        else if (strcmp(pt->name, "content") == 0 && p->content[0] == '\0')
            strncpy(p->content, (char*)pt->data, sizeof(p->content)-1);
        else if (strcmp(pt->name, "sage") == 0)
            p->sage = (pt->data[0]=='1'||strcasecmp((char*)pt->data,"true")==0) ? 1 : 0;
        else if (strcmp(pt->name, "pgp_sig") == 0 && p->pgp_sig[0] == '\0')
            strncpy(p->pgp_sig, (char*)pt->data, sizeof(p->pgp_sig)-1);
        /* accept field name "media" or legacy "image" */
        else if ((strcmp(pt->name,"media")==0 || strcmp(pt->name,"image")==0)
                 && pt->data_len > 0 && pt->filename[0] != '\0') {
            char ext[8];
            media_ext_from_ct(pt->content_type, pt->filename, ext);
            if (ext[0] == '\0' || !is_allowed_ext(ext)) {
                *media_err_out = 1; continue;
            }
            char hash[65];
            if (save_media(srv->cfg->data_dir, pt->data, pt->data_len, ext, hash) == 0) {
                strncpy(p->media_hash, hash, sizeof(p->media_hash)-1);
                strncpy(p->media_ext,  ext,  sizeof(p->media_ext)-1);
            } else {
                *media_err_out = 2;
            }
        }
    }
    (void)is_op;
}

static void fill_post_from_urlenc(kewld_post_t *p, const char *body) {
    strncpy(p->name, "Anonymous", sizeof(p->name)-1);
    parse_form_field(body, "subject", p->subject, sizeof(p->subject));
    parse_form_field(body, "content", p->content, sizeof(p->content));
    parse_form_field(body, "pgp_sig", p->pgp_sig,  sizeof(p->pgp_sig));
    char sage_val[8];
    parse_form_field(body, "sage", sage_val, sizeof(sage_val));
    p->sage = (sage_val[0]=='1'||strcasecmp(sage_val,"true")==0) ? 1 : 0;
}

/* ── POST /api/<tag>/post ── */
static void handle_post_thread(int fd, kewld_server_t *srv,
                                const char *headers,
                                const unsigned char *body, size_t blen) {
    if (!body) { send_err(fd, 400, "empty body"); return; }

    kewld_post_t p; memset(&p, 0, sizeof(p));
    p.thread_id = 0;

    char boundary[256];
    int is_mp = extract_boundary(headers, boundary, sizeof(boundary)) == 0;

    if (is_mp) {
        mp_part_t parts[MAX_MP_PARTS]; int nparts;
        nparts = parse_multipart(body, blen, boundary, parts, MAX_MP_PARTS);
        int merr = 0;
        fill_post_from_parts(&p, parts, nparts, srv, 1, &merr);
        free_mp_parts(parts, nparts);
        if (merr == 1) { send_err(fd, 400, "unsupported media format"); return; }
        if (merr == 2) { send_err(fd, 500, "failed to save media"); return; }
        if (p.media_hash[0] == '\0') { send_err(fd, 400, "media required for new thread"); return; }
    } else {
        send_err(fd, 400, "media required for new thread"); return;
    }

    if (p.content[0] == '\0') { send_err(fd, 400, "content required"); return; }

    if (db_insert_post(srv->db, &p) != 0) { send_err(fd, 500, "database error"); return; }
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"id\":%lld,\"thread_id\":%lld,\"status\":\"submitted for review to index.\"}",
        (long long)p.id, (long long)p.id);
    send_json(fd, 201, buf);
}

/* ── POST /api/<tag>/thread/<id>/post ── */
static void handle_post_reply(int fd, kewld_server_t *srv, int64_t thread_id,
                               const char *headers,
                               const unsigned char *body, size_t blen) {
    if (!db_thread_exists(srv->db, thread_id)) { send_err(fd, 404, "thread not found"); return; }
    if (!body) { send_err(fd, 400, "empty body"); return; }

    kewld_post_t p; memset(&p, 0, sizeof(p));
    p.thread_id = thread_id;

    char boundary[256];
    int is_mp = extract_boundary(headers, boundary, sizeof(boundary)) == 0;

    if (is_mp) {
        mp_part_t parts[MAX_MP_PARTS]; int nparts;
        nparts = parse_multipart(body, blen, boundary, parts, MAX_MP_PARTS);
        int merr = 0;
        fill_post_from_parts(&p, parts, nparts, srv, 0, &merr);
        free_mp_parts(parts, nparts);
        if (merr == 1) { send_err(fd, 400, "unsupported media format"); return; }
        if (merr == 2) { send_err(fd, 500, "failed to save media"); return; }
    } else {
        fill_post_from_urlenc(&p, (const char*)body);
    }

    if (p.content[0] == '\0') { send_err(fd, 400, "content required"); return; }

    if (db_insert_post(srv->db, &p) != 0) { send_err(fd, 500, "database error"); return; }
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"id\":%lld,\"thread_id\":%lld,\"status\":\"submitted for review to index.\"}",
        (long long)p.id, (long long)thread_id);
    send_json(fd, 201, buf);
}

/* ── GET /api/<tag>/media/<hash><ext> ──
   Only serves the file if the hash is approved.
   If the request is for the placeholder hash, serve the built-in PNG. */

/* Minimal 1×1 grey PNG as a built-in placeholder */
static const unsigned char PENDING_PNG[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,
    0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
    0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,
    0xde,0x00,0x00,0x00,0x0c,0x49,0x44,0x41,
    0x54,0x08,0xd7,0x63,0xa8,0xa8,0xa8,0x00,
    0x00,0x00,0x04,0x00,0x01,0x27,0x08,0x46,
    0x6b,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,
    0x44,0xae,0x42,0x60,0x82
};
/* Actually let's use a proper "Media waiting for approval" 200×50 SVG-based
   PNG-ish approach – but since we can't run imagemagick in the binary, we
   embed a real 16-byte minimal grey PNG and let the client interpret
   media_pending=1 to show its own placeholder text.
   We still serve the 1-pixel grey PNG for hash=="pending". */

static void handle_get_media(int fd, kewld_server_t *srv,
                              const char *hash, const char *ext) {
    /* Serve built-in placeholder for the "pending" hash */
    if (strcmp(hash, PENDING_MEDIA_HASH) == 0) {
        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
            sizeof(PENDING_PNG));
        write(fd, hdr, hlen);
        write(fd, PENDING_PNG, sizeof(PENDING_PNG));
        return;
    }

    /* Check approval status in DB before serving */
    sqlite3_stmt *st = NULL;
    int approved = 0;
    /* Check threads first */
    sqlite3_prepare_v2(srv->db,
        "SELECT media_approved FROM threads WHERE media_hash=? LIMIT 1",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        approved = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    if (!approved) {
        sqlite3_prepare_v2(srv->db,
            "SELECT media_approved FROM posts WHERE media_hash=? LIMIT 1",
            -1, &st, NULL);
        sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            approved = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }

    if (approved != KEWLD_MEDIA_APPROVED) {
        /* Serve placeholder instead */
        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
            sizeof(PENDING_PNG));
        write(fd, hdr, hlen);
        write(fd, PENDING_PNG, sizeof(PENDING_PNG));
        return;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/media/%s%s", srv->cfg->data_dir, hash, ext);
    int imgfd = open(path, O_RDONLY);
    if (imgfd < 0) { send_err(fd, 404, "media not found"); return; }
    struct stat statbuf; fstat(imgfd, &statbuf);

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
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        ctype, (long long)statbuf.st_size);
    write(fd, hdr, hlen);
    char fbuf[8192]; ssize_t nr;
    while ((nr = read(imgfd, fbuf, sizeof(fbuf))) > 0) write(fd, fbuf, nr);
    close(imgfd);
}

/* ── GET /health ── */
static void handle_health(int fd, kewld_server_t *srv) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\",\"tag\":\"/%s/\",\"version\":\"%s\"}",
        srv->cfg->tag, KEWLD_VERSION);
    send_json(fd, 200, buf);
}

/* ── dispatcher ── */
static void dispatch(int fd, kewld_server_t *srv,
                     const char *method, const char *raw_path,
                     const char *headers,
                     const unsigned char *body, size_t blen) {
    char path[1024];
    char *q = strchr(raw_path, '?');
    const char *query = q ? q+1 : NULL;
    strncpy(path, raw_path, sizeof(path)-1); path[sizeof(path)-1]='\0';
    if (q) path[q-raw_path] = '\0';

    if (strcmp(path, "/health")==0) { handle_health(fd, srv); return; }
    if (strcmp(path, "/api/board")==0 && strcmp(method,"GET")==0) {
        handle_get_board_info(fd, srv); return;
    }

    char tag_threads[128], tag_post[128];
    snprintf(tag_threads, sizeof(tag_threads), "/api/%s/threads", srv->cfg->tag);
    snprintf(tag_post,    sizeof(tag_post),    "/api/%s/post",    srv->cfg->tag);

    if (strcmp(path, tag_threads)==0) {
        if (strcmp(method,"GET")==0) { handle_get_threads(fd, srv, query); return; }
        send_err(fd, 405, "method not allowed"); return;
    }
    if (strcmp(path, tag_post)==0) {
        if (strcmp(method,"POST")==0) { handle_post_thread(fd, srv, headers, body, blen); return; }
        send_err(fd, 405, "method not allowed"); return;
    }

    char thread_prefix[128];
    snprintf(thread_prefix, sizeof(thread_prefix), "/api/%s/thread/", srv->cfg->tag);
    if (strncmp(path, thread_prefix, strlen(thread_prefix))==0) {
        const char *rest = path + strlen(thread_prefix);
        char *slash = strchr(rest, '/');
        if (!slash) {
            int64_t tid = (int64_t)atoll(rest);
            if (strcmp(method,"GET")==0) { handle_get_thread(fd, srv, tid); return; }
            send_err(fd, 405, "method not allowed"); return;
        }
        int64_t tid = (int64_t)atoll(rest);
        if (strcmp(slash,"/post")==0 && strcmp(method,"POST")==0) {
            handle_post_reply(fd, srv, tid, headers, body, blen); return;
        }
        send_err(fd, 404, "not found"); return;
    }

    /* Media endpoint — also supports legacy /image/ path */
    char media_prefix[128], image_prefix[128];
    snprintf(media_prefix, sizeof(media_prefix), "/api/%s/media/", srv->cfg->tag);
    snprintf(image_prefix, sizeof(image_prefix), "/api/%s/image/", srv->cfg->tag);
    const char *mp = NULL;
    if (strncmp(path, media_prefix, strlen(media_prefix))==0 && strcmp(method,"GET")==0)
        mp = path + strlen(media_prefix);
    else if (strncmp(path, image_prefix, strlen(image_prefix))==0 && strcmp(method,"GET")==0)
        mp = path + strlen(image_prefix);

    if (mp) {
        char hash[65]={0}, ext[8]={0};
        const char *dot = strrchr(mp, '.');
        if (dot) {
            size_t hlen = (size_t)(dot-mp); if(hlen>64) hlen=64;
            memcpy(hash, mp, hlen);
            strncpy(ext, dot, sizeof(ext)-1);
        } else { strncpy(hash, mp, 64); }
        handle_get_media(fd, srv, hash, ext); return;
    }

    send_err(fd, 404, "not found");
}

/* ── connection thread ── */
static void *conn_thread(void *arg) {
    conn_ctx_t *ctx = (conn_ctx_t *)arg;
    char *method=NULL, *path=NULL, *headers=NULL;
    unsigned char *body=NULL; size_t blen=0;
    if (read_request(ctx->client_fd, &method, &path, &headers, &body, &blen)==0)
        dispatch(ctx->client_fd, ctx->srv, method, path, headers, body, blen);
    free(method); free(path); free(headers); free(body);
    close(ctx->client_fd); free(ctx);
    return NULL;
}

/* ── public API ── */
kewld_server_t *http_server_create(kewld_config_t *cfg, sqlite3 *db) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd<0) { log_err("socket: %s", strerror(errno)); return NULL; }
    int opt=1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_port=htons(cfg->http_port);
    inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
    if (bind(fd,(struct sockaddr*)&addr,sizeof(addr))!=0) {
        log_err("bind: %s", strerror(errno)); close(fd); return NULL;
    }
    if (listen(fd,64)!=0) { log_err("listen: %s",strerror(errno)); close(fd); return NULL; }
    kewld_server_t *srv = malloc(sizeof(kewld_server_t));
    srv->cfg=cfg; srv->db=db; srv->fd=fd;
    return srv;
}

int http_server_run(kewld_server_t *srv) {
    struct sockaddr_in ca; socklen_t clen=sizeof(ca);
    while (1) {
        int cfd=accept(srv->fd,(struct sockaddr*)&ca,&clen);
        if (cfd<0) { if(errno==EINTR) continue; break; }
        conn_ctx_t *ctx=malloc(sizeof(conn_ctx_t));
        ctx->client_fd=cfd; ctx->srv=srv;
        pthread_t tid;
        if (pthread_create(&tid,NULL,conn_thread,ctx)!=0) { close(cfd); free(ctx); }
        else pthread_detach(tid);
    }
    return 0;
}

void http_server_destroy(kewld_server_t *srv) { close(srv->fd); free(srv); }
