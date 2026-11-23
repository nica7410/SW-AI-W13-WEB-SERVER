#include "test.h"
#include "router.h"
#include <stdlib.h>
#include <unistd.h>

static void test_duplicate_prefix(void) {
    char path[] = "/tmp/w13-routes-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(1); }
    CHECK(dprintf(fd, "/api/ first:8001\n/api/ second:8002\n") > 0);
    CHECK(close(fd) == 0);

    FILE *warnings = tmpfile();
    int saved = dup(STDERR_FILENO);
    if (!warnings || saved < 0 || dup2(fileno(warnings), STDERR_FILENO) < 0) {
        perror("capture stderr"); unlink(path); exit(1);
    }
    route_table table;
    int status = load_routes(path, &table);
    fflush(stderr);
    if (dup2(saved, STDERR_FILENO) < 0) { perror("restore stderr"); unlink(path); exit(1); }
    close(saved);
    unlink(path);

    CHECK(status == 0 && table.count == 2);
    CHECK(match_route(&table, "/api/x") == &table.routes[0]);
    CHECK_STR(table.routes[0].backends[0].host, "first");
    rewind(warnings);
    char line[256], expected[256];
    snprintf(expected, sizeof(expected), "%s:2: duplicate route prefix /api/ (first rule takes precedence)\n", path);
    CHECK(fgets(line, sizeof(line), warnings) && !strcmp(line, expected));
    CHECK(fgetc(warnings) == EOF);
    fclose(warnings);
}

int main(void) {
    test_duplicate_prefix();
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
