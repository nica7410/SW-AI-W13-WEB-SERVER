// HTTP 필드 이름과 쉼표로 구분된 Connection 옵션을 읽는 내부 도우미.
#ifndef HTTP_FIELDS_H
#define HTTP_FIELDS_H
#include <string.h>
#include <strings.h>

static inline int is_field_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_|~" "\x60", c));
}

static inline int has_option(const char *list, const char *option) {
    size_t len = strlen(option);
    while (*list) {
        list += strspn(list, " \t,");
        const char *end = list + strcspn(list, ",");
        const char *trim = end;
        while (trim > list && (trim[-1] == ' ' || trim[-1] == '\t')) trim--;
        if ((size_t)(trim - list) == len && strncasecmp(list, option, len) == 0) return 1;
        list = end;
    }
    return 0;
}
#endif
