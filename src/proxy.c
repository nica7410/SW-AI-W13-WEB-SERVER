#include "proxy.h"
#include "upstream.h"
#include "forward_proxy.h"
#include "http_fields.h"
#include "net.h"
#include "response_stream.h"
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int skip_header(const http_request *req, const char *name) {
    static const char *const local[] = {
        "Connection", "Proxy-Connection", "Keep-Alive", "Proxy-Authorization",
        "Proxy-Authenticate", "TE", "Trailer", "Upgrade", "Expect", "Host", "Content-Length"
    };
    for (size_t i = 0; i < sizeof(local) / sizeof(local[0]); i++)
        if (!strcasecmp(name, local[i])) return 1;
    for (int i = 0; i < req->header_count; i++)
        if (!strcasecmp(req->headers[i].name, "Connection") &&
            has_option(req->headers[i].value, name)) return 1;
    return 0;
}

static int send_field(int fd, const char *name, const char *value, deadline_ms end) {
    char line[MAX_LINE + 1];
    int n = snprintf(line, sizeof(line), "%s: %s\r\n", name, value);
    if (n < 0 || (size_t)n >= sizeof(line)) { errno = EINVAL; return -1; }
    return send_until(fd, line, (size_t)n, end);
}

static int write_request(int fd, const http_request *req, const char *target, deadline_ms end) {
    char line[MAX_LINE + 1];
    int n = snprintf(line, sizeof(line), "%s %s %s\r\n", req->method, target, req->version);
    if (n < 0 || (size_t)n >= sizeof(line)) { errno = EINVAL; return -1; }
    if (send_until(fd, line, (size_t)n, end) < 0) return -1;
    if (is_absolute_uri(req->uri)) {
        const char *host = req->uri + 7;
        int length = (int)strcspn(host, "/?");
        snprintf(line, sizeof(line), "%.*s", length, host);
        if (send_field(fd, "Host", line, end) < 0) return -1;
    } else {
        const char *host = get_header(req, "Host");
        if (host && send_field(fd, "Host", host, end) < 0) return -1;
    }
    for (int i = 0; i < req->header_count; i++) {
        const http_header *h = &req->headers[i];
        if (!skip_header(req, h->name) && send_field(fd, h->name, h->value, end) < 0)
            return -1;
    }
    snprintf(line, sizeof(line), "%s w13-proxy", req->version + 5);
    if (send_field(fd, "Via", line, end) < 0) return -1;
    if (get_header(req, "Content-Length")) {
        snprintf(line, sizeof(line), "%zu", req->content_length);
        if (send_field(fd, "Content-Length", line, end) < 0) return -1;
    }
    static const char close_header[] = "Connection: close\r\n\r\n";
    if (send_until(fd, close_header, sizeof(close_header) - 1, end) < 0) return -1;
    return send_until(fd, req->body, req->content_length, end);
}

int relay_until(int clientfd, int upstreamfd, const http_request *req, const char *target,
                deadline_ms end) {
    struct timeval tv = {.tv_sec = PROXY_TIMEOUT_SEC};
    if (setsockopt(upstreamfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
        setsockopt(upstreamfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return 502;
    if (write_request(upstreamfd, req, target, end) < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ? 504 : 502;
    return response_until(clientfd, upstreamfd, req, 0, end);
}

int proxy_relay(int clientfd, int upstreamfd, const http_request *req, const char *target) {
    return relay_until(clientfd, upstreamfd, req, target, deadline_after(PROXY_TIMEOUT_SEC * 1000));
}

int reverse_proxy_until(int clientfd, const http_request *req, route *r, deadline_ms end,
                        connect_fn *connect_backend) {
    const backend *first = next_backend(r);
    if (!first) return 502;
    int start = (int)(first - r->backends), error = 502;
    for (int i = 0; i < r->backend_count; i++) {
        const backend *b = &r->backends[(start + i) % r->backend_count];
        int left = deadline_left(end);
        if (!left) return 504;
        int share = left / (r->backend_count - i);
        deadline_ms attempt_end = end - left + (share ? share : 1);
        int upstream = connect_backend(b->host, b->port, attempt_end, getaddrinfo);
        if (upstream < 0) {
            if (errno == ETIMEDOUT) error = 504;
            if (!deadline_left(end)) return 504;
            continue;
        }
        // 연결 전 실패만 재시도한다. 이미 전달한 POST가 두 번 실행될 수 있다.
        int status = relay_until(clientfd, upstream, req, req->uri, end);
        close(upstream);
        return status;
    }
    return error;
}

int reverse_proxy_request(int clientfd, const http_request *req, route *r) {
    return reverse_proxy_until(clientfd, req, r, deadline_after(PROXY_TIMEOUT_SEC * 1000),
                               connect_until);
}
