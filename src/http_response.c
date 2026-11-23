#include "http_response.h"
#include "access_log.h"
#include "deadline.h"
#include "rio.h"
#include "server_internal.h"
#include <stdio.h>
#include <string.h>

int format_http_date(time_t when, char value[HTTP_DATE_SIZE]) {
    static const char *const days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *const months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                         "Jul","Aug","Sep","Oct","Nov","Dec"};
    struct tm utc;
    if (!gmtime_r(&when, &utc) || utc.tm_year < -1900 || utc.tm_year > 8099) return 0;
    return snprintf(value, HTTP_DATE_SIZE, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                    days[utc.tm_wday], utc.tm_mday, months[utc.tm_mon], utc.tm_year + 1900,
                    utc.tm_hour, utc.tm_min, utc.tm_sec) == HTTP_DATE_SIZE - 1;
}

const char *status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}

static int write_headers(int fd, int status, const char *content_type, size_t content_length,
                         int keep_alive, const char *extra) {
    char headers[MAX_URI_LEN + 512];
    char date[HTTP_DATE_SIZE];
    time_t now = time(NULL);
    int dated = now != (time_t)-1 && format_http_date(now, date);
    int n = snprintf(headers, sizeof(headers),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: %s\r\n%s%s%s%s\r\n", status, status_text(status), content_type,
                     content_length, keep_alive ? "keep-alive" : "close",
                     dated ? "Date: " : "", dated ? date : "", dated ? "\r\n" : "",
                     extra);
    if (n < 0 || (size_t)n >= sizeof(headers)) return -1;
    if (send_until(fd, headers, (size_t)n, INT64_MAX) < 0) return -1;
    access_headers_sent(status);
    return 0;
}

int send_headers(int fd, int status, const char *type, size_t length, int keep_alive) {
    return write_headers(fd, status, type, length, keep_alive,
                         status == 405 ? "Allow: GET, HEAD\r\n" : "");
}

int send_directory_redirect(int fd, const http_request *req) {
    // Use the original URL spelling, including an empty query, with one leading slash.
    const char *path = req->path + strspn(req->path, "/");
    const char *query = strchr(req->uri, '?');
    char field[MAX_URI_LEN + sizeof("Location: //\r\n")];
    int n = snprintf(field, sizeof(field), "Location: /%s/%s\r\n", path, query ? query : "");
    if (n < 0 || (size_t)n >= sizeof(field)) return 414;
    return write_headers(fd, 301, "text/html", 0, req->keep_alive, field);
}

int send_response(int fd, int status, const char *type, const void *body, size_t len,
                  int keep_alive) {
    if (send_headers(fd, status, type, len, keep_alive) < 0) return -1;
    return send_until(fd, body, len, INT64_MAX);
}

int send_request_error(int fd, int status, int keep_alive, int head_only) {
    char body[128];
    int n = snprintf(body, sizeof(body), "<h1>%d %s</h1>\n", status, status_text(status));
    if (head_only) return send_headers(fd, status, "text/html", (size_t)n, keep_alive);
    return send_response(fd, status, "text/html", body, (size_t)n, keep_alive);
}

int send_error(int fd, int status, int keep_alive) {
    return send_request_error(fd, status, keep_alive, 0);
}
