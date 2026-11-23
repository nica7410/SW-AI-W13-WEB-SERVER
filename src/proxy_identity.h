#ifndef PROXY_IDENTITY_H
#define PROXY_IDENTITY_H
#include "http_request.h"

// Numeric host:port for the listener or upstream. The listener accepts IPv4.
#define ENDPOINT_SIZE 80
int proxy_endpoint(int fd, int peer, char value[ENDPOINT_SIZE]);
int is_listener_authority(int client, const char *host, const char *port, const char *alias);
int via_revisits_listener(int client, const http_request *req);
int proxy_returns_to_hop(int client, int upstream, const http_request *req);
#endif
