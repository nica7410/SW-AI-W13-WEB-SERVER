#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "cgi.h"
#include "server_internal.h"
#include <fcntl.h>
#include "local_path.h"
#include "proxy_internal.h"
#include "response_stream.h"
#include "server.h"
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

// CGI가 stdin을 읽기 전에 stdout을 채워도 부모와 자식이 서로 기다리지 않는다.
static FILE *body_input(const http_request *req) {
    if (!req->content_length) return fopen("/dev/null", "r");
    FILE *file = tmpfile();
    if (file && (fwrite(req->body, 1, req->content_length, file) != req->content_length ||
                 fseek(file, 0, SEEK_SET) != 0)) {
        fclose(file);
        return NULL;
    }
    return file;
}

static void close_interval(unsigned first, unsigned last) {
    if (first > last) return;
#ifdef SYS_close_range
    if (syscall(SYS_close_range, first, last, 0u) == 0) return;
#endif
    for (unsigned fd = first; fd <= last; fd++) close((int)fd);
}

static void reap_child(pid_t child, int owns_group, int abort_child, deadline_ms end) {
    // CGI가 출력을 닫은 후에도 끝나지 않으면 워커를 계속 점유하지 않게 한다.
    deadline_ms idle_end = deadline_after(KEEPALIVE_TIMEOUT_SEC * 1000);
    if (idle_end < end) end = idle_end;
    while (!abort_child && deadline_left(end)) {
        pid_t result = waitpid(child, NULL, WNOHANG);
        if (result == child || (result < 0 && errno != EINTR)) return;
        struct timespec pause = {.tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    }
    // Keep the direct child unreaped until the group signal has used its reserved PID.
    if (owns_group) kill(-child, SIGKILL);
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static int numeric_address(int fd, int peer, char *host, socklen_t hostlen,
                           char *port, socklen_t portlen) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    int rc = peer ? getpeername(fd, (struct sockaddr *)&addr, &len)
                  : getsockname(fd, (struct sockaddr *)&addr, &len);
    if (rc || (addr.ss_family != AF_INET && addr.ss_family != AF_INET6)) return 0;
    return getnameinfo((struct sockaddr *)&addr, len, host, hostlen, port, portlen,
                       NI_NUMERICHOST | NI_NUMERICSERV) == 0;
}

static int run_cgi(int fd, int program, char *path, const http_request *req) {
    char method[32], query[MAX_URI_LEN + 16], length[64], type[MAX_HEADER_VALUE + 16];
    const char *content_type = get_header(req, "Content-Type");
    snprintf(method, sizeof(method), "REQUEST_METHOD=%s", req->method);
    snprintf(query, sizeof(query), "QUERY_STRING=%s", req->query);
    snprintf(length, sizeof(length), "CONTENT_LENGTH=%zu", req->content_length);
    snprintf(type, sizeof(type), "CONTENT_TYPE=%s", content_type ? content_type : "");
    char protocol[sizeof("SERVER_PROTOCOL=") + sizeof(req->version)];
    char script[sizeof("SCRIPT_NAME=") - 1 + MAX_URI_LEN] = "SCRIPT_NAME=";
    snprintf(protocol, sizeof(protocol), "SERVER_PROTOCOL=%s", req->version);
    int decoded = decode_url_path(req->path, script + strlen(script), MAX_URI_LEN);
    if (decoded) return decoded;

    char name[sizeof("SERVER_NAME=") - 1 + MAX_HEADER_VALUE] = "SERVER_NAME=";
    char port[sizeof("SERVER_PORT=") - 1 + NI_MAXSERV] = "SERVER_PORT=";
    char remote[sizeof("REMOTE_ADDR=") - 1 + NI_MAXHOST] = "REMOTE_ADDR=";
    char *host = name + strlen(name);
    int local = numeric_address(fd, 0, host, MAX_HEADER_VALUE, port + strlen(port), NI_MAXSERV);
    const char *authority = get_header(req, "Host");
    if (authority) {
        char ignored_port[8];
        if (parse_authority(authority, strlen(authority), "80", host, MAX_HEADER_VALUE,
                            ignored_port, sizeof(ignored_port)) < 0) return 400;
    }
    char *env[] = {method, query, length, type, "GATEWAY_INTERFACE=CGI/1.1", "PATH=/usr/bin:/bin",
                   protocol, script, NULL, NULL, NULL, NULL};
    char **next = env;
    while (*next) next++;
    if (authority || local) *next++ = name;
    if (local) *next++ = port;
    if (numeric_address(fd, 1, remote + strlen(remote), NI_MAXHOST, NULL, 0)) *next++ = remote;
    char *argv[] = {path, NULL};
    FILE *input = body_input(req);
    if (!input) return 500;
    int in = fileno(input), out[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, out) < 0) { fclose(input); return 500; }
    // Successful exec closes this pipe; on failure the child reports the kernel errno.
    int report[2];
    if (pipe(report) < 0) {
        fclose(input);
        close(out[0]);
        close(out[1]);
        return 500;
    }
    struct timeval tv = {.tv_sec = KEEPALIVE_TIMEOUT_SEC};
    long limit = sysconf(_SC_OPEN_MAX);
    deadline_ms end = deadline_after(RESPONSE_TIMEOUT_MS);
    pid_t child = -1;
    if (limit > 0 && fcntl(report[1], F_SETFD, FD_CLOEXEC) == 0 &&
        setsockopt(out[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0)
        child = fork();
    if (child == 0) {
        if (setpgid(0, 0) == 0 && dup2(in, STDIN_FILENO) >= 0 &&
            dup2(out[1], STDOUT_FILENO) >= 0) {
            int low = program < report[1] ? program : report[1];
            int high = program > report[1] ? program : report[1];
            close_interval(3, (unsigned)(low - 1));
            close_interval((unsigned)(low + 1), (unsigned)(high - 1));
            close_interval((unsigned)(high + 1), (unsigned)limit - 1);
#ifdef __linux__
            fexecve(program, argv, env);
            // A shebang interpreter must still be able to open the executable FD.
            if (errno == ENOENT && fcntl(program, F_SETFD, 0) == 0)
                fexecve(program, argv, env);
#else
            execve(path, argv, env);
#endif
        }
        int error = errno;
        ssize_t n;
        do { n = write(report[1], &error, sizeof(error)); } while (n < 0 && errno == EINTR);
        _exit(127);
    }
    int owns_group = child > 0 && (setpgid(child, child) == 0 || getpgid(child) == child);
    fclose(input);
    close(out[1]);
    close(report[1]);
    int status = 500;
    if (child > 0) {
        int error;
        deadline_ms exec_end = deadline_after(SOCKET_IDLE_TIMEOUT_MS);
        if (end < exec_end) exec_end = end;
        ssize_t n = -1;
        while (wait_until(report[0], POLLIN, exec_end) == 0) {
            n = read(report[0], &error, sizeof(error));
            if (n >= 0 || errno != EINTR) break;
        }
        if (!n) status = response_until(fd, out[0], req, 1, end, end);
        else if (n == sizeof(error) && (error == EACCES || error == EPERM)) status = 403;
    }
    close(report[0]);
    close(out[0]);
    if (child > 0) reap_child(child, owns_group, status != 0, end);
    return status;
}

int serve_open_cgi(int fd, int program, char *path, const http_request *req) {
    struct stat opened;
    if (program < 3 || fstat(program, &opened) < 0) return 500;
    if (!S_ISREG(opened.st_mode)) return 403;
#ifdef __linux__
    return run_cgi(fd, program, path, req);
#else
    // Keep a stable executable name until the interpreter has finished reopening it.
    // A hard link preserves the original file's execute permissions and filesystem.
    const char *tmp = getenv("TMPDIR");
    char dir[PATH_MAX], executable[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/w13-cgi-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (n < 0 || (size_t)n >= sizeof(dir) || !mkdtemp(dir)) return 500;
    int status = 500;
    n = snprintf(executable, sizeof(executable), "%s/program", dir);
    if (n >= 0 && (size_t)n < sizeof(executable)) {
        if (linkat(AT_FDCWD, path, AT_FDCWD, executable, 0) == 0) {
            struct stat linked;
            if (lstat(executable, &linked) == 0) {
                if (linked.st_dev == opened.st_dev && linked.st_ino == opened.st_ino)
                    status = run_cgi(fd, program, executable, req);
                else status = 403;
            }
            unlink(executable);
        } else status = file_error_status(errno);
    }
    rmdir(dir);
    return status;
#endif
}

int serve_cgi_at(int fd, const struct local_root *root, const http_request *req) {
    char path[PATH_MAX];
    if (strncmp(req->path, CGI_PREFIX, strlen(CGI_PREFIX))) return 404;
    int program;
#ifdef __linux__
    int flags = O_PATH;
#else
    int flags = O_RDONLY | O_NONBLOCK;
#endif
    int status = open_local_file(root, req->path + strlen(CGI_PREFIX), 0, flags, &program, path);
    if (!status) {
        status = serve_open_cgi(fd, program, path, req);
        close(program);
    }
    return status;
}

int serve_cgi(int fd, const char *cgi_dir, const http_request *req) {
    struct local_root root;
    init_local_root(&root, cgi_dir);
    int status = serve_cgi_at(fd, &root, req);
    close_local_root(&root);
    return status;
}
