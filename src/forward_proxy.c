#include "forward_proxy.h"
#include "proxy_identity.h"
#include "upstream.h"
#include "proxy_internal.h"
#include "net.h"
#include "proxy.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

int is_absolute_uri(const char *uri) {
    return strncasecmp(uri, "http://", 7) == 0;
}

int parse_authority(const char *text, size_t len, const char *default_port,
                    char *host, size_t hostlen, char *port, size_t portlen) {
    const char *end = text + len, *name = text, *after, *name_end;
    int ipv6 = len && *text == '[';
    if (ipv6) {
        name++;
        name_end = memchr(name, ']', (size_t)(end - name));
        if (!name_end) return -1;
        after = name_end + 1;
    } else {
        name_end = memchr(text, ':', len);
        if (!name_end) name_end = end;
        after = name_end;
        for (const char *p = name; p < name_end; p++) {
            char c = *p;
            if (!(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'Z') &&
                !(c >= 'a' && c <= 'z') && c != '.' && c != '-' && c != '_') return -1;
        }
    }
    size_t n = (size_t)(name_end - name);
    if (!n || n >= hostlen) return -1;
    memcpy(host, name, n);
    host[n] = '\0';
    struct in6_addr addr;
    if (ipv6 && inet_pton(AF_INET6, host, &addr) != 1) return -1;
    if (after == end) {
        if (!default_port || strlen(default_port) >= portlen) return -1;
        strcpy(port, default_port);
        return 0;
    }
    if (*after++ != ':' || after == end) return -1;
    unsigned number = 0;
    for (; after < end; after++) {
        if (*after < '0' || *after > '9') return -1;
        number = number * 10 + (unsigned)(*after - '0');
        if (number > 65535) return -1;
    }
    if (!number) return -1;
    int count = snprintf(port, portlen, "%u", number);
    return count >= 0 && (size_t)count < portlen ? 0 : -1;
}

int parse_absolute_uri(const char *uri, char *host, size_t hostlen,
                       char *port, size_t portlen, char *path, size_t pathlen) {
    if (!is_absolute_uri(uri) || strpbrk(uri, "# \r\n\t")) return -1;
    const char *authority = uri + 7;
    size_t len = strcspn(authority, "/?");
    if (parse_authority(authority, len, "80", host, hostlen, port, portlen) < 0) return -1;
    const char *target = authority + len;
    int n = snprintf(path, pathlen, "%s%s", *target == '/' ? "" : "/", target);
    return n >= 0 && (size_t)n < pathlen ? 0 : -1;
}

int forward_proxy_request(int clientfd, const http_request *req) {
    deadline_ms end = deadline_after(PROXY_TIMEOUT_SEC * 1000);
    char host[256], port[8], target[MAX_URI_LEN];
    if (parse_absolute_uri(req->uri, host, sizeof(host), port, sizeof(port),
                           target, sizeof(target)) < 0) return 400;
    if (via_revisits_listener(clientfd, req)) return 508;
    int upstream = connect_until(host, port, end, getaddrinfo);
    if (upstream < 0) return errno == ETIMEDOUT ? 504 : 502;
    int status = relay_until(clientfd, upstream, req, target, end, NULL);
    close(upstream);
    return status;
}
