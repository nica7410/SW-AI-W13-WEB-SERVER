#include "proxy_identity.h"
#include "proxy_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static char via_comment[sizeof("(w13-") + 32 + 1];
static pthread_once_t via_once = PTHREAD_ONCE_INIT;

static void init_via_comment(void) {
    unsigned char id[16];
    int fd;
    do fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); while (fd < 0 && errno == EINTR);
    if (fd < 0) return;
    size_t used = 0;
    while (used < sizeof(id)) {
        ssize_t n = read(fd, id + used, sizeof(id) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);
    if (used != sizeof(id)) return;
    strcpy(via_comment, "(w13-");
    char *p = via_comment + strlen(via_comment);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(id); i++) {
        *p++ = hex[id[i] >> 4];
        *p++ = hex[id[i] & 15];
    }
    strcpy(p, ")");
}

const char *proxy_via_comment(void) {
    pthread_once(&via_once, init_via_comment);
    return via_comment;
}

int proxy_endpoint(int fd, int peer, char value[ENDPOINT_SIZE]) {
    struct sockaddr_storage addr;
    socklen_t size = sizeof(addr);
    int rc = peer ? getpeername(fd, (struct sockaddr *)&addr, &size)
                  : getsockname(fd, (struct sockaddr *)&addr, &size);
    if (rc || (addr.ss_family != AF_INET && addr.ss_family != AF_INET6)) return 0;
    char host[NI_MAXHOST], port[NI_MAXSERV];
    if (getnameinfo((struct sockaddr *)&addr, size, host, sizeof(host), port, sizeof(port),
                    NI_NUMERICHOST | NI_NUMERICSERV)) return 0;
    int n = snprintf(value, ENDPOINT_SIZE, addr.ss_family == AF_INET ? "%s:%s" : "[%s]:%s",
                     host, port);
    return n > 0 && n < ENDPOINT_SIZE;
}

static int same_name(const char *a, const char *b) {
    size_t n = strlen(a), m = strlen(b);
    if (n && a[n - 1] == '.') n--;
    if (m && b[m - 1] == '.') m--;
    return n == m && !strncasecmp(a, b, n);
}

int is_listener_authority(int client, const char *host, const char *port, const char *alias) {
    struct sockaddr_in local;
    socklen_t size = sizeof(local);
    if (getsockname(client, (struct sockaddr *)&local, &size) || local.sin_family != AF_INET ||
        ntohs(local.sin_port) != strtoul(port, NULL, 10)) return 0;
    if (same_name(host, "localhost") || (alias && same_name(host, alias))) return 1;
    struct in_addr address;
    if (inet_pton(AF_INET, host, &address) != 1) {
        char hostname[256];
        return gethostname(hostname, sizeof(hostname)) == 0 &&
               memchr(hostname, 0, sizeof(hostname)) && same_name(host, hostname);
    }
    if (address.s_addr == local.sin_addr.s_addr || (ntohl(address.s_addr) >> 24) == 127) return 1;
    struct ifaddrs *interfaces;
    if (getifaddrs(&interfaces)) return 0;
    int found = 0;
    for (struct ifaddrs *i = interfaces; i; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
            ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == address.s_addr) {
            found = 1;
            break;
        }
    }
    freeifaddrs(interfaces);
    return found;
}

// A Via entry contains protocol, received-by, then an optional nested comment.
// Commas inside comments do not start another hop.
static const char *via_entry_end(const char *p) {
    unsigned depth = 0;
    while (*p && (*p != ',' || depth)) {
        if (depth && *p == '\\' && p[1]) p++;
        else if (*p == '(') depth++;
        else if (depth && *p == ')') depth--;
        p++;
    }
    return p;
}

static int via_contains(const http_request *req, const char *endpoint, const char *comment) {
    size_t len = endpoint ? strlen(endpoint) : 0;
    size_t comment_len = comment ? strlen(comment) : 0;
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, "Via")) continue;
        const char *p = req->headers[i].value;
        while (*p) {
            p += strspn(p, " \t,");
            if (!*p) break;
            const char *end = via_entry_end(p);
            const char *next = end;
            while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
            p += strcspn(p, " \t,(");
            p += strspn(p, " \t");
            size_t n = strcspn(p, " \t,(");
            if (len && n == len && !strncasecmp(p, endpoint, n)) return 1;
            p += n;
            if (comment_len && n && (*p == ' ' || *p == '\t')) {
                p += strspn(p, " \t");
                if (p < end && (size_t)(end - p) == comment_len && !memcmp(p, comment, comment_len))
                    return 1;
            }
            p = next;
        }
    }
    return 0;
}

int via_revisits_listener(int client, const http_request *req) {
    char identity[ENDPOINT_SIZE];
    if (!get_header(req, "Via")) return 0;
    return via_contains(req, proxy_endpoint(client, 0, identity) ? identity : NULL,
                        proxy_via_comment());
}

int proxy_returns_to_hop(int client, int upstream, const http_request *req) {
    char destination[ENDPOINT_SIZE], host[ENDPOINT_SIZE], port[8];
    if (!proxy_endpoint(upstream, 1, destination)) return 0;
    if (via_contains(req, destination, NULL)) return 1;
    if (parse_authority(destination, strlen(destination), NULL,
                        host, sizeof(host), port, sizeof(port)) < 0) return 0;
    return is_listener_authority(client, host, port, NULL);
}
