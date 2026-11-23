#include "static_file.h"
#include "deadline.h"
#include "server_internal.h"
#include "http_response.h"
#include "local_path.h"
#include "mime.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int serve_static_at(int fd, const struct local_root *root, const http_request *req) {
    if (!strcmp(req->method, "POST")) return 405;
    char path[PATH_MAX];
    int file;
    int status = open_local_file(root, req->path, 1, O_RDONLY | O_NONBLOCK, &file, path);
    if (status) return status;
    struct stat st;
    if (fstat(file, &st) < 0) status = 500;
    else if (!S_ISREG(st.st_mode)) status = 403;
    else if (st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) status = 500;
    else {
        size_t remaining = (size_t)st.st_size;
        status = send_headers(fd, 200, mime_type(path), remaining, req->keep_alive);
        if (!strcmp(req->method, "HEAD")) remaining = 0;
        char buf[RIO_BUFSIZE];
        while (!status && remaining) {
            size_t take = remaining < sizeof(buf) ? remaining : sizeof(buf);
            ssize_t n = read(file, buf, take);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || send_until(fd, buf, (size_t)n, INT64_MAX) < 0) status = -1;
            else remaining -= (size_t)n;
        }
    }
    close(file);
    return status;
}

int serve_static(int fd, const char *root, const http_request *req) {
    struct local_root dir;
    init_local_root(&dir, root);
    int status = serve_static_at(fd, &dir, req);
    close_local_root(&dir);
    return status;
}
