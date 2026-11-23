#include "local_path.h"
#include "server_internal.h"
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__) && defined(__has_include)
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif
#endif
#include "http_request.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int file_error_status(int error) {
    switch (error) {
    case ENOENT: case ENOTDIR: return 404;
    case EACCES: case EPERM: case ELOOP: return 403;
    case ENAMETOOLONG: return 414;
    default: return 500;
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int decode_url_path(const char *path, char *decoded, size_t capacity) {
    size_t n = 0;
    if (!capacity) return 414;
    while (*path) {
        unsigned char c = (unsigned char)*path++;
        if (c == '%') {
            int hi = hex_value(*path);
            if (hi < 0) return 400;
            int lo = hex_value(*++path);
            if (lo < 0) return 400;
            path++;
            c = (unsigned char)(hi * 16 + lo);
        }
        if (c < 32 || c == 127 || c == '\\') return 400;
        if (n == capacity - 1) return 414;
        decoded[n++] = (char)c;
    }
    decoded[n] = '\0';
    return 0;
}

static int canonical_target(const char *base, const char *path, int directory_index,
                            char resolved[PATH_MAX]) {
    char decoded[MAX_URI_LEN];
    int status = decode_url_path(path, decoded, sizeof(decoded));
    if (status) return status;
    for (const char *p = decoded; *p;) {
        p += strspn(p, "/");
        size_t len = strcspn(p, "/");
        if (len == 2 && p[0] == '.' && p[1] == '.') return 403;
        p += len;
    }

    char candidate[PATH_MAX];
    size_t n = strlen(decoded);
    int index = directory_index && n && decoded[n - 1] == '/';
    int len = snprintf(candidate, sizeof(candidate), "%s/%s%s", base, decoded,
                       index ? "index.html" : "");
    if (len < 0 || (size_t)len >= sizeof(candidate)) return 414;
    if (!realpath(candidate, resolved)) return file_error_status(errno);
    size_t base_len = strlen(base);
    if (strcmp(base, "/") && (strncmp(base, resolved, base_len) ||
        (resolved[base_len] && resolved[base_len] != '/'))) return 403;
    return 0;
}

int resolve_local_path(const char *root, const char *path, int directory_index,
                       char resolved[PATH_MAX]) {
    char base[PATH_MAX];
    if (!realpath(root, base)) return file_error_status(errno);
    return canonical_target(base, path, directory_index, resolved);
}

int init_local_root(struct local_root *root, const char *directory) {
    root->fd = -1;
    root->status = 0;
    if (realpath(directory, root->base))
        root->fd = open(root->base, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root->fd >= 0) return 0;
    int error = errno;
    root->status = file_error_status(error);
    return error;
}

void close_local_root(struct local_root *root) {
    if (root->fd >= 0) close(root->fd);
}

int open_local_file(const struct local_root *root, const char *url, int directory_index,
                    int flags, int *file, char resolved[PATH_MAX]) {
    if (root->status) return root->status;
    int status = canonical_target(root->base, url, directory_index, resolved);
    if (status) return status;

    // realpath permits internal links. Reopen its relative result from the pinned root;
    // a newly substituted symlink must never be followed by this walk.
    char relative[PATH_MAX];
    const char *suffix = resolved + strlen(root->base);
    if (*suffix == '/') suffix++;
    strcpy(relative, *suffix ? suffix : ".");
#ifdef SYS_openat2
    struct open_how how = {.flags = (unsigned)(flags | O_CLOEXEC | O_NOFOLLOW),
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS};
    *file = (int)syscall(SYS_openat2, root->fd, relative, &how, sizeof(how));
    if (*file >= 0) return 0;
    if (errno != ENOSYS) {
        if (errno == EXDEV || errno == EAGAIN) return 403;
        return file_error_status(errno);
    }
#endif
    char *component = relative;
    int parent = root->fd;
    for (;;) {
        char *slash = strchr(component, '/');
        if (slash) *slash = '\0';
        int mode = slash ? O_RDONLY | O_DIRECTORY : flags;
        int next = openat(parent, component, mode | O_NOFOLLOW | O_CLOEXEC);
        int error = errno;
        if (parent != root->fd) close(parent);
        if (next < 0) return file_error_status(error);
        if (!slash) {
            *file = next;
            return 0;
        }
        parent = next;
        component = slash + 1;
    }
}
