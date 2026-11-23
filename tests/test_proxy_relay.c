#include "test.h"
#include "proxy.h"
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

static ssize_t finish_and_read(int pair[2], char *out, size_t capacity) {
    shutdown(pair[0], SHUT_WR);
    rio_t rio;
    rio_readinitb(&rio, pair[1]);
    ssize_t n = rio_readnb(&rio, out, capacity - 1);
    out[n < 0 ? 0 : n] = '\0';
    return n;
}

static void close_pair(int pair[2]) {
    close(pair[0]);
    close(pair[1]);
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
    memset(value, 'v', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';
    CHECK(dprintf(origin[1], "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                             "Via: 1.0 inner\r\nConnection: X-Local\r\nX-Local: drop\r\n"
                             "X:%s\r\n\r\n", value) > 0);
    CHECK(rio_writen(origin[1], chunks, sizeof(chunks) - 1) > 0);
    // No shutdown from origin: only the end of the trailer section completes this reply.
    CHECK(proxy_relay(client[0], origin[0], &req, "/x?q") == 0);

    char wire[MAX_LINE + 2048];
    ssize_t n = finish_and_read(origin, wire, sizeof(wire));
    CHECK(strstr(wire, "POST /x?q HTTP/1.1\r\nHost: origin:81\r\n") == wire);
    CHECK(strstr(wire, "X-Local") == NULL && strstr(wire, "old-host") == NULL);
    CHECK(strstr(wire, "Via: 1.0 previous\r\n") && strstr(wire, "Via: 1.1 w13-proxy\r\n"));
    CHECK(strstr(wire, "Content-Length: 3\r\n") && strstr(wire, "Connection: close\r\n"));
    char *separator = strstr(wire, "\r\n\r\n");
    CHECK(separator && n == separator + 4 - wire + (ssize_t)sizeof(post) &&
          !memcmp(separator + 4, post, sizeof(post)));

    n = finish_and_read(client, wire, sizeof(wire));
    CHECK(strstr(wire, "HTTP/1.1 200 OK\r\n") == wire);
    CHECK(!strstr(wire, "X-Local") && strstr(wire, "Via: 1.0 inner\r\n") &&
          strstr(wire, "Via: 1.1 w13-proxy\r\n"));
    const char *field = strstr(wire, "\r\nX: ");
    if (field) field += strlen("\r\nX: ");
    CHECK(field && strspn(field, "v") == sizeof(value) - 1 &&
          !strncmp(field + sizeof(value) - 1, "\r\n", 2));
    separator = strstr(wire, "\r\n\r\n");
    CHECK(separator && n == separator + 4 - wire + (ssize_t)sizeof(chunks) - 1 &&
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
    test_deadline();
    return TEST_RESULT();
}
