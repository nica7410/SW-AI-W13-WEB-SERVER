#ifndef SERVER_INTERNAL_H
#define SERVER_INTERNAL_H

#include "http_request.h"
#include <limits.h>

struct local_root {
    int fd, status;
    char base[PATH_MAX];
};

int init_local_root(struct local_root *root, const char *directory);
void close_local_root(struct local_root *root);
int open_local_file(const struct local_root *root, const char *url, int directory_index,
                    int flags, int *file, char resolved[PATH_MAX]);
int serve_static_at(int fd, const struct local_root *root, const http_request *req);
int serve_cgi_at(int fd, const struct local_root *root, const http_request *req);
int serve_open_cgi(int client, int program, char *path, const http_request *req);
int send_request_error(int fd, int status, int keep_alive, int head_only);
int finish_request(int fd, const http_request *req, int result, int linger_ms);
int send_directory_redirect(int fd, const http_request *req);
#endif
