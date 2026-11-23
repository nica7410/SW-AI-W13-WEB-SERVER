#include "net.h"
#include "upstream.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "proxy.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

int open_listenfd(const char *port) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM,
                             .ai_flags = AI_PASSIVE | AI_NUMERICSERV};
    struct addrinfo *list;
    if (getaddrinfo(NULL, port, &hints, &list) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        int yes = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0 &&
            bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, 1024) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}

// Keep timed-out DNS lookups bounded. Only the resolver owns an abandoned job.
#define MAX_RESOLVERS 8
struct query {
    char host[NI_MAXHOST], port[NI_MAXSERV];
    struct addrinfo *addresses;
    lookup_fn *lookup;
    int done, abandoned, error;
};

static pthread_mutex_t query_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t query_once = PTHREAD_ONCE_INIT;
static pthread_cond_t query_changed;
static int query_init_error, active_queries;

static void init_query_condition(void) {
#ifdef __APPLE__
    query_init_error = pthread_cond_init(&query_changed, NULL);
#else
    pthread_condattr_t attr;
    if ((query_init_error = pthread_condattr_init(&attr)) != 0) return;
    query_init_error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!query_init_error) query_init_error = pthread_cond_init(&query_changed, &attr);
    pthread_condattr_destroy(&attr);
#endif
}

static int wait_for_query(deadline_ms end) {
    end = effective_deadline(end);
    int left = deadline_left(end);
    if (!left) return ETIMEDOUT;
#ifdef __APPLE__
    struct timespec relative = {.tv_sec = left / 1000, .tv_nsec = (left % 1000) * 1000000L};
    return pthread_cond_timedwait_relative_np(&query_changed, &query_lock, &relative);
#else
    struct timespec absolute = {.tv_sec = end / 1000, .tv_nsec = (end % 1000) * 1000000L};
    return pthread_cond_timedwait(&query_changed, &query_lock, &absolute);
#endif
}

static void *resolve_job(void *arg) {
    struct query *q = arg;
    pthread_detach(pthread_self());
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV};
    struct addrinfo *list = NULL;
    int rc = q->lookup(q->host, q->port, &hints, &list);
    int error = rc == EAI_MEMORY ? ENOMEM : rc == EAI_SYSTEM ? errno : EHOSTUNREACH;
    if (rc) list = NULL;
    pthread_mutex_lock(&query_lock);
    active_queries--;
    if (q->abandoned) {
        if (list) freeaddrinfo(list);
        free(q);
    } else {
        q->done = 1;
        q->addresses = list;
        q->error = rc ? error : 0;
    }
    pthread_cond_broadcast(&query_changed);
    pthread_mutex_unlock(&query_lock);
    return NULL;
}

static int resolve_until(const char *host, const char *port, deadline_ms end,
                         lookup_fn *lookup, struct addrinfo **list, int *contacted) {
    struct in6_addr numeric;
    if (inet_pton(AF_INET, host, &numeric) || inet_pton(AF_INET6, host, &numeric)) {
        struct addrinfo hints = {.ai_socktype = SOCK_STREAM,
                                 .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV};
        return getaddrinfo(host, port, &hints, list) == 0 ? 0 : EHOSTUNREACH;
    }
    pthread_once(&query_once, init_query_condition);
    if (query_init_error) return query_init_error;
    struct query *q = calloc(1, sizeof(*q));
    if (!q) return ENOMEM;
    if (strlen(host) >= sizeof(q->host) || strlen(port) >= sizeof(q->port)) {
        free(q);
        return EINVAL;
    }
    strcpy(q->host, host);
    strcpy(q->port, port);
    q->lookup = lookup;

    int error = 0;
    pthread_t thread;
    pthread_mutex_lock(&query_lock);
    while (!error && active_queries >= MAX_RESOLVERS) error = wait_for_query(end);
    if (!error && !deadline_left(end)) error = ETIMEDOUT;
    if (!error) error = pthread_create(&thread, NULL, resolve_job, q);
    if (!error) {
        active_queries++;
        *contacted = 1;
        while (!error && !q->done) error = wait_for_query(end);
        if (!q->done) {
            q->abandoned = 1;
            pthread_mutex_unlock(&query_lock);
            return error;
        }
        *list = q->addresses;
        if (!*list) error = q->error ? q->error : EHOSTUNREACH;
    }
    pthread_mutex_unlock(&query_lock);
    free(q);
    return error;
}

static int connect_address(int fd, const struct addrinfo *a, deadline_ms end) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    if (connect(fd, a->ai_addr, a->ai_addrlen) < 0) {
        if (errno != EINPROGRESS && errno != EINTR) return -1;
        if (wait_until(fd, POLLOUT, end) < 0) return -1;
        int error;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) return -1;
        if (error) { errno = error; return -1; }
    }
    return fcntl(fd, F_SETFL, flags);
}

int connect_for_route(const char *host, const char *port, deadline_ms end, lookup_fn *lookup,
                      int *peer_failed) {
    *peer_failed = 0;
    if (!deadline_left(end)) return -1;
    struct addrinfo *list = NULL;
    int contacted = 0;
    int error = resolve_until(host, port, end, lookup, &list, &contacted);
    int fd = -1;
    if (!error) {
        // 조회 성공 뒤 연결을 시도하기 전에 예산이 끝나면 목적지 실패로 세지 않는다.
        contacted = 0;
        error = ETIMEDOUT;
        for (struct addrinfo *a = list; a && deadline_left(end); a = a->ai_next) {
            fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) { error = errno; continue; }
            contacted = 1;
            if (connect_address(fd, a, end) == 0) break;
            error = errno;
            close(fd);
            fd = -1;
        }
    }
    if (list) freeaddrinfo(list);
    *peer_failed = fd < 0 && contacted &&
        (error == ECONNREFUSED || error == ETIMEDOUT || error == EHOSTUNREACH ||
         error == ENETUNREACH || error == ECONNRESET);
    if (!deadline_left(end)) {
        if (fd >= 0) close(fd);
        errno = ETIMEDOUT;
        return -1;
    }
    if (fd < 0) errno = error;
    return fd;
}

int connect_until(const char *host, const char *port, deadline_ms end, lookup_fn *lookup) {
    int peer_failed;
    return connect_for_route(host, port, end, lookup, &peer_failed);
}

int open_clientfd(const char *host, const char *port) {
    return connect_until(host, port, deadline_after(PROXY_TIMEOUT_SEC * 1000), getaddrinfo);
}
