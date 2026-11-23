#include "response_stream.h"
#include "access_log.h"
#include "forward_proxy.h"
#include "proxy.h"
#include "upstream.h"
#include "http_fields.h"
#include "http_response.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// 원 서버의 여러 Set-Cookie 등 반복 가능한 헤더의 순서도 유지한다.
struct fields {
    char data[65536];
    struct { char *name, *value; } items[128];
    size_t used, count;
};

static int stream_error(int cgi) {
    if (cgi) return 500;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ? 504 : 502;
}

// '\r\n'을 제거한 한 줄을 반환한다. CGI 프로그램은 '\n'도 쓸 수 있다.
static int next_line(rio_t *rio, char *line, int cgi, deadline_ms end) {
    if (!deadline_left(end)) return stream_error(cgi);
    ssize_t n = rio_readline_until(rio, line, MAX_LINE + 1, end);
    if (n < 0) return stream_error(cgi);
    if (!n || line[n - 1] != '\n' || memchr(line, '\0', (size_t)n)) goto invalid;
    n--;
    if (n && line[n - 1] == '\r') n--;
    else if (!cgi) goto invalid;
    line[n] = '\0';
    for (ssize_t i = 0; i < n; i++)
        if (((unsigned char)line[i] < 32 && line[i] != '\t') || line[i] == 127)
            goto invalid;
    return 0;
invalid:
    return cgi ? 500 : 502;
}

static int read_fields(rio_t *rio, struct fields *f, int cgi, deadline_ms deadline) {
    char line[MAX_LINE + 1];
    f->used = f->count = 0;
    for (;;) {
        int status = next_line(rio, line, cgi, deadline);
        if (status || !*line) return status;
        size_t size = strlen(line) + 1;
        char *colon = strchr(line, ':');
        if (!colon || colon == line || size > sizeof(f->data) - f->used ||
            f->count == sizeof(f->items) / sizeof(f->items[0])) return cgi ? 500 : 502;
        for (char *p = line; p < colon; p++)
            if (!is_field_char((unsigned char)*p)) return cgi ? 500 : 502;
        char *name = memcpy(f->data + f->used, line, size);
        char *value = name + (colon - line);
        *value++ = '\0';
        value += strspn(value, " \t");
        char *end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        f->items[f->count].name = name;
        f->items[f->count++].value = value;
        f->used += size;
    }
}

static int connection_field(const struct fields *f, const char *name) {
    if (!strcasecmp(name, "Connection") || !strcasecmp(name, "Keep-Alive") ||
        !strcasecmp(name, "Proxy-Connection") || !strcasecmp(name, "Upgrade")) return 1;
    for (size_t i = 0; i < f->count; i++)
        if (!strcasecmp(f->items[i].name, "Connection") &&
            has_option(f->items[i].value, name)) return 1;
    return 0;
}

static int body_to_client(rio_t *rio, int client, int known_length, uintmax_t left,
                          deadline_ms end) {
    char buf[RIO_BUFSIZE];
    while (!known_length || left) {
        size_t want = sizeof(buf);
        if (known_length && left < want) want = (size_t)left;
        ssize_t n = rio_readsome_until(rio, buf, want, end);
        if (!n) return known_length ? -1 : 0;
        if (n < 0 || send_until(client, buf, (size_t)n, end) < 0) return -1;
        if (known_length) left -= (size_t)n;
    }
    return 0;
}

static void skip_space(const char **p) {
    *p += strspn(*p, " \t");
}

static int read_token(const char **p) {
    const char *start = *p;
    while (is_field_char((unsigned char)**p)) (*p)++;
    return *p != start;
}

static int read_parameter(const char **p) {
    if (**p != '"') return read_token(p);
    (*p)++;
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            if (!**p) return 0;
        }
        (*p)++;
    }
    if (**p != '"') return 0;
    (*p)++;
    return 1;
}

// Both transfer-coding parameters and chunk extensions allow quoted strings.
static int read_parameters(const char **p, int value_required) {
    skip_space(p);
    while (**p == ';') {
        (*p)++;
        skip_space(p);
        if (!read_token(p)) return 0;
        skip_space(p);
        if (**p == '=') {
            (*p)++;
            skip_space(p);
            if (!read_parameter(p)) return 0;
            skip_space(p);
        } else if (value_required) return 0;
    }
    return 1;
}

static int transfer_codings(const char *p, int *chunked) {
    for (;;) {
        skip_space(&p);
        const char *name = p;
        if (*chunked || !read_token(&p)) return 0;
        *chunked = p - name == 7 && !strncasecmp(name, "chunked", 7);
        skip_space(&p);
        if (*chunked && *p == ';') return 0;
        if (!read_parameters(&p, 1)) return 0;
        if (!*p) return 1;
        if (*p++ != ',') return 0;
    }
}

static int parse_chunk(const char *p, uintmax_t *size) {
    const char *start = p;
    uintmax_t n = 0;
    for (;; p++) {
        unsigned digit;
        if (*p >= '0' && *p <= '9') digit = (unsigned)(*p - '0');
        else if (*p >= 'a' && *p <= 'f') digit = (unsigned)(*p - 'a') + 10;
        else if (*p >= 'A' && *p <= 'F') digit = (unsigned)(*p - 'A') + 10;
        else break;
        if (n > (UINTMAX_MAX - digit) / 16) return 0;
        n = n * 16 + digit;
    }
    if (p == start || !read_parameters(&p, 0) || *p) return 0;
    *size = n;
    return 1;
}

static int write_line(int fd, deadline_ms end, const char *first, const char *second,
                      const char *third) {
    // A CGI status value can gain a protocol prefix, a space and CRLF on output.
    char wire[MAX_LINE + sizeof("HTTP/1.1  \r\n")];
    int n = snprintf(wire, sizeof(wire), "%s%s%s\r\n", first, second, third);
    if (n < 0 || (size_t)n >= sizeof(wire)) return -1;
    return send_until(fd, wire, (size_t)n, end);
}

static int chunked_body(rio_t *rio, int clientfd, struct fields *fields, deadline_ms end) {
    char line[MAX_LINE + 1];
    for (;;) {
        uintmax_t size;
        if (next_line(rio, line, 0, end) || !parse_chunk(line, &size)) return -1;
        if (!size) break;
        if (write_line(clientfd, end, line, "", "") < 0 ||
            body_to_client(rio, clientfd, 1, size, end) < 0) return -1;
        char crlf[2];
        if (rio_readn_until(rio, crlf, 2, end) != 2 || memcmp(crlf, "\r\n", 2) ||
            send_until(clientfd, crlf, 2, end) < 0) return -1;
    }
    // Signal completion downstream only once the entire trailer section is valid.
    if (read_fields(rio, fields, 0, end)) return -1;
    for (size_t i = 0; i < fields->count; i++) {
        const char *name = fields->items[i].name;
        if (!strcasecmp(name, "Content-Length") || !strcasecmp(name, "Transfer-Encoding") ||
            !strcasecmp(name, "Host") || !strcasecmp(name, "Trailer") ||
            connection_field(fields, name)) return -1;
    }
    if (write_line(clientfd, end, line, "", "") < 0) return -1;
    for (size_t i = 0; i < fields->count; i++)
        if (write_line(clientfd, end, fields->items[i].name, ": ", fields->items[i].value) < 0)
            return -1;
    return send_until(clientfd, "\r\n", 2, end);
}

static int status_code(const char *text) {
    if (text[0] < '1' || text[0] > '5' || text[1] < '0' || text[1] > '9' ||
        text[2] < '0' || text[2] > '9' || (text[3] && text[3] != ' ')) return 0;
    return (text[0] - '0') * 100 + (text[1] - '0') * 10 + text[2] - '0';
}

int response_until(int clientfd, int source, const http_request *req, int from_cgi,
                   deadline_ms header_end, deadline_ms end) {
    if (end < header_end) header_end = end;
    rio_t rio;
    rio_readinitb(&rio, source);
    struct fields fields;
    char line[MAX_LINE + 1];
    const char *reason = "200 OK";
    int code = 200, error = from_cgi ? 500 : 502;
    // 요청의 Expect는 앞단에서 처리했다. 원 서버의 중간 응답은 건너뛴다.
    for (;;) {
        if (!from_cgi) {
            int rc = next_line(&rio, line, 0, header_end);
            if (rc) return rc;
            if (strncmp(line, "HTTP/1.0 ", 9) && strncmp(line, "HTTP/1.1 ", 9)) return 502;
            reason = line + 9;
            code = status_code(reason);
            if (!code || code == 101) return 502;
        }
        int rc = read_fields(&rio, &fields, from_cgi, header_end);
        if (rc) return rc;
        if (code >= 200) break;
    }

    time_t received = time(NULL);
    int has_length = 0, transfer_encoded = 0, chunked = 0, has_type = 0;
    uintmax_t length = 0;
    for (size_t i = 0; i < fields.count; i++) {
        const char *name = fields.items[i].name, *v = fields.items[i].value;
        if (!strcasecmp(name, "Content-Length")) {
            if (has_length || !*v || v[strspn(v, "0123456789")]) return error;
            errno = 0;
            length = strtoull(v, NULL, 10);
            if (errno) return error;
            has_length = 1;
        } else if (!strcasecmp(name, "Transfer-Encoding")) {
            if (from_cgi || !strcmp(req->version, "HTTP/1.0") ||
                connection_field(&fields, name)) return error;
            if (!transfer_codings(v, &chunked)) return error;
            transfer_encoded = 1;
        } else if (from_cgi && !strcasecmp(name, "Status")) {
            code = status_code(v);
            if (code < 200) return error;
            reason = v;
        } else if (!strcasecmp(name, "Content-Type")) has_type = 1;
    }
    if ((has_length && transfer_encoded) || (from_cgi && !has_type)) return error;

    if (!deadline_left(header_end)) return stream_error(from_cgi);
    if (write_line(clientfd, end, "HTTP/1.1 ", reason, reason[3] ? "" : " ") < 0 ||
        write_line(clientfd, end, "Connection", ": ", "close") < 0) return -1;
    int sent_date = 0;
    for (size_t i = 0; i < fields.count; i++) {
        const char *name = fields.items[i].name;
        if ((from_cgi && !strcasecmp(name, "Status")) || connection_field(&fields, name))
            continue;
        if (!strcasecmp(name, "Date")) {
            if (sent_date) continue;
            sent_date = 1;
        }
        if (write_line(clientfd, end, name, ": ", fields.items[i].value) < 0) return -1;
    }
    char date[HTTP_DATE_SIZE];
    if (!sent_date && received != (time_t)-1 && format_http_date(received, date) &&
        write_line(clientfd, end, "Date: ", date, "") < 0) return -1;
    if (!from_cgi && is_absolute_uri(req->uri) &&
        write_line(clientfd, end, "Via: ", line[7] == '0' ? "1.0" : "1.1", " w13-proxy") < 0) return -1;
    if (send_until(clientfd, "\r\n", 2, end) < 0) return -1;
    access_headers_sent(code);
    if (!strcmp(req->method, "HEAD") || code == 204 || code == 304) return 0;
    if (chunked) return chunked_body(&rio, clientfd, &fields, end);
    return body_to_client(&rio, clientfd, has_length, length, end);
}

int relay_response(int clientfd, int source, const http_request *req, int from_cgi) {
    deadline_ms end = deadline_after(RESPONSE_TIMEOUT_MS);
    deadline_ms head = from_cgi ? end : deadline_after(PROXY_TIMEOUT_SEC * 1000);
    return response_until(clientfd, source, req, from_cgi, head, end);
}
