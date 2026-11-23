#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_integer(const char *s, long *number) {
    const char *digits = s;
    if (*digits == '-' || *digits == '+') digits++;
    if (!*digits || digits[strspn(digits, "0123456789")]) return -1;
    errno = 0;
    *number = strtol(s, NULL, 10);
    return errno ? -1 : 0;
}

static int sum_form(char *form, long *sum) {
    long a = 0, b = 0;
    int seen_a = 0, seen_b = 0;
    char *state;
    for (char *s = strtok_r(form, "&", &state); s; s = strtok_r(NULL, "&", &state)) {
        if (!strncmp(s, "a=", 2) && !seen_a) {
            if (parse_integer(s + 2, &a)) return -1;
            seen_a = 1;
        } else if (!strncmp(s, "b=", 2) && !seen_b) {
            if (parse_integer(s + 2, &b)) return -1;
            seen_b = 1;
        } else return -1;
    }
    if (!seen_a || !seen_b || (b > 0 && a > LONG_MAX - b) ||
        (b < 0 && a < LONG_MIN - b)) return -1;
    *sum = a + b;
    return 0;
}

int main(void) {
    const char *method = getenv("REQUEST_METHOD"), *query = getenv("QUERY_STRING");
    char form[256], body[96];
    int valid = 0;
    if (method && !strcmp(method, "POST")) {
        const char *length = getenv("CONTENT_LENGTH");
        long n;
        if (length && !parse_integer(length, &n) && n >= 0 && (size_t)n < sizeof(form) &&
            fread(form, 1, (size_t)n, stdin) == (size_t)n && !memchr(form, '\0', (size_t)n)) {
            form[n] = '\0';
            valid = 1;
        }
    } else if (query && strlen(query) < sizeof(form)) {
        strcpy(form, query);
        valid = 1;
    }
    long sum;
    valid = valid && sum_form(form, &sum) == 0;
    if (valid) snprintf(body, sizeof(body), "%ld\n", sum);
    else strcpy(body, "Expected two integers: a and b\n");
    if (!valid) printf("Status: 400 Bad Request\r\n");
    printf("Content-Type: text/plain\r\nContent-Length: %zu\r\n\r\n", strlen(body));
    if (!method || strcmp(method, "HEAD")) fputs(body, stdout);
    return 0;
}
