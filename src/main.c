/*
 * 2kewld — The 2Kewl Onion Daemon
 * main.c — entry point, config parsing, startup orchestration
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>

#include "2kewld.h"

static volatile int g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
    log_info("signal received, shutting down...");
    tor_shutdown();
    exit(0);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "2kewld v%s — The 2Kewl Onion Daemon\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --tag        <tag>      Board tag, e.g. 'tech' → /tech/  (required)\n"
        "  --title      <title>    Board display title\n"
        "  --desc       <desc>     Board description\n"
        "  --data-dir   <path>     Data directory (default: ~/.2kewld/<tag>)\n"
        "  --port       <port>     Local HTTP port (default: %d)\n"
        "  --socks-port <port>     Tor SOCKS port (default: %d)\n"
        "  --ctrl-port  <port>     Tor control port (default: %d)\n"
        "  --no-images             Disable image uploads\n"
        "  --nsfw                  Mark board as NSFW\n"
        "  --no-register           Don't register with 2kewl index\n"
        "  --daemon                Fork to background\n"
        "  --pid-file   <path>     PID file path (implies --daemon)\n"
        "  --max-threads  <n>      Max threads per board (default: %d)\n"
        "  --max-replies  <n>      Max replies per thread (default: %d)\n"
        "  --verbose               Verbose logging\n"
        "  --help                  Show this help\n"
        "\n"
        "Example:\n"
        "  2kewld --tag tech --title 'Technology' --desc 'Tech discussion'\n",
        KEWLD_VERSION, argv0,
        KEWLD_DEFAULT_HTTP_PORT,
        KEWLD_DEFAULT_SOCKS_PORT,
        KEWLD_DEFAULT_CTRL_PORT,
        KEWLD_DEFAULT_THREADS,
        KEWLD_DEFAULT_REPLIES
    );
}

static void config_defaults(kewld_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->http_port       = KEWLD_DEFAULT_HTTP_PORT;
    cfg->tor_socks_port  = KEWLD_DEFAULT_SOCKS_PORT;
    cfg->tor_ctrl_port   = KEWLD_DEFAULT_CTRL_PORT;
    cfg->register_index  = 1;
    cfg->allow_images    = 1;
    cfg->nsfw            = 0;
    cfg->daemon          = 0;
    cfg->max_threads     = KEWLD_DEFAULT_THREADS;
    cfg->max_replies     = KEWLD_DEFAULT_REPLIES;
    cfg->verbose         = 0;
    strncpy(cfg->title, "Anonymous Board", sizeof(cfg->title) - 1);
    strncpy(cfg->desc,  "A 2kewl board.", sizeof(cfg->desc) - 1);
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            log_err("path exists but is not a directory: %s", path);
            return -1;
        }
        return 0;
    }
    if (mkdir(path, 0700) != 0) {
        log_err("mkdir(%s): %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void build_default_data_dir(kewld_config_t *cfg) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(cfg->data_dir, sizeof(cfg->data_dir),
             "%s/.2kewld/%s", home, cfg->tag);
}

static int parse_args(int argc, char **argv, kewld_config_t *cfg) {
    for (int i = 1; i < argc; i++) {
#define NEED_ARG(flag) \
    if (i + 1 >= argc) { \
        fprintf(stderr, "error: %s requires an argument\n", flag); \
        return -1; \
    } \
    i++

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--tag") == 0) {
            NEED_ARG("--tag");
            strncpy(cfg->tag, argv[i], sizeof(cfg->tag) - 1);
            /* strip leading slash if user wrote /tech/ */
            char *p = cfg->tag;
            while (*p == '/') memmove(p, p+1, strlen(p));
            size_t len = strlen(cfg->tag);
            while (len > 0 && cfg->tag[len-1] == '/') cfg->tag[--len] = '\0';
        } else if (strcmp(argv[i], "--title") == 0) {
            NEED_ARG("--title");
            strncpy(cfg->title, argv[i], sizeof(cfg->title) - 1);
        } else if (strcmp(argv[i], "--desc") == 0) {
            NEED_ARG("--desc");
            strncpy(cfg->desc, argv[i], sizeof(cfg->desc) - 1);
        } else if (strcmp(argv[i], "--data-dir") == 0) {
            NEED_ARG("--data-dir");
            strncpy(cfg->data_dir, argv[i], sizeof(cfg->data_dir) - 1);
        } else if (strcmp(argv[i], "--port") == 0) {
            NEED_ARG("--port");
            cfg->http_port = (uint16_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "--socks-port") == 0) {
            NEED_ARG("--socks-port");
            cfg->tor_socks_port = atoi(argv[i]);
        } else if (strcmp(argv[i], "--ctrl-port") == 0) {
            NEED_ARG("--ctrl-port");
            cfg->tor_ctrl_port = atoi(argv[i]);
        } else if (strcmp(argv[i], "--no-images") == 0) {
            cfg->allow_images = 0;
        } else if (strcmp(argv[i], "--nsfw") == 0) {
            cfg->nsfw = 1;
        } else if (strcmp(argv[i], "--no-register") == 0) {
            cfg->register_index = 0;
        } else if (strcmp(argv[i], "--daemon") == 0) {
            cfg->daemon = 1;
        } else if (strcmp(argv[i], "--pid-file") == 0) {
            NEED_ARG("--pid-file");
            strncpy(cfg->pid_file, argv[i], sizeof(cfg->pid_file) - 1);
            cfg->daemon = 1;
        } else if (strcmp(argv[i], "--max-threads") == 0) {
            NEED_ARG("--max-threads");
            cfg->max_threads = atoi(argv[i]);
        } else if (strcmp(argv[i], "--max-replies") == 0) {
            NEED_ARG("--max-replies");
            cfg->max_replies = atoi(argv[i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = 1;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
#undef NEED_ARG
    }
    return 0;
}

int main(int argc, char **argv) {
    kewld_config_t cfg;
    config_defaults(&cfg);

    if (parse_args(argc, argv, &cfg) != 0)
        return 1;

    if (cfg.tag[0] == '\0') {
        fprintf(stderr, "error: --tag is required (e.g. --tag tech)\n");
        usage(argv[0]);
        return 1;
    }

    /* Build data dir if not specified */
    if (cfg.data_dir[0] == '\0')
        build_default_data_dir(&cfg);

    log_info("2kewld v%s starting", KEWLD_VERSION);
    log_info("board  : /%s/", cfg.tag);
    log_info("title  : %s",   cfg.title);
    log_info("datadir: %s",   cfg.data_dir);

    /* Create directory tree */
    {
        /* ~/.2kewld */
        char parent[512];
        snprintf(parent, sizeof(parent), "%s/.2kewld", getenv("HOME") ? getenv("HOME") : "/tmp");
        /* We call ensure_dir on parent first, then full path */
        /* (ignore error on parent — might already exist) */
        mkdir(parent, 0700);
    }
    if (ensure_dir(cfg.data_dir) != 0) return 1;

    char db_path[600], tor_dir[600], img_dir[600];
    snprintf(db_path,  sizeof(db_path),  "%s/board.db",  cfg.data_dir);
    snprintf(tor_dir,  sizeof(tor_dir),  "%s/tor",       cfg.data_dir);
    snprintf(img_dir,  sizeof(img_dir),  "%s/images",    cfg.data_dir);
    if (ensure_dir(tor_dir) != 0) return 1;
    if (ensure_dir(img_dir) != 0) return 1;

    /* Open / init database */
    sqlite3 *db = NULL;
    if (db_open(db_path, &db) != 0) return 1;
    if (db_init_schema(db)    != 0) return 1;
    log_info("database ready: %s", db_path);

    /* Daemonize before launching Tor so the child owns the process */
    if (cfg.daemon)
        daemonize(cfg.pid_file[0] ? cfg.pid_file : NULL);

    /* Signals */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Launch embedded Tor */
    log_info("launching Tor hidden service...");
    if (tor_launch(&cfg) != 0) {
        log_err("failed to launch Tor");
        return 1;
    }
    if (tor_wait_ready(cfg.tor_ctrl_port, 120) != 0) {
        log_err("Tor did not become ready in time");
        return 1;
    }
    if (tor_read_onion(tor_dir, cfg.onion_addr, sizeof(cfg.onion_addr)) != 0) {
        log_err("could not read onion address");
        return 1;
    }

    log_info("onion address : %s", cfg.onion_addr);
    log_info("board URL     : http://%s/%s/", cfg.onion_addr, cfg.tag);

    /* Register with 2kewl index */
    if (cfg.register_index) {
        log_info("registering with 2kewl index at %s ...", KEWLD_INDEX_HOST);
        if (index_register(&cfg) != 0)
            log_warn("registration failed (continuing anyway)");
        else
            log_info("registered OK");
    }

    /* Start HTTP server (blocks until shutdown) */
    kewld_server_t *srv = http_server_create(&cfg, db);
    if (!srv) {
        log_err("failed to create HTTP server");
        return 1;
    }
    log_info("HTTP server listening on 127.0.0.1:%d", cfg.http_port);
    http_server_run(srv);   /* blocks */

    http_server_destroy(srv);
    sqlite3_close(db);
    tor_shutdown();
    log_info("2kewld stopped.");
    return 0;
}
