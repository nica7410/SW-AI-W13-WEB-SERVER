#include "router.h"
#include "proxy_internal.h"
#include <stdio.h>
#include <pthread.h>
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
        for (int i = 0; i < table->count; i++) {
            if (!strcmp(table->routes[i].prefix, entry.prefix)) {
                fprintf(stderr, "%s:%zu: duplicate route prefix %s (first rule takes precedence)\n",
                        path, number, entry.prefix);
                break;
            }
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

// Routes are copied while loading configuration. The lock lives outside those copies.
static pthread_mutex_t health_lock = PTHREAD_MUTEX_INITIALIZER;

int select_backend(route *r, unsigned tried, int64_t now, backend_attempt *attempt) {
    pthread_mutex_lock(&health_lock);
    unsigned start = tried ? (unsigned)attempt->index + 1 : r->next;
    attempt->eligible = 0;
    for (int offset = 0; offset < r->backend_count; offset++) {
        int i = (int)((start + (unsigned)offset) % (unsigned)r->backend_count);
        backend_health *h = &r->health[i];
        if ((tried & (1u << i)) || h->probing || h->retry_at > now) continue;
        if (attempt->eligible++ == 0) attempt->index = i;
    }
    if (attempt->eligible) {
        backend_health *h = &r->health[attempt->index];
        attempt->ticket = ++h->issued;
        if (h->retry_at) {
            h->probing = 1;
            h->applied = attempt->ticket - 1;
        }
        if (!tried) r->next = (unsigned)attempt->index + 1;
    }
    pthread_mutex_unlock(&health_lock);
    return attempt->eligible;
}

void finish_backend(route *r, const backend_attempt *attempt, int connected,
                    int peer_failed, int64_t now) {
    if (r->backend_count < 2) return;
    pthread_mutex_lock(&health_lock);
    backend_health *h = &r->health[attempt->index];
    if (attempt->ticket > h->applied) {
        // 로컬 실패는 다른 워커의 연결 결과를 무효화할 근거가 없다.
        if (connected || peer_failed) {
            h->retry_at = connected ? 0 : now + 5000;
            h->applied = attempt->ticket;
        }
        h->probing = 0;
    }
    pthread_mutex_unlock(&health_lock);
}

const backend *next_backend(route *r) {
    // Address-only round robin for callers that do not perform a connection attempt.
    pthread_mutex_lock(&health_lock);
    const backend *b = r->backend_count ? &r->backends[r->next++ % (unsigned)r->backend_count] : NULL;
    pthread_mutex_unlock(&health_lock);
    return b;
}
