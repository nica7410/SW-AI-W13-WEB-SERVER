#include "test.h"
#include "router.h"

int main(void) {
    route_table t = {0};
    route *base = &t.routes[0], *nested = &t.routes[1];
    CHECK(parse_route_line("/api/ 127.0.0.1:8001 localhost:8002 # api", base) == 1);
    CHECK_STR(base->prefix, "/api/");
    CHECK(base->backend_count == 2);
    CHECK_STR(base->backends[0].host, "127.0.0.1");
    CHECK_STR(base->backends[1].port, "8002");
    CHECK(parse_route_line("/api/v2/ localhost:9000", nested) == 1);
    t.count = 2;
    CHECK(match_route(&t, "/api/v2/x") == nested);
    CHECK(match_route(&t, "/api/foo") == base);
    CHECK(match_route(&t, "/other") == NULL);
    CHECK(next_backend(base) == &base->backends[0]);
    CHECK(next_backend(base) == &base->backends[1]);
    CHECK(next_backend(base) == &base->backends[0]);

    route r;
    CHECK(parse_route_line(" \t\n", &r) == 0);
    CHECK(parse_route_line(" # comment", &r) == 0);
    CHECK(parse_route_line("/api/", &r) == -1);
    CHECK(parse_route_line("/api/ localhost", &r) == -1);
    CHECK(parse_route_line("/api/ host:65536", &r) == -1);
    return TEST_RESULT();
}
