#include "test.h"
#include "http_request_internal.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void test_request_lines(void) {
    http_request req = {0};
    CHECK(parse_request_line("GET /index.html?a=1 HTTP/1.1\r\n", &req) == 0);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.path, "/index.html");
    CHECK_STR(req.query, "a=1");
    CHECK(req.keep_alive == 1);
    CHECK(parse_request_line("POST / HTTP/1.0\r\n", &req) == 0);
    CHECK(req.keep_alive == 0);
    CHECK(parse_request_line("HEAD http://host?x HTTP/1.1\r\n", &req) == 0);
    CHECK_STR(req.path, "/");
    CHECK_STR(req.query, "x");
    CHECK(parse_request_line("GET /\r\n", &req) == 400);
    CHECK(parse_request_line("GET / HTTP/1.1 extra\r\n", &req) == 400);
    CHECK(parse_request_line("DELETE / HTTP/1.1\r\n", &req) == 501);
    CHECK(parse_request_line("GET / HTTP/2.0\r\n", &req) == 505);

    char uri[MAX_URI_LEN + 1], line[MAX_URI_LEN + 32];
    memset(uri, 'x', sizeof(uri) - 1);
    uri[0] = '/';
    uri[MAX_URI_LEN] = '\0';
    snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\n", uri);
    CHECK(parse_request_line(line, &req) == 414);
}

static void test_headers(void) {
    http_request req = {0};
    CHECK(parse_header_line("Host:  localhost \t\r\n", &req) == 0);
    CHECK_STR(get_header(&req, "host"), "localhost");
    CHECK(parse_header_line("Host: duplicate\r\n", &req) == 400);
    CHECK(parse_header_line("Content-Length: xyz\r\n", &req) == 400);
    CHECK(parse_header_line("Content-Length: 1048577\r\n", &req) == 413);
    CHECK(parse_header_line("Content-Length: 1048576\r\n", &req) == 0);
    CHECK(req.content_length == MAX_BODY_LEN);
    CHECK(parse_header_line("Content-Length: 20\r\n", &req) == 400);
    CHECK(parse_header_line("Connection: keep-alive\r\n", &req) == 0);
    CHECK(req.keep_alive == 1);
    CHECK(parse_header_line("Connection: upgrade, CLOSE\r\n", &req) == 0);
    CHECK(req.keep_alive == 0);
    CHECK(parse_header_line("Bad field: x\r\n", &req) == 400);
    CHECK(parse_header_line("Missing colon\r\n", &req) == 400);
    while (req.header_count < MAX_HEADERS) {
        int status = parse_header_line("X-Test: 1\r\n", &req);
        CHECK(status == 0);
        if (status) break;
    }
    CHECK(parse_header_line("X-Extra: 1\r\n", &req) == 431);
}

static void test_host_syntax(void) {
    const char *valid[] = {"example.com:8001", "[::1]", "[2001:db8::1]:443"};
    const char *invalid[] = {"example.com:abc", "[::1", "::1", ":80", "host:99999"};
    char line[128];
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        http_request req = {0};
        snprintf(line, sizeof(line), "Host: %s\r\n", valid[i]);
        CHECK(parse_header_line(line, &req) == 0);
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        http_request req = {0};
        snprintf(line, sizeof(line), "Host: %s\r\n", invalid[i]);
        CHECK(parse_header_line(line, &req) == 400);
    }
}

static void test_expect(const char *version, const char *expected) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) { CHECK(0); return; }
    CHECK(dprintf(pair[1], "POST / HTTP/%s\r\nHost: h\r\nExpect: 100-continue\r\n"
                           "Content-Length: 1\r\n\r\nx", version) > 0);
    shutdown(pair[1], SHUT_WR);
    rio_t in, out;
    rio_readinitb(&in, pair[0]);
    http_request req;
    CHECK(read_request(&in, &req) == 0);
    CHECK(req.content_length == 1 && req.body && req.body[0] == 'x');
    free_request(&req);
    shutdown(pair[0], SHUT_WR);
    char reply[64] = {0};
    rio_readinitb(&out, pair[1]);
    CHECK(rio_readnb(&out, reply, sizeof(reply) - 1) == (ssize_t)strlen(expected));
    CHECK_STR(reply, expected);
    close(pair[0]);
    close(pair[1]);
}

static void test_body_io_error(int fail_continue) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) { CHECK(0); return; }
    // A complete body isolates the interim write failure. A short body forces EOF.
    if (fail_continue) CHECK(shutdown(pair[0], SHUT_WR) == 0);
    CHECK(dprintf(pair[1], "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n%s\r\n%s",
                  fail_continue ? "Expect: 100-continue\r\n" : "", fail_continue ? "xy" : "x") > 0);
    CHECK(shutdown(pair[1], SHUT_WR) == 0);
    rio_t in;
    rio_readinitb(&in, pair[0]);
    http_request req;
    CHECK(read_request(&in, &req) == -1);
    CHECK(req.body == NULL);
    free_request(&req);
    close(pair[0]);
    close(pair[1]);
}

struct deadline_probe {
    int peer;
    deadline_ms end;
    size_t reads;
};

static struct deadline_probe *probe;

ssize_t test_request_readline_until(rio_t *rp, void *buf, size_t maxlen, deadline_ms end) {
    if (!probe) return rio_readline_until(rp, buf, maxlen, end);
    if (end != probe->end) {
        CHECK(end == probe->end);
        errno = EINVAL;
        return -1;
    }
    static const char *const lines[] = {"GET / HTTP/1.1\r\n", "Host: h\r\n", "Accept: */*\r\n"};
    size_t index = probe->reads++;
    if (index < sizeof(lines) / sizeof(lines[0])) {
        if (index == 1) {
            // Cross a monotonic clock tick so restarting the budget changes the deadline.
            struct timespec pause = {.tv_nsec = 10 * 1000 * 1000};
            while (nanosleep(&pause, &pause) < 0)
                if (errno != EINTR) return -1;
        }
        if (rio_writen(probe->peer, lines[index], strlen(lines[index])) < 0) return -1;
    }
    // The fourth read waits for the missing empty line on the real socket.
    return rio_readline_until(rp, buf, maxlen, end);
}

static void test_header_deadline(void) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) { CHECK(0); return; }
    // Bound this test even if the parser falls back to an ordinary blocking read.
    struct timeval fallback = {.tv_sec = 2};
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &fallback, sizeof(fallback)) == 0);
    rio_t in;
    rio_readinitb(&in, pair[0]);
    http_request req = {0};
    struct deadline_probe observation = {.peer = pair[1], .end = deadline_after(200)};
    probe = &observation;
    int result = read_request_head_until(&in, &req, observation.end);
    int error = errno;
    deadline_ms returned_at = deadline_after(0);
    probe = NULL;
    CHECK(result == 408 && error == ETIMEDOUT);
    CHECK(observation.reads == 4 && req.header_count == 2);
    CHECK(returned_at >= observation.end && returned_at < observation.end + 1500);
    close(pair[0]);
    close(pair[1]);
}

// 본문이 조금씩 도착해도 마감 시각은 처음 정한 값에서 늘어나지 않는다.
static void *trickle(void *arg) {
    int fd = *(int *)arg;
    for (int i = 0; i < 20; i++) {
        struct timespec pause = {.tv_nsec = 50 * 1000 * 1000};
        nanosleep(&pause, NULL);
        if (write(fd, "b", 1) != 1) break;
    }
    return NULL;
}

static void test_body_deadline(void) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) { CHECK(0); return; }
    struct timeval fallback = {.tv_sec = 2};
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &fallback, sizeof(fallback)) == 0);
    CHECK(write(pair[1], "aaa", 3) == 3);
    pthread_t writer;
    CHECK(pthread_create(&writer, NULL, trickle, &pair[1]) == 0);
    rio_t in;
    rio_readinitb(&in, pair[0]);
    char body[64];
    deadline_ms end = deadline_after(200);
    ssize_t n = rio_readn_until(&in, body, sizeof(body), end);
    int error = errno;
    deadline_ms returned_at = deadline_after(0);
    CHECK(n == -1 && error == ETIMEDOUT);
    CHECK(returned_at >= end && returned_at < end + 500);
    pthread_join(writer, NULL);
    close(pair[0]);
    close(pair[1]);
}

static void test_chunked_request(void) {
    static const char wire[] =
        "POST /upload HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: Chunked\r\n\r\n"
        "2;tag=\"x;y\"\r\na\0\r\n1\r\nb\r\n0\r\nX-Sum: ok\r\n\r\n"
        "GET /next HTTP/1.1\r\nHost: h\r\n\r\n";
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) { CHECK(0); return; }
    CHECK(rio_writen(pair[1], wire, sizeof(wire)-1) == (ssize_t)sizeof(wire)-1);
    shutdown(pair[1], SHUT_WR);
    rio_t in;
    rio_readinitb(&in, pair[0]);
    http_request req;
    CHECK(read_request(&in, &req) == 0);
    CHECK(req.content_length == 3 && req.body && !memcmp(req.body, "a\0b", 3));
    CHECK(get_header(&req, "X-Sum") == NULL);
    free_request(&req);
    CHECK(read_request(&in, &req) == 0 && !strcmp(req.path, "/next"));
    free_request(&req);
    close(pair[0]);
    close(pair[1]);
}

static void check_chunk_error(const char *fields_and_chunks, int expected) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) { CHECK(0); return; }
    CHECK(dprintf(pair[1], "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n%s",
                  fields_and_chunks) > 0);
    shutdown(pair[1], SHUT_WR);
    rio_t in;
    rio_readinitb(&in, pair[0]);
    http_request req;
    CHECK(read_request(&in, &req) == expected);
    CHECK(req.body == NULL);
    close(pair[0]);
    close(pair[1]);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_host_syntax();
    test_expect("1.0", "");
    test_expect("1.1", "HTTP/1.1 100 Continue\r\n\r\n");
    test_header_deadline();
    test_body_deadline();
    test_chunked_request();
    check_chunk_error("Content-Length: 0\r\n\r\n", 400);
    check_chunk_error("\r\n100001\r\n", 413);
    check_chunk_error("\r\n1\r\nx!!", 400);
    check_chunk_error("\r\n0\r\nHost: changed\r\n\r\n", 400);
    test_body_io_error(0);
    test_body_io_error(1);
    test_request_lines();
    test_headers();
    return TEST_RESULT();
}
