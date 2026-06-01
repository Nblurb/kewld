#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "kewld.h"

static pid_t tor_pid = 0;

static int write_torrc(kewld_config_t *cfg, const char *torrc_path, const char *hs_dir) {
    FILE *f = fopen(torrc_path, "w");
    if (!f) { log_err("write_torrc: fopen %s: %s", torrc_path, strerror(errno)); return -1; }
    fprintf(f, "SocksPort 127.0.0.1:%d\n", cfg->tor_socks_port);
    fprintf(f, "ControlPort 127.0.0.1:%d\n", cfg->tor_ctrl_port);
    fprintf(f, "DataDirectory %s/data\n", cfg->data_dir);
    fprintf(f, "HiddenServiceDir %s\n", hs_dir);
    fprintf(f, "HiddenServicePort 80 127.0.0.1:%d\n", cfg->http_port);
    fprintf(f, "Log notice stderr\n");
    fclose(f);
    return 0;
}

int tor_launch(kewld_config_t *cfg) {
    char torrc_path[600], hs_dir[600], tor_data[600];
    snprintf(torrc_path, sizeof(torrc_path), "%s/tor/torrc",    cfg->data_dir);
    snprintf(hs_dir,     sizeof(hs_dir),     "%s/tor/hs",       cfg->data_dir);
    snprintf(tor_data,   sizeof(tor_data),   "%s/tor/data",     cfg->data_dir);
    mkdir(hs_dir,   0700);
    mkdir(tor_data, 0700);
    if (write_torrc(cfg, torrc_path, hs_dir) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { log_err("fork: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        execlp("tor", "tor", "-f", torrc_path, NULL);
        perror("execlp tor");
        _exit(1);
    }
    tor_pid = pid;
    log_info("tor launched (pid %d)", (int)pid);
    return 0;
}

int tor_wait_ready(int ctrl_port, int timeout_sec) {
    for (int i = 0; i < timeout_sec; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { sleep(1); continue; }
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(ctrl_port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(fd);
            log_info("tor control port ready after %ds", i);
            sleep(2);
            return 0;
        }
        close(fd);
        sleep(1);
    }
    log_err("tor_wait_ready: timed out after %ds", timeout_sec);
    return -1;
}

int tor_read_onion(const char *tor_dir, char *out, size_t outlen) {
    char path[700];
    snprintf(path, sizeof(path), "%s/hs/hostname", tor_dir);
    for (int i = 0; i < 30; i++) {
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(out, (int)outlen, f)) {
                size_t len = strlen(out);
                while (len > 0 && (out[len-1]=='\n'||out[len-1]=='\r'||out[len-1]==' '))
                    out[--len] = '\0';
                fclose(f);
                return 0;
            }
            fclose(f);
        }
        sleep(1);
    }
    log_err("tor_read_onion: hostname file not found at %s", path);
    return -1;
}

void tor_shutdown(void) {
    if (tor_pid > 0) {
        kill(tor_pid, SIGTERM);
        tor_pid = 0;
        log_info("tor stopped");
    }
}
