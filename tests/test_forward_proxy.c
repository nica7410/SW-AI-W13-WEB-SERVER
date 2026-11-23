#include "test.h"
#include "forward_proxy.h"

int main(void) {
    char host[64], port[8], path[128];
    CHECK(is_absolute_uri("HTTP://example.com/"));
    CHECK(!is_absolute_uri("/index.html"));
    CHECK(!is_absolute_uri("https://example.com"));
    CHECK(parse_absolute_uri("http://example.com/a.html", host, sizeof(host),
                             port, sizeof(port), path, sizeof(path)) == 0);
    CHECK_STR(host, "example.com");
    CHECK_STR(port, "80");
    CHECK_STR(path, "/a.html");
    CHECK(parse_absolute_uri("http://localhost:8001/api?x=1", host, sizeof(host),
                             port, sizeof(port), path, sizeof(path)) == 0);
    CHECK_STR(port, "8001");
    CHECK_STR(path, "/api?x=1");
    CHECK(parse_absolute_uri("http://h", host, sizeof(host), port, sizeof(port),
                             path, sizeof(path)) == 0);
    CHECK_STR(path, "/");
    CHECK(parse_absolute_uri("http://[::1]?q", host, sizeof(host), port, sizeof(port),
                             path, sizeof(path)) == 0);
    CHECK_STR(host, "::1");
    CHECK_STR(path, "/?q");
    const char *invalid[] = {"http:///a", "http://h:0", "http://h:99999/", "http://h:x",
                             "http://user@h/", "https://h/"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(parse_absolute_uri(invalid[i], host, sizeof(host), port, sizeof(port),
                                 path, sizeof(path)) == -1);
    CHECK(parse_absolute_uri("http://toolong", host, 3, port, sizeof(port),
                             path, sizeof(path)) == -1);
    return TEST_RESULT();
}
