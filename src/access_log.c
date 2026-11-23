#include "access_log.h"
#include "deadline.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// 각 워커는 요청 하나를 동기적으로 처리한다. 수락 스레드의 503 기록도 독립적이다.
struct access_record {
    int active, status, attempts;
    deadline_ms started;
    const backend *first, *connected;
};
static _Thread_local struct access_record record;

void access_begin(void) {
    record = (struct access_record){.active = 1, .started = deadline_after(0)};
}

void access_attempt(const backend *candidate) {
    if (!record.active) return;
    if (!record.attempts++) record.first = candidate;
}

void access_connected(const backend *selected) {
    if (record.active) record.connected = selected;
}

void access_headers_sent(int status) {
    if (record.active) record.status = status;
}

static void format_endpoint(const backend *b, char *dst, size_t size) {
    if (!b) snprintf(dst, size, "-");
    else snprintf(dst, size, strchr(b->host, ':') ? "[%s]:%s" : "%s:%s", b->host, b->port);
}

static void format_path(const char *path, char *dst, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (; *path && *path != '?'; path++) {
        unsigned char c = (unsigned char)*path;
        int escape = c <= ' ' || c >= 127 || c == '\\';
        if (size - used < (size_t)(escape ? 4 : 1) + sizeof("...")) {
            memcpy(dst + used, "...", sizeof("..."));
            return;
        }
        if (escape) {
            dst[used++] = '\\';
            dst[used++] = 'x';
            dst[used++] = hex[c >> 4];
            dst[used++] = hex[c & 15];
        } else dst[used++] = (char)c;
    }
    dst[used] = '\0';
}

void access_finish(const http_request *req, int result) {
    if (!record.active) return;
    record.active = 0;
    int64_t duration = deadline_after(0) - record.started;
    char first[272], connected[272], path[512], status[16] = "-";
    format_endpoint(record.first, first, sizeof(first));
    format_endpoint(record.connected, connected, sizeof(connected));
    format_path(req && *req->path ? req->path : "-", path, sizeof(path));
    if (record.status) snprintf(status, sizeof(status), "%d", record.status);
    fprintf(stderr, "access event=%s method=%s path=%s status=%s outcome=%s "
                    "first=%s connected=%s attempts=%d duration_ms=%" PRId64 "\n",
            req ? "request" : "overload", req && *req->method ? req->method : "-", path,
            status, result == 0 ? "complete" : "aborted", first, connected, record.attempts, duration);
}
