#include "proxy.h"
#include "proxy_identity.h"
#include "access_log.h"
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

static int write_request(int fd, const http_request *req, const char *target, deadline_ms end,
                         const char *reverse_peer, const char *identity) {
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
        if (reverse_peer && (!strcasecmp(h->name, "X-Forwarded-For") ||
                             !strcasecmp(h->name, "X-Forwarded-Proto"))) continue;
        if (!skip_header(req, h->name) && send_field(fd, h->name, h->value, end) < 0)
            return -1;
    }
    snprintf(line, sizeof(line), "%s %s", req->version + 5, identity);
    if (reverse_peer) {
        if (*reverse_peer && send_field(fd, "X-Forwarded-For", reverse_peer, end) < 0) return -1;
        if (send_field(fd, "X-Forwarded-Proto", "http", end) < 0) return -1;
    }
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
                deadline_ms end, const char *reverse_peer) {
    if (proxy_returns_to_hop(clientfd, upstreamfd, req)) return 508;
    char identity[ENDPOINT_SIZE] = "w13-proxy";
    proxy_endpoint(clientfd, 0, identity);
    struct timeval tv = {.tv_sec = PROXY_TIMEOUT_SEC};
    if (setsockopt(upstreamfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
        setsockopt(upstreamfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return 502;
    if (write_request(upstreamfd, req, target, end, reverse_peer, identity) < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ? 504 : 502;
    return response_until(clientfd, upstreamfd, req, 0, end, deadline_after(RESPONSE_TIMEOUT_MS));
}

int proxy_relay(int clientfd, int upstreamfd, const http_request *req, const char *target) {
    return relay_until(clientfd, upstreamfd, req, target, deadline_after(PROXY_TIMEOUT_SEC * 1000), NULL);
}

static const char *peer_ip(int fd, char ip[NI_MAXHOST]) {
    struct sockaddr_storage peer;
    socklen_t len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &len) < 0 ||
        (peer.ss_family != AF_INET && peer.ss_family != AF_INET6)) return "";
    int rc = getnameinfo((struct sockaddr *)&peer, len, ip, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
    return rc == 0 ? ip : "";
}

int reverse_proxy_until(int clientfd, const http_request *req, route *r, deadline_ms end,
                        connect_fn *connect_backend) {
    if (via_revisits_listener(clientfd, req)) return 508;
    unsigned tried = 0;
    int error = 0, loop_seen = 0;
    backend_attempt attempt = {0};
    while (deadline_left(end) && select_backend(r, tried, deadline_after(0), &attempt)) {
        const backend *b = &r->backends[attempt.index];
        tried |= 1u << attempt.index;
        end = effective_deadline(end);
        int left = deadline_left(end);
        if (!left) {
            finish_backend(r, &attempt, 0, 0, deadline_after(0));
            return 504;
        }
        if (is_listener_authority(clientfd, b->host, b->port, NULL)) {
            finish_backend(r, &attempt, 0, 0, deadline_after(0));
            loop_seen = 1;
            continue;
        }
        int share = left / attempt.eligible;
        deadline_ms attempt_end = end - left + (share ? share : 1);
        int peer_failed = 0;
        access_attempt(b);
        int upstream = connect_backend(b->host, b->port, attempt_end, getaddrinfo, &peer_failed);
        int connect_error = errno;
        int backend_failure = peer_failed && deadline_left(end) &&
                              effective_deadline(INT64_MAX) == INT64_MAX;
        finish_backend(r, &attempt, upstream >= 0, backend_failure, deadline_after(0));
        if (upstream < 0) {
            if (connect_error == ETIMEDOUT) error = 504;
            else if (!error) error = 502;
            if (!deadline_left(end)) return 504;
            continue;
        }
        // 연결 전 실패만 재시도한다. 이미 전달한 POST가 두 번 실행될 수 있다.
        access_connected(b);
        char ip[NI_MAXHOST];
        int status = relay_until(clientfd, upstream, req, req->uri, end, peer_ip(clientfd, ip));
        close(upstream);
        if (status == 508) {
            loop_seen = 1;
            continue;   // relay_until detected a loop before writing any request bytes
        }
        return status;
    }
    if (!deadline_left(end)) return 504;
    if (error) return error;
    return loop_seen ? 508 : 502;
}

int reverse_proxy_request(int clientfd, const http_request *req, route *r) {
    return reverse_proxy_until(clientfd, req, r, deadline_after(PROXY_TIMEOUT_SEC * 1000),
                               connect_for_route);
}
