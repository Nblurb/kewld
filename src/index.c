#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "kewld.h"

static int tcp_connect(const char *host, int port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

int index_register(const kewld_config_t *cfg) {
    char body[1024], onion_esc[128], tag_esc[64], title_esc[256], desc_esc[512];
    json_escape(cfg->onion_addr, onion_esc, sizeof(onion_esc));
    json_escape(cfg->tag,        tag_esc,   sizeof(tag_esc));
    json_escape(cfg->title,      title_esc, sizeof(title_esc));
    json_escape(cfg->desc,       desc_esc,  sizeof(desc_esc));
    int blen = snprintf(body, sizeof(body),
        "{\"onion\":\"%s\",\"tag\":\"/%s/\",\"title\":\"%s\","
        "\"desc\":\"%s\",\"nsfw\":%d,\"version\":\"%s\"}",
        onion_esc, tag_esc, title_esc, desc_esc, cfg->nsfw, KEWLD_VERSION);
    char request[2048];
    int rlen = snprintf(request, sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "User-Agent: kewld/%s\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        KEWLD_INDEX_REGISTER_PATH, KEWLD_INDEX_HOST, blen, KEWLD_VERSION, body);

    /* Connect TCP */
    int fd = tcp_connect(KEWLD_INDEX_HOST, KEWLD_INDEX_PORT);
    if (fd < 0) { log_warn("index_register: connect failed: %s", strerror(errno)); return -1; }

    /* Wrap with TLS */
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { close(fd); log_warn("index_register: SSL_CTX_new failed"); return -1; }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(ctx);

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, KEWLD_INDEX_HOST); /* SNI */

    if (SSL_connect(ssl) != 1) {
        log_warn("index_register: TLS handshake failed");
        SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
        return -1;
    }

    SSL_write(ssl, request, rlen);

    char resp[512];
    int n = SSL_read(ssl, resp, sizeof(resp) - 1);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);

    if (n <= 0) return -1;
    resp[n] = '\0';
    if (strncmp(resp, "HTTP/1.1 200", 12) == 0 || strncmp(resp, "HTTP/1.1 201", 12) == 0)
        return 0;
    log_warn("index_register: unexpected response: %.64s", resp);
    return -1;
}
