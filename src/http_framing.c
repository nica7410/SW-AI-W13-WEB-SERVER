#include "http_framing.h"
#include "http_fields.h"

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

int parse_transfer_codings(const char *p, int *chunked) {
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

int parse_chunk_size(const char *p, uintmax_t *size, size_t *extension_bytes) {
    for (const unsigned char *c = (const unsigned char *)p; *c; c++)
        if ((*c < 32 && *c != '\t') || *c == 127) return 0;
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
    const char *suffix = p;
    if (p == start || !read_parameters(&p, 0) || *p) return 0;
    if (extension_bytes) *extension_bytes = strchr(suffix, ';') ? (size_t)(p - suffix) : 0;
    *size = n;
    return 1;
}
