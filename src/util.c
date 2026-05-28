#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <openssl/evp.h>
#include "2kewld.h"

static void vlog(const char *level, const char *fmt, va_list ap) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(stderr, "[%s] [%s] ", ts, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
void log_info(const char *fmt, ...) { va_list a; va_start(a,fmt); vlog("INFO",fmt,a); va_end(a); }
void log_warn(const char *fmt, ...) { va_list a; va_start(a,fmt); vlog("WARN",fmt,a); va_end(a); }
void log_err (const char *fmt, ...) { va_list a; va_start(a,fmt); vlog("ERR" ,fmt,a); va_end(a); }

void daemonize(const char *pid_file) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);
    setsid();
    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0);
    umask(0);
    if (chdir("/") != 0) {}
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) { dup2(fd,0); dup2(fd,1); dup2(fd,2); if (fd>2) close(fd); }
    if (pid_file) {
        FILE *f = fopen(pid_file, "w");
        if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }
    }
}

int sha256_file(const char *path, char *hex_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    unsigned char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) EVP_DigestUpdate(ctx, buf, n);
    fclose(f);
    unsigned char hash[32]; unsigned int hlen = 32;
    EVP_DigestFinal_ex(ctx, hash, &hlen);
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < 32; i++) sprintf(hex_out + i*2, "%02x", hash[i]);
    hex_out[64] = '\0';
    return 0;
}

int sha256_buf(const unsigned char *data, size_t len, char *hex_out) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    unsigned char hash[32]; unsigned int hlen = 32;
    EVP_DigestFinal_ex(ctx, hash, &hlen);
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < 32; i++) sprintf(hex_out + i*2, "%02x", hash[i]);
    hex_out[64] = '\0';
    return 0;
}

char *url_decode(const char *in, char *out, size_t outlen) {
    size_t i = 0, j = 0;
    while (in[i] && j < outlen - 1) {
        if (in[i] == '%' && in[i+1] && in[i+2]) {
            char h[3] = { in[i+1], in[i+2], 0 };
            out[j++] = (char)strtol(h, NULL, 16); i += 3;
        } else if (in[i] == '+') { out[j++] = ' '; i++;
        } else { out[j++] = in[i++]; }
    }
    out[j] = '\0'; return out;
}

char *json_escape(const char *in, char *out, size_t outlen) {
    size_t i = 0, j = 0;
    while (in[i] && j < outlen - 2) {
        unsigned char c = (unsigned char)in[i];
        if      (c == '"')  { out[j++]='\\'; out[j++]='"'; }
        else if (c == '\\') { out[j++]='\\'; out[j++]='\\'; }
        else if (c == '\n') { out[j++]='\\'; out[j++]='n'; }
        else if (c == '\r') { out[j++]='\\'; out[j++]='r'; }
        else if (c == '\t') { out[j++]='\\'; out[j++]='t'; }
        else if (c < 0x20)  { j += snprintf(out+j, outlen-j, "\\u%04x", c); }
        else                { out[j++] = (char)c; }
        i++;
    }
    out[j] = '\0'; return out;
}

long current_unix(void) { return (long)time(NULL); }
