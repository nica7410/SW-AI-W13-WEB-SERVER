#include "test.h"
#include "proxy.h"
#include "response_stream.h"
#include "upstream.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void make_pair(int pair[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) { perror("socketpair"); exit(1); }
}

static ssize_t read_wire(int fd, char *out, size_t capacity) {
    rio_t rio;
    rio_readinitb(&rio, fd);
    ssize_t n = rio_readnb(&rio, out, capacity - 1);
    out[n < 0 ? 0 : n] = '\0';
    return n;
}

static ssize_t finish_and_read(int pair[2], char *out, size_t capacity) {
    shutdown(pair[0], SHUT_WR);
    return read_wire(pair[1], out, capacity);
}

static void close_pair(int pair[2]) {
    close(pair[0]);
    close(pair[1]);
}

struct relay_fixture {
    int client, origin, result;
    const http_request *req;
};

static void *relay_forward_post(void *arg) {
    struct relay_fixture *f = arg;
    f->result = proxy_relay(f->client, f->origin, f->req, "/x?q");
    shutdown(f->client, SHUT_WR);
    return NULL;
}

static void test_forward_post_and_chunked_response(void) {
    int client[2], origin[2];
    make_pair(client);
    make_pair(origin);
    http_request req = {0};
    CHECK(parse_request_line("POST http://origin:81/x?q HTTP/1.1\r\n", &req) == 0);
    CHECK(parse_header_line("Host: old-host\r\n", &req) == 0);
    CHECK(parse_header_line("Connection: X-Local\r\n", &req) == 0);
    CHECK(parse_header_line("X-Local: drop\r\n", &req) == 0);
    CHECK(parse_header_line("Via: 1.0 previous\r\n", &req) == 0);
    CHECK(parse_header_line("Content-Length: 3\r\n", &req) == 0);
    char post[] = {'a', '\0', 'b'};
    req.body = post;

    static const char chunks[] = "3;tag=\"a;b\"\r\nx\0y\r\n0\r\nX-Sum: ok\r\n\r\n";
    // "X:" + value + CRLF exactly fills the accepted input line.
    char value[MAX_LINE - 3];
    // The header can exceed the socket buffer. Run the relay while its peers write and read.
    struct relay_fixture relay = {.client = client[0], .origin = origin[0], .req = &req};
    pthread_t thread;
    if (pthread_create(&thread, NULL, relay_forward_post, &relay)) exit(1);
    memset(value, 'v', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';
    CHECK(dprintf(origin[1], "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                             "Via: 1.0 inner\r\nDate: Wed, 01 Jan 2020 00:00:00 GMT\r\n"
                             "Connection: X-Local\r\nX-Local: drop\r\n"
                             "X:%s\r\n\r\n", value) > 0);
    CHECK(rio_writen(origin[1], chunks, sizeof(chunks) - 1) > 0);
    // No shutdown from origin: only the end of the trailer section completes this reply.
    char response[MAX_LINE + 2048];
    ssize_t response_size = read_wire(client[1], response, sizeof(response));
    pthread_join(thread, NULL);
    CHECK(relay.result == 0);

    char request[512];
    ssize_t request_size = finish_and_read(origin, request, sizeof(request));
    CHECK(strstr(request, "POST /x?q HTTP/1.1\r\nHost: origin:81\r\n") == request);
    CHECK(strstr(request, "X-Local") == NULL && strstr(request, "old-host") == NULL);
    CHECK(strstr(request, "Via: 1.0 previous\r\n") && strstr(request, "Via: 1.1 w13-proxy\r\n"));
    CHECK(strstr(request, "Content-Length: 3\r\n") && strstr(request, "Connection: close\r\n"));
    char *separator = strstr(request, "\r\n\r\n");
    CHECK(separator && request_size == separator + 4 - request + (ssize_t)sizeof(post) &&
          !memcmp(separator + 4, post, sizeof(post)));

    CHECK(strstr(response, "HTTP/1.1 200 OK\r\n") == response);
    char *date = strstr(response, "\r\nDate: Wed, 01 Jan 2020 00:00:00 GMT\r\n");
    CHECK(date && !strstr(date + 1, "\r\nDate: "));
    CHECK(!strstr(response, "X-Local") && strstr(response, "Via: 1.0 inner\r\n") &&
          strstr(response, "Via: 1.1 w13-proxy\r\n"));
    const char *field = strstr(response, "\r\nX: ");
    if (field) field += strlen("\r\nX: ");
    CHECK(field && strspn(field, "v") == sizeof(value) - 1 &&
          !strncmp(field + sizeof(value) - 1, "\r\n", 2));
    separator = strstr(response, "\r\n\r\n");
    CHECK(separator && response_size == separator + 4 - response + (ssize_t)sizeof(chunks) - 1 &&
          !memcmp(separator + 4, chunks, sizeof(chunks) - 1));
    close_pair(client);
    close_pair(origin);
}

static void test_truncated_chunk(void) {
    int client[2], origin[2];
    make_pair(client);
    make_pair(origin);
    http_request req = {0};
    CHECK(parse_request_line("GET / HTTP/1.1\r\n", &req) == 0);
    CHECK(dprintf(origin[1], "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nxy") > 0);
    shutdown(origin[1], SHUT_WR);
    CHECK(proxy_relay(client[0], origin[0], &req, "/") == -1);
    char wire[512];
    finish_and_read(client, wire, sizeof(wire));
    CHECK(strstr(wire, "HTTP/1.1 200 OK\r\n") == wire);
    CHECK(!strstr(wire + 1, "HTTP/") && !strstr(wire, "0\r\n\r\n"));
    close_pair(client);
    close_pair(origin);
}

static void *write_finite_body(void *arg) {
    int fd = *(int *)arg;
    for (int i = 0; i < 15; i++) {
        if (write(fd, "x", 1) != 1) break;
        struct timespec pause = {.tv_nsec = 20000000};
        nanosleep(&pause, NULL);
    }
    shutdown(fd, SHUT_WR);
    return NULL;
}

static void test_total_response_deadline(void) {
    int client[2], origin[2];
    make_pair(client);
    make_pair(origin);
    http_request req = {0};
    CHECK(parse_request_line("GET / HTTP/1.1\r\n", &req) == 0);
    CHECK(dprintf(origin[1], "HTTP/1.1 200 OK\r\nContent-Length: 16\r\n\r\nx") > 0);
    pthread_t writer;
    if (pthread_create(&writer, NULL, write_finite_body, &origin[1])) exit(1);
    deadline_ms end = deadline_after(100);
    CHECK(response_until(client[0], origin[0], &req, 0, end, end) == -1);
    char wire[512];
    finish_and_read(client, wire, sizeof(wire));
    char *body = strstr(wire, "\r\n\r\n");
    CHECK(strstr(wire, "HTTP/1.1 200 OK") == wire && !strstr(wire + 1, "HTTP/"));
    CHECK(body && body[4] == 'x' && strlen(body + 4) < 16);
    pthread_join(writer, NULL);
    close_pair(origin);
    close_pair(client);
}

static int connect_calls, healthy_fd;
static deadline_ms request_end;

static int connect_candidate(const char *host, const char *port, deadline_ms end, lookup_fn *lookup,
                             int *peer_failed) {
    (void)port;
    (void)lookup;
    if (++connect_calls == 1) {
        CHECK_STR(host, "first");
        CHECK(end < request_end && deadline_left(end) > 0);
        errno = ETIMEDOUT;
        *peer_failed = 1;
        return -1;
    }
    CHECK_STR(host, "second");
    CHECK(end == request_end);
    return healthy_fd;
}

static void test_connection_failover(void) {
    int client[2], origin[2];
    make_pair(client);
    make_pair(origin);
    healthy_fd = origin[0];
    route r;
    CHECK(parse_route_line("/api/ first:80 second:80", &r) == 1);
    http_request req = {0};
    CHECK(parse_request_line("POST /api/ HTTP/1.1\r\n", &req) == 0);
    CHECK(parse_header_line("Content-Length: 1\r\n", &req) == 0);
    req.body = "x";
    CHECK(dprintf(origin[1], "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok") > 0);
    request_end = deadline_after(1000);
    CHECK(reverse_proxy_until(client[0], &req, &r, request_end, connect_candidate) == 0);
    CHECK(connect_calls == 2);
    char wire[512];
    rio_t rio;
    rio_readinitb(&rio, origin[1]);
    ssize_t n = rio_readnb(&rio, wire, sizeof(wire) - 1);
    wire[n > 0 ? n : 0] = '\0';
    CHECK(strstr(wire, "POST /api/ ") == wire && !strstr(wire + 1, "POST "));
    CHECK(strstr(wire, "\r\n\r\nx") != NULL);
    finish_and_read(client, wire, sizeof(wire));
    CHECK(strstr(wire, "HTTP/1.1 200 OK") == wire && strstr(wire, "\r\n\r\nok"));
    char *date = strstr(wire, "\r\nDate: ");
    CHECK(date && !strstr(date + 1, "\r\nDate: "));
    close(origin[1]);
    close_pair(client);
}

static void test_cooldown_recovery(void) {
    route r;
    CHECK(parse_route_line("/api/ first:80 second:80", &r) == 1);
    backend_attempt a = {0}, b = {0};
    CHECK(select_backend(&r, 0, 10, &a) == 2 && a.index == 0);
    finish_backend(&r, &a, 0, 1, 10);
    CHECK(select_backend(&r, 1, 11, &a) == 1 && a.index == 1);
    finish_backend(&r, &a, 1, 0, 11);
    CHECK(select_backend(&r, 0, 12, &a) == 1 && a.index == 1);
    finish_backend(&r, &a, 0, 1, 12);
    CHECK(!select_backend(&r, 0, 13, &a));
    CHECK(select_backend(&r, 0, 5010, &a) == 1 && a.index == 0);
    CHECK(!select_backend(&r, 0, 5010, &b));
    finish_backend(&r, &a, 1, 0, 5010);
    CHECK(select_backend(&r, 0, 5011, &b) == 1 && b.index == 0);
    finish_backend(&r, &b, 1, 0, 5011);

    // 같은 후보를 고른 두 워커 중 나중 워커가 로컬 오류로 먼저 끝나는 경우.
    CHECK(select_backend(&r, 0, 5011, &a) == 1 && a.index == 0);
    CHECK(select_backend(&r, 0, 5011, &b) == 1 && b.index == 0);
    finish_backend(&r, &b, 0, 0, 5011);
    finish_backend(&r, &a, 0, 1, 5011);
    CHECK(!select_backend(&r, 0, 5011, &b));
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_key_t resolver_key;
static int release_lookup, resolver_finished;

// POSIX calls the destructor after resolve_job returns and frees its abandoned query.
static void resolver_exited(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    resolver_finished = 1;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&lock);
}

static int delayed_lookup(const char *host, const char *port, const struct addrinfo *hints,
                          struct addrinfo **addresses) {
    (void)host;
    if (pthread_setspecific(resolver_key, &resolver_finished) != 0) return EAI_FAIL;
    pthread_mutex_lock(&lock);
    while (!release_lookup) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    return getaddrinfo("127.0.0.1", port, hints, addresses);
}

static void test_deadline(void) {
    int rc = pthread_key_create(&resolver_key, resolver_exited);
    CHECK(rc == 0);
    if (rc) return;
    deadline_ms end = deadline_after(80);
    CHECK(connect_until("stub.invalid", "80", end, delayed_lookup) == -1 && errno == ETIMEDOUT);
    pthread_mutex_lock(&lock);
    release_lookup = 1;
    pthread_cond_broadcast(&changed);
    struct timespec until;
    rc = clock_gettime(CLOCK_REALTIME, &until);
    if (!rc) {
        until.tv_sec += 2;
        while (!resolver_finished && !rc) rc = pthread_cond_timedwait(&changed, &lock, &until);
    }
    int finished = resolver_finished;
    pthread_mutex_unlock(&lock);
    CHECK(finished);
    if (finished) CHECK(pthread_key_delete(resolver_key) == 0);

    int client[2], origin[2];
    make_pair(client);
    make_pair(origin);
    http_request req = {0};
    CHECK(parse_request_line("GET / HTTP/1.1\r\n", &req) == 0);
    CHECK(relay_until(client[0], origin[0], &req, "/", end) == 504);
    char byte;
    CHECK(recv(origin[1], &byte, 1, MSG_DONTWAIT) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));

    // A partial status line must obey the remaining header deadline on the next refill.
    CHECK(rio_writen(origin[1], "HTTP/1.", 7) == 7);
    deadline_ms header_end = deadline_after(80);
    CHECK(relay_until(client[0], origin[0], &req, "/", header_end) == 504);
    CHECK(deadline_after(0) < header_end + 1000);
    close_pair(client);
    close_pair(origin);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    alarm(8);
    test_forward_post_and_chunked_response();
    test_truncated_chunk();
    test_total_response_deadline();
    test_connection_failover();
    test_cooldown_recovery();
    test_deadline();
    return TEST_RESULT();
}
