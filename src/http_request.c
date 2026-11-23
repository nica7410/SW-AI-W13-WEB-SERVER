#include "http_request_internal.h"
#include "forward_proxy.h"
#include "http_fields.h"
#include "http_framing.h"
#include "proxy_internal.h"
#include "server.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>

#define REQUEST_HEADER_TIMEOUT_MS 5000
#define REQUEST_BODY_TIMEOUT_MS   5000   // 본문 전체(최대 1MB)를 받는 시간. 느린 업로드가 워커를 붙잡지 못하게 한다

int parse_request_line(const char *line, http_request *req) {
    size_t len = strcspn(line, "\r\n");
    const char *end = line + len;
    if (*end && strcmp(end, "\r\n") != 0) return 400;
    const char *first = memchr(line, ' ', len);
    if (!first || first == line) return 400;
    const char *uri = first + 1;
    const char *second = memchr(uri, ' ', (size_t)(end - uri));
    if (!second || second == uri || second + 1 == end) return 400;
    for (const char *p = line; p < first; p++)
        if (!is_field_char((unsigned char)*p)) return 400;
    size_t method_len = (size_t)(first - line);
    if (method_len >= sizeof(req->method)) return 501;
    memcpy(req->method, line, method_len);
    req->method[method_len] = '\0';
    if (strcmp(req->method, "GET") && strcmp(req->method, "POST") &&
        strcmp(req->method, "HEAD")) return 501;

    size_t uri_len = (size_t)(second - uri);
    if (uri_len >= sizeof(req->uri)) return 414;
    for (const char *p = uri; p < second; p++)
        if ((unsigned char)*p <= 32 || *p == 127 || *p == '#') return 400;
    memcpy(req->uri, uri, uri_len);
    req->uri[uri_len] = '\0';

    const char *version = second + 1;
    if (memchr(version, ' ', (size_t)(end - version))) return 400;
    if (end - version != 8 ||
        (memcmp(version, "HTTP/1.0", 8) && memcmp(version, "HTTP/1.1", 8))) return 505;
    memcpy(req->version, version, 8);
    req->version[8] = '\0';
    req->keep_alive = version[7] == '1';

    if (is_absolute_uri(req->uri)) {
        char host[256], port[8];
        if (parse_absolute_uri(req->uri, host, sizeof(host), port, sizeof(port),
                               req->path, sizeof(req->path)) < 0) return 400;
    } else {
        if (req->uri[0] != '/') return 400;
        strcpy(req->path, req->uri);
    }
    char *query = strchr(req->path, '?');
    if (query) *query++ = '\0';
    strcpy(req->query, query ? query : "");
    return 0;
}

const char *get_header(const http_request *req, const char *name) {
    for (int i = 0; i < req->header_count; i++)
        if (!strcasecmp(req->headers[i].name, name)) return req->headers[i].value;
    return NULL;
}

static int parse_field(const char *line, http_header *h) {
    size_t len = strcspn(line, "\r\n");
    if (line[len] && strcmp(line + len, "\r\n")) return 400;
    const char *colon = memchr(line, ':', len);
    if (!colon || colon == line) return 400;
    size_t name_len = (size_t)(colon - line);
    const char *value = colon + 1, *end = line + len;
    while (value < end && (*value == ' ' || *value == '\t')) value++;
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t value_len = (size_t)(end - value);
    if (name_len >= MAX_HEADER_NAME ||
        value_len >= MAX_HEADER_VALUE) return 431;
    for (const char *p = line; p < colon; p++)
        if (!is_field_char((unsigned char)*p)) return 400;
    for (const char *p = value; p < end; p++)
        if (((unsigned char)*p < 32 && *p != '\t') || *p == 127) return 400;

    memcpy(h->name, line, name_len);
    h->name[name_len] = '\0';
    memcpy(h->value, value, value_len);
    h->value[value_len] = '\0';
    return 0;
}

int parse_header_line(const char *line, http_request *req) {
    if (req->header_count == MAX_HEADERS) return 431;
    http_header *h = &req->headers[req->header_count];
    int status = parse_field(line, h);
    if (status) return status;
    size_t value_len = strlen(h->value);
    if (!strcasecmp(h->name, "Content-Length")) {
        if (!value_len || get_header(req, h->name)) return 400;
        size_t n = 0;
        for (const char *p = h->value; *p; p++) {
            if (*p < '0' || *p > '9') return 400;
            n = n * 10 + (size_t)(*p - '0');
            if (n > MAX_BODY_LEN) return 413;
        }
        req->content_length = n;
    } else if (!strcasecmp(h->name, "Host")) {
        char host[MAX_HEADER_VALUE], port[8];
        if (get_header(req, "Host") ||
            parse_authority(h->value, value_len, "80", host, sizeof(host), port, sizeof(port)) < 0)
            return 400;
    } else if (!strcasecmp(h->name, "Connection")) {
        if (has_option(h->value, "close")) req->keep_alive = 0;
        else if (has_option(h->value, "keep-alive")) {
            req->keep_alive = 1;
            for (int i = 0; i < req->header_count; i++)
                if (!strcasecmp(req->headers[i].name, "Connection") &&
                    has_option(req->headers[i].value, "close")) req->keep_alive = 0;
        }
    }
    req->header_count++;
    return 0;
}

static int read_line(rio_t *rp, char *line, int too_long, deadline_ms end) {
    ssize_t n = rio_readline_until(rp, line, MAX_LINE + 1, end);
    if (n < 0 && errno == ETIMEDOUT) return 408;   // 요청을 보내기 시작했지만 마감까지 헤더를 다 못 보냄
    if (n <= 0) return -1;
    if (memchr(line, '\0', (size_t)n)) return 400;
    if (line[n - 1] != '\n' && n == MAX_LINE) return too_long;
    if (n < 2 || line[n - 2] != '\r' || line[n - 1] != '\n') return 400;
    return 0;
}

int read_request_head_until(rio_t *rp, http_request *req, deadline_ms end) {
    char line[MAX_LINE + 1];
    int status = read_line(rp, line, 414, end);
    if (status) return status;
    status = parse_request_line(line, req);
    if (status) return status;
    for (;;) {
        status = read_line(rp, line, 431, end);
        if (status) return status;
        if (!strcmp(line, "\r\n")) return 0;
        status = parse_header_line(line, req);
        if (status) return status;
    }
}

static int read_body_bytes(rio_t *rp, char *out, size_t n, deadline_ms end) {
    ssize_t got = rio_readn_until(rp, out, n, end);
    if (got == (ssize_t)n) return 0;
    return got < 0 && errno == ETIMEDOUT ? 408 : -1;
}

static int request_coding(const http_request *req) {
    int chunked = 0, unsupported = 0;
    if (get_header(req, "Content-Length") || !strcmp(req->version, "HTTP/1.0")) return 400;
    for (int i = 0; i < req->header_count; i++) {
        const http_header *h = &req->headers[i];
        if (strcasecmp(h->name, "Transfer-Encoding")) continue;
        if (!parse_transfer_codings(h->value, &chunked)) return 400;
        if (strcasecmp(h->value, "chunked")) unsupported = 1;
    }
    if (!chunked) return 400;
    return unsupported ? 501 : 0;
}

static int parse_expectations(const http_request *req, int *expect_continue) {
    static const char supported[] = "100-continue";
    int present = 0;
    *expect_continue = 0;
    for (int i = 0; i < req->header_count; i++) {
        const http_header *h = &req->headers[i];
        if (strcasecmp(h->name, "Expect")) continue;
        present = 1;
        const char *p = h->value;
        while (*p) {
            p += strspn(p, " \t,");
            if (!*p) break;
            const char *end = p + strcspn(p, ",");
            const char *trim = end;
            while (trim > p && (trim[-1] == ' ' || trim[-1] == '\t')) trim--;
            if ((size_t)(trim - p) != sizeof(supported) - 1 ||
                strncasecmp(p, supported, sizeof(supported) - 1)) return 417;
            *expect_continue = 1;
            p = end;
        }
    }
    return present && !*expect_continue ? 417 : 0;
}

static int read_trailers(rio_t *rp, deadline_ms end) {
    char line[MAX_LINE + 1];
    for (int count = 0; ; count++) {
        int status = read_line(rp, line, 431, end);
        if (status || !strcmp(line, "\r\n")) return status;
        if (count == MAX_HEADERS) return 431;
        http_header h;
        status = parse_field(line, &h);
        if (status) return status;
        if (!strcasecmp(h.name, "Content-Length") || !strcasecmp(h.name, "Transfer-Encoding") ||
            !strcasecmp(h.name, "Host") || !strcasecmp(h.name, "Connection") ||
            !strcasecmp(h.name, "Trailer")) return 400;
        // Accepted trailers are consumed separately and never merged into request headers.
    }
}

static int read_chunked_request(rio_t *rp, http_request *req, deadline_ms end) {
    size_t capacity = 0;
    size_t extensions_left = MAX_CHUNK_EXTENSION_BYTES;
    for (;;) {
        char line[MAX_LINE + 1];
        if (!deadline_left(end)) return 408;
        int status = read_line(rp, line, 400, end);
        if (status) return status;
        size_t line_len = strlen(line) - 2;
        line[line_len] = '\0';
        uintmax_t chunk;
        size_t extension_bytes;
        if (!parse_chunk_size(line, &chunk, &extension_bytes)) return 400;
        char last = line[line_len - 1];
        if (!extension_bytes && (last == ' ' || last == '\t')) return 400;
        if (extension_bytes > extensions_left) return 413;
        extensions_left -= extension_bytes;
        if (!chunk) return read_trailers(rp, end);
        if (chunk > MAX_BODY_LEN - req->content_length) return 413;
        size_t needed = req->content_length + (size_t)chunk;
        if (needed > capacity) {
            capacity = capacity ? capacity * 2 : 4096;
            if (capacity < needed) capacity = needed;
            if (capacity > MAX_BODY_LEN) capacity = MAX_BODY_LEN;
            char *body = realloc(req->body, capacity);
            if (!body) return 500;
            req->body = body;
        }
        status = read_body_bytes(rp, req->body + req->content_length, (size_t)chunk, end);
        if (status) return status;
        req->content_length = needed;
        char crlf[2];
        status = read_body_bytes(rp, crlf, sizeof(crlf), end);
        if (status) return status;
        if (memcmp(crlf, "\r\n", 2)) return 400;
    }
}

int read_request(rio_t *rp, http_request *req) {
    *req = (http_request){0};
    if (!rp->cnt && wait_until(rp->fd, POLLIN, deadline_after(KEEPALIVE_TIMEOUT_SEC * 1000)) < 0)
        return -1;
    int status = read_request_head_until(rp, req, deadline_after(REQUEST_HEADER_TIMEOUT_MS));
    if (status) return status;
    if (!strcmp(req->version, "HTTP/1.1") && !get_header(req, "Host")) return 400;
    int chunked = get_header(req, "Transfer-Encoding") != NULL;
    if (chunked) {
        status = request_coding(req);
        if (status) return status;
    } else if (!strcmp(req->method, "POST") && !get_header(req, "Content-Length")) return 411;

    int expect_continue;
    status = parse_expectations(req, &expect_continue);
    if (status) return status;
    if (!chunked && !req->content_length) return 0;
    if (!chunked) {
        req->body = malloc(req->content_length);
        if (!req->body) return 500;
    }
    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
    if (expect_continue && strcmp(req->version, "HTTP/1.0") &&
        send_until(rp->fd, interim, sizeof(interim) - 1, INT64_MAX) < 0) {
        free_request(req);
        return -1;
    }
    deadline_ms end = deadline_after(REQUEST_BODY_TIMEOUT_MS);
    status = chunked ? read_chunked_request(rp, req, end)
                     : read_body_bytes(rp, req->body, req->content_length, end);
    if (status) free_request(req);
    return status;
}

void free_request(http_request *req) {
    free(req->body);
    req->body = NULL;
}
