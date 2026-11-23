#include "server.h"
#include "proxy_internal.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int positive_number(const char *text, int max) {
    if (!*text || text[strspn(text, "0123456789")]) return -1;
    errno = 0;
    long n = strtol(text, NULL, 10);
    return !errno && n >= 1 && n <= max ? (int)n : -1;
}

static void stop_handler(int sig) {
    (void)sig;
    request_stop();
}

int main(int argc, char **argv) {
    server_config cfg = {.port = "8080", .root = "./www", .cgi_dir = "./cgi-bin",
                         .threads = 8, .queue_size = 1024};
    int opt;
    while ((opt = getopt(argc, argv, "p:r:g:c:n:ft:q:v")) != -1) {
        switch (opt) {
        case 'p': cfg.port = optarg; break;
        case 'r': cfg.root = optarg; cfg.root_explicit = 1; break;
        case 'g': cfg.cgi_dir = optarg; break;
        case 'c': cfg.routes_path = optarg; break;
        case 'n': cfg.server_name = optarg; break;
        case 'f': cfg.forward_enabled = 1; break;
        case 'v': cfg.verbose = 1; break;
        case 't': cfg.threads = positive_number(optarg, INT_MAX); break;
        case 'q': cfg.queue_size = positive_number(optarg, INT_MAX); break;
        default: goto usage;
        }
    }
    if (optind != argc || positive_number(cfg.port, 65535) < 0 ||
        cfg.threads < 1 || cfg.queue_size < 1) goto usage;

    if (cfg.server_name) {
        char host[256], port[8];
        if (strchr(cfg.server_name, ':') ||
            parse_authority(cfg.server_name, strlen(cfg.server_name), "80",
                            host, sizeof(host), port, sizeof(port)) < 0) goto usage;
    }

    struct sigaction action = {.sa_handler = SIG_IGN};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, NULL) < 0) return 1;
    action.sa_handler = stop_handler;
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0)
        return 1;
    return run_server(&cfg) == 0 ? 0 : 1;
usage:
    fprintf(stderr, "usage: %s [-p port] [-r root] [-g cgi_dir] [-c routes.conf] [-n server-name]"
                    " [-f] [-t threads] [-q queue_size] [-v]\n", argv[0]);
    return 1;
}
