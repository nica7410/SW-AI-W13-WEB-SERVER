#include "router.h"
#include "proxy_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int parse_route_line(const char *line, route *r) {
    static const char spaces[] = " \t\r\n";
    size_t n = strcspn(line, "#");
    char *copy = strndup(line, n);
    if (!copy) return -1;
    char *state, *prefix = strtok_r(copy, spaces, &state);
    int result = 0;
    if (!prefix) goto done;
    result = -1;
    if (*prefix != '/' || strlen(prefix) >= sizeof(r->prefix)) goto done;
    *r = (route){0};
    strcpy(r->prefix, prefix);
    for (char *s; (s = strtok_r(NULL, spaces, &state)) != NULL;) {
        if (r->backend_count == MAX_BACKENDS) goto done;
        backend *b = &r->backends[r->backend_count];
        if (parse_authority(s, strlen(s), NULL, b->host, sizeof(b->host),
                            b->port, sizeof(b->port)) < 0) goto done;
        r->backend_count++;
    }
    if (r->backend_count) result = 1;
done:
    free(copy);
    return result;
}

int load_routes(const char *path, route_table *table) {
    FILE *file = fopen(path, "r");
    if (!file) { perror(path); return -1; }
    table->count = 0;
    char *line = NULL;
    size_t size = 0, number = 0;
    int result = 0;
    while (getline(&line, &size, file) >= 0) {
        number++;
        route entry;
        int parsed = parse_route_line(line, &entry);
        if (!parsed) continue;
        if (parsed < 0 || table->count == MAX_ROUTES) {
            fprintf(stderr, "%s:%zu: invalid route or too many routes\n", path, number);
            result = -1;
            break;
        }
        table->routes[table->count++] = entry;
    }
    if (ferror(file)) result = -1;
    free(line);
    fclose(file);
    return result;
}

route *match_route(route_table *table, const char *path) {
    route *best = NULL;
    size_t longest = 0;
    for (int i = 0; i < table->count; i++) {
        route *r = &table->routes[i];
        size_t n = strlen(r->prefix);
        if (n > longest && strncmp(path, r->prefix, n) == 0) {
            best = r;
            longest = n;
        }
    }
    return best;
}

const backend *next_backend(route *r) {
    if (!r->backend_count) return NULL;
    unsigned next = __atomic_fetch_add(&r->next, 1, __ATOMIC_RELAXED);
    return &r->backends[next % (unsigned)r->backend_count];
}
