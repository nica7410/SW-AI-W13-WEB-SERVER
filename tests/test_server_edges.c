#include "test.h"
#include "tcp_pair.h"
#include "http_response.h"
#include "server.h"
#include "server_internal.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *name, const char *text, mode_t mode) {
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, mode);
    require(fd >= 0, name);
    require(rio_writen(fd, text, strlen(text)) == (ssize_t)strlen(text), name);
    require(close(fd) == 0, name);
}

static ssize_t read_reply(int fd, char *data, size_t size) {
    rio_t rio;
    rio_readinitb(&rio, fd);
    ssize_t n = rio_readnb(&rio, data, size - 1);
    data[n < 0 ? 0 : n] = '\0';
    return n;
}

static void check_reply(int pair[2], const char *status_line, const char *expected_body) {
    shutdown(pair[0], SHUT_WR);
    char data[512];
    CHECK(read_reply(pair[1], data, sizeof(data)) > 0);
    CHECK(strncmp(data, status_line, strlen(status_line)) == 0);
    char *date = strstr(data, "\r\nDate: ");
    CHECK(date && strstr(date + 1, "\r\nDate: ") == NULL);
    char *body = strstr(data, "\r\n\r\n");
    CHECK(body && !strcmp(body + 4, expected_body));
    close(pair[0]);
    close(pair[1]);
}

static void check_directory_redirect(const struct local_root *root, const char *request_line,
                                     const char *location_header) {
    http_request req = {0};
    CHECK(parse_request_line(request_line, &req) == 0);
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    CHECK(serve_static_at(pair[0], root, &req) == 0);
    shutdown(pair[0], SHUT_WR);
    char reply[512];
    CHECK(read_reply(pair[1], reply, sizeof(reply)) > 0);
    CHECK(strstr(reply, "HTTP/1.1 301 Moved Permanently\r\n") == reply);
    CHECK(strstr(reply, location_header) && strstr(reply, "\r\nContent-Length: 0\r\n"));
    CHECK(strstr(reply, "\r\nDate: ") && strstr(reply, "\r\nConnection: keep-alive\r\n"));
    CHECK_STR(strstr(reply, "\r\n\r\n"), "\r\n\r\n");
    close(pair[0]);
    close(pair[1]);
}

static void test_root_and_file_identity(void) {
    char temp[] = "/tmp/w13-files-XXXXXX";
    int cwd = open(".", O_RDONLY | O_DIRECTORY);
    require(cwd >= 0 && mkdtemp(temp) && chdir(temp) == 0, "temporary directory");
    require(mkdir("root", 0700) == 0, "mkdir");
    write_file("root/page.txt", "original", 0600);
    write_file("sibling", "outside", 0600);
    require(symlink("page.txt", "root/internal") == 0, "internal link");
    require(symlink("../sibling", "root/external") == 0, "external link");

    struct local_root root;
    init_local_root(&root, "root");
    require(root.status == 0, "open root");
    require(mkdir("root/sub", 0700) == 0, "mkdir sub");
    write_file("root/sub/index.html", "sub index", 0600);
    check_directory_redirect(&root, "GET /s%75b?lang=ko HTTP/1.1\r\n",
                             "\r\nLocation: /s%75b/?lang=ko\r\n");
    check_directory_redirect(&root, "HEAD ///sub? HTTP/1.1\r\n", "\r\nLocation: /sub/?\r\n");
    check_directory_redirect(&root, "GET /sub%2F HTTP/1.1\r\n", "\r\nLocation: /sub%2F/\r\n");
    http_request req = {0};
    int pair[2];
    CHECK(parse_request_line("GET /s%75b/?lang=ko HTTP/1.1\r\n", &req) == 0);
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    CHECK(serve_static_at(pair[0], &root, &req) == 0);
    check_reply(pair, "HTTP/1.1 200 OK\r\n", "sub index");
    CHECK(parse_request_line("GET /internal HTTP/1.1\r\n", &req) == 0);
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    CHECK(serve_static_at(pair[0], &root, &req) == 0);
    check_reply(pair, "HTTP/1.1 200 OK\r\n", "original");
    strcpy(req.path, "/external");
    CHECK(serve_static_at(-1, &root, &req) == 403);
    strcpy(req.path, "/cgi-bin/external");
    CHECK(serve_cgi_at(-1, &root, &req) == 403);

    char path[PATH_MAX];
    int file;
    require(open_local_file(&root, "/internal", 1, O_RDONLY, &file, path) == 0, "pin file");
    write_file("root/replacement", "changed", 0600);
    require(rename("root/replacement", "root/page.txt") == 0, "replace page");
    char content[16] = {0};
    CHECK(read(file, content, sizeof(content) - 1) == 8);
    CHECK_STR(content, "original");
    close(file);

    const char *script = "#!/bin/sh\nprintf 'Status: 400\\r\\nContent-Type: text/plain\\r\\n\\r\\nfirst'\n";
    write_file("root/run", script, 0700);
    require(symlink("run", "root/run-link") == 0, "script link");
    strcpy(req.path, "/cgi-bin/run-link");
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    CHECK(serve_cgi_at(pair[0], &root, &req) == 0);
    check_reply(pair, "HTTP/1.1 400 \r\n", "first");
#ifdef __linux__
    int program;
    require(open_local_file(&root, "run-link", 0, O_RDONLY, &program, path) == 0, "pin CGI");
    write_file("root/replacement", "not an executable\n", 0600);
    require(rename("root/replacement", "root/run") == 0, "replace CGI");
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    CHECK(serve_open_cgi(pair[0], program, path, &req) == 0);
    close(program);
    check_reply(pair, "HTTP/1.1 400 \r\n", "first");
#else
    require(chmod("root/run", 0600) == 0, "chmod");
#endif
    CHECK(serve_cgi_at(-1, &root, &req) == 403);

    close_local_root(&root);
    unlink("root/run-link");
    unlink("root/run");
    unlink("root/internal");
    unlink("root/external");
    unlink("root/page.txt");
    unlink("root/sub/index.html");
    rmdir("root/sub");
    unlink("sibling");
    rmdir("root");
    require(fchdir(cwd) == 0, "restore cwd");
    close(cwd);
    require(rmdir(temp) == 0, "remove temporary directory");
}

static void test_cgi_adder(void) {
    struct local_root root;
    init_local_root(&root, TEST_CGI_DIR);
    require(root.status == 0, "open cgi-bin");
    http_request req = {0};
    CHECK(parse_request_line("GET /cgi-bin/adder?a=17&b=25 HTTP/1.1\r\n", &req) == 0);
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    CHECK(serve_cgi_at(pair[0], &root, &req) == 0);
    check_reply(pair, "HTTP/1.1 200 OK\r\n", "42\n");
    close_local_root(&root);
}

static void *handle(void *arg) {
    handle_connection(*(int *)arg);
    return NULL;
}

static void test_error_on_wire(int head) {
    int pair[2];
    tcp_pair(pair);
    if (head) {
        require(dprintf(pair[1], "HEAD / HTTP/1.1\r\n\r\n") > 0, "HEAD");
    } else {
        require(dprintf(pair[1], "POST / HTTP/1.1\r\nHost: h\r\n"
                                 "Content-Length: 1048577\r\n\r\n") > 0, "POST");
        char pending[32768] = {0};
        require(rio_writen(pair[1], pending, sizeof(pending)) > 0, "queued request body");
    }
    pthread_t worker;
    require(pthread_create(&worker, NULL, handle, &pair[0]) == 0, "worker");
    char reply[512];
    CHECK(read_reply(pair[1], reply, sizeof(reply)) > 0);
    CHECK(strstr(reply, head ? "HTTP/1.1 400 " : "HTTP/1.1 413 ") == reply);
    char *body = strstr(reply, "\r\n\r\n");
    CHECK(body && (head ? !body[4] : strstr(body + 4, "413 Payload Too Large") != NULL));
    CHECK(strstr(reply, "Content-Length: ") && strstr(reply, "Connection: close\r\n"));
    close(pair[1]);
    pthread_join(worker, NULL);
}

int main(void) {
    char date[HTTP_DATE_SIZE];
    CHECK(format_http_date(0, date) && !strcmp(date, "Thu, 01 Jan 1970 00:00:00 GMT"));
    signal(SIGPIPE, SIG_IGN);
    alarm(8);
    test_root_and_file_identity();
    test_cgi_adder();
    test_error_on_wire(1);
    test_error_on_wire(0);
    return TEST_RESULT();
}
