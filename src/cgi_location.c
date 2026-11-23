#include "cgi.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

// RFC 3986 components share unreserved characters, sub-delimiters and %HH.
static int uri_component(const char *p, const char *end, const char *extra) {
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '%') {
            if (end - p < 2 || !isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]))
                return 0;
            p += 2;
        } else if (c >= 128 || !(isalnum(c) || strchr("-._~!$&'()*+,;=", c) || strchr(extra, c)))
            return 0;
    }
    return 1;
}

static int ip_literal(const char *p, const char *end) {
    if (p == end) return 0;
    if (*p == 'v' || *p == 'V') {
        const char *version = ++p;
        while (p < end && isxdigit((unsigned char)*p)) p++;
        if (p == version || p == end || *p++ != '.' || p == end) return 0;
        return !memchr(p, '%', (size_t)(end - p)) && uri_component(p, end, ":");
    }
    char text[INET6_ADDRSTRLEN];
    size_t len = (size_t)(end - p);
    if (len >= sizeof(text)) return 0;
    memcpy(text, p, len);
    text[len] = '\0';
    struct in6_addr addr;
    return inet_pton(AF_INET6, text, &addr) == 1;
}

static int uri_authority(const char *p, const char *end, int require_host) {
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at) {
        if (!uri_component(p, at, ":")) return 0;
        p = at + 1;
    }
    if (p < end && *p == '[') {
        const char *close = memchr(p, ']', (size_t)(end - p));
        if (!close || !ip_literal(p + 1, close)) return 0;
        p = close + 1;
    } else {
        const char *port = memchr(p, ':', (size_t)(end - p));
        const char *host_end = port ? port : end;
        if ((require_host && p == host_end) || !uri_component(p, host_end, "")) return 0;
        p = host_end;
    }
    if (p == end) return 1;
    if (*p++ != ':') return 0;
    while (p < end && *p >= '0' && *p <= '9') p++;
    return p == end;
}

int is_cgi_redirect_uri(const char *uri) {
    const char *p = uri;
    if ((unsigned char)*p >= 128 || !isalpha((unsigned char)*p)) return 0;
    for (p++; *p && *p != ':'; p++)
        if ((unsigned char)*p >= 128 || !(isalnum((unsigned char)*p) || strchr("+.-", *p))) return 0;
    if (*p != ':') return 0;
    int http = !strncasecmp(uri, "http:", 5) || !strncasecmp(uri, "https:", 6);
    p++;
    if (!strncmp(p, "//", 2)) {
        p += 2;
        const char *end = p + strcspn(p, "/?#");
        if (!uri_authority(p, end, http)) return 0;
        p = end;
    } else if (http) return 0;

    const char *end = p + strcspn(p, "?#");
    if (!uri_component(p, end, ":@/")) return 0;
    p = end;
    if (*p == '?') {
        p++;
        end = p + strcspn(p, "#");
        if (!uri_component(p, end, ":@/?")) return 0;
        p = end;
    }
    return !*p || uri_component(p + 1, p + strlen(p), ":@/?");
}
