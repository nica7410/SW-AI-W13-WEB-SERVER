#ifndef TEST_TCP_PAIR_H
#define TEST_TCP_PAIR_H
#include "test.h"
#include "net.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static void require(int ok, const char *what) {
    if (!ok) { perror(what); exit(1); }
}

static void tcp_pair(int pair[2]) {
    int listener = open_listenfd("0");
    require(listener >= 0, "open_listenfd");
    struct sockaddr_in addr = {0};
    socklen_t n = sizeof(addr);
    require(getsockname(listener, (struct sockaddr *)&addr, &n) == 0, "getsockname");
    CHECK(n == sizeof(addr) && addr.sin_family == AF_INET);
    CHECK(addr.sin_addr.s_addr == htonl(INADDR_ANY) && addr.sin_port != 0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    pair[1] = socket(AF_INET, SOCK_STREAM, 0);
    require(pair[1] >= 0 && connect(pair[1], (struct sockaddr *)&addr, sizeof(addr)) == 0, "connect");
    pair[0] = accept(listener, NULL, NULL);
    require(pair[0] >= 0, "accept");
    close(listener);
}

#endif
