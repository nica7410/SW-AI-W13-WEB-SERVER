#ifndef UPSTREAM_H
#define UPSTREAM_H
#include "deadline.h"
#include "http_request.h"
#include <netdb.h>

typedef int lookup_fn(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int connect_until(const char *host, const char *port, deadline_ms end, lookup_fn *lookup);
int relay_until(int client, int upstream, const http_request *req, const char *target,
                deadline_ms end);
int response_until(int client, int source, const http_request *req, int from_cgi,
                   deadline_ms end);
#endif
