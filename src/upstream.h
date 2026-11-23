#ifndef UPSTREAM_H
#define UPSTREAM_H
#include "deadline.h"
#include "http_request.h"
#include "router.h"
#include <netdb.h>

typedef int lookup_fn(const char *, const char *, const struct addrinfo *, struct addrinfo **);
typedef int connect_fn(const char *, const char *, deadline_ms, lookup_fn *, int *peer_failed);
connect_fn connect_for_route;
int reverse_proxy_until(int client, const http_request *req, route *r, deadline_ms end,
                        connect_fn *connect_backend);
int connect_until(const char *host, const char *port, deadline_ms end, lookup_fn *lookup);
int relay_until(int client, int upstream, const http_request *req, const char *target,
                deadline_ms end);
#endif
