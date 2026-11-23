#include "server.h"
#include "access_log.h"
#include "server_internal.h"
#include "deadline.h"
#include "cgi.h"
#include "forward_proxy.h"
#include "http_response.h"
#include "net.h"
#include "proxy.h"
#include "proxy_identity.h"
#include <strings.h>
#include "static_file.h"
#include "threadpool.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const server_config *g_cfg;
static route_table g_routes;
static struct local_root static_root, cgi_root;
static atomic_int g_stop;
static threadpool *g_pool;   // run_server 밖(단위 테스트)에서는 NULL — 양보 판단을 하지 않는다
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal handler needs lock-free atomics");

// 유휴 keep-alive 연결이 이 시간보다 오래 조용하고 큐에 기다리는 연결이 있으면 워커를 양보한다
#define IDLE_YIELD_GRACE_MS 100
#define IDLE_POLL_SLICE_MS  50
#define FINAL_LINGER_MS     50
#define SHUTDOWN_GRACE_MS 5000

// 큐가 가득 차 503을 보낸 연결은 클라이언트가 닫을 때까지 최대 이 수만큼 잠깐 붙잡는다
#define MAX_REJECTED     64
#define REJECT_LINGER_MS 1000

// 워커 하나가 동시에 쓰는 fd (연결 1 + CGI의 임시 파일·socketpair·pipe·프로그램 6 + 여유 1)
#define FDS_PER_WORKER 8
#define FD_RESERVE     (32 + MAX_REJECTED)   // 표준 입출력, 듣기 소켓, 루트 디렉터리, 503 대기 연결 등

// 큐에 쌓인 연결 fd와 워커가 쓰는 fd가 프로세스의 열린 파일 한도를 넘지 않게 큐 크기를 맞춘다.
// 넘으면 큐가 차기 전에 accept가 EMFILE로 실패해 503조차 보내지 못하고 연결이 멈춘다.
static int fit_queue_to_fd_limit(int threads, int queue) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) < 0) return queue;
    rlim_t reserve = (rlim_t)threads * FDS_PER_WORKER + FD_RESERVE;
    rlim_t want = reserve + (rlim_t)queue;
    if (limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur < want) {
        // soft 한도는 hard 한도까지 프로세스가 스스로 올릴 수 있다.
        rlim_t raised = limit.rlim_max != RLIM_INFINITY && limit.rlim_max < want ? limit.rlim_max : want;
        struct rlimit next = {.rlim_cur = raised, .rlim_max = limit.rlim_max};
        if (setrlimit(RLIMIT_NOFILE, &next) == 0) limit.rlim_cur = raised;
    }
    if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= want) return queue;
    if (limit.rlim_cur <= reserve) {
        fprintf(stderr, "open file limit %llu is too low for %d workers (minimum %llu with one queue slot)\n",
                (unsigned long long)limit.rlim_cur, threads, (unsigned long long)(reserve + 1));
        return -1;
    }
    int fitted = (int)(limit.rlim_cur - reserve);
    fprintf(stderr, "queue size %d exceeds the open file limit %llu; using %d\n",
            queue, (unsigned long long)limit.rlim_cur, fitted);
    return fitted;
}

void request_stop(void) {
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
}

static int dispatch(int fd, http_request *req) {
    if (is_absolute_uri(req->uri)) {
        char host[256], port[8], target[MAX_URI_LEN];
        if (parse_absolute_uri(req->uri, host, sizeof(host), port, sizeof(port),
                               target, sizeof(target)) < 0) return 400;
        if (!is_listener_authority(fd, host, port, g_cfg->server_name)) {
            req->keep_alive = 0;
            return g_cfg->forward_enabled ? forward_proxy_request(fd, req) : 403;
        }
        // Local dispatch and reverse forwarding use origin-form and the URI's authority.
        int i = 0;
        while (i < req->header_count && strcasecmp(req->headers[i].name, "Host")) i++;
        size_t len = strcspn(req->uri + 7, "/?");
        if (i == MAX_HEADERS || len >= MAX_HEADER_VALUE) return 431;
        if (i == req->header_count) {
            strcpy(req->headers[i].name, "Host");
            req->header_count++;
        }
        memcpy(req->headers[i].value, req->uri + 7, len);
        req->headers[i].value[len] = '\0';
        strcpy(req->uri, target);
    }
    route *r = match_route(&g_routes, req->path);
    if (r) {
        req->keep_alive = 0;
        return reverse_proxy_request(fd, req, r);
    }
    if (!strncmp(req->path, CGI_PREFIX, strlen(CGI_PREFIX))) {
        int status = serve_cgi_at(fd, &cgi_root, req, &req->keep_alive);
        if (status) req->keep_alive = 0;
        return status;
    }
    return serve_static_at(fd, &static_root, req);
}

// 송신을 먼저 닫고 남은 입력을 잠깐 버린 뒤 close한다. 읽지 않은 입력이 남은 채 close하면
// 커널이 RST를 보내 클라이언트가 방금 받은 응답까지 잃을 수 있다.
static void linger_close_input(int fd, int milliseconds) {
    if (shutdown(fd, SHUT_WR) < 0) return;
    deadline_ms end = deadline_after(milliseconds);
    char discard[RIO_BUFSIZE];
    while (wait_until(fd, POLLIN, end) == 0) {
        ssize_t n = recv(fd, discard, sizeof(discard), MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) break;
    }
}

int finish_request(int fd, const http_request *req, int result, int linger_ms) {
    if (result > 0)
        result = send_request_error(fd, result, req->keep_alive, !strcmp(req->method, "HEAD"));
    if (!result && linger_ms > 0) linger_close_input(fd, linger_ms);
    return result;
}

// 503을 보낸 연결의 입력을 버린다. 계속 붙잡을 연결이면 1, 이제 close할 연결이면 0
static int drain_rejected(int fd, short revents, deadline_ms end) {
    if (!deadline_left(end)) return 0;
    if (!revents) return 1;
    if (revents & (POLLERR | POLLNVAL)) return 0;
    char discard[RIO_BUFSIZE];
    for (;;) {
        ssize_t n = recv(fd, discard, sizeof(discard), MSG_DONTWAIT);
        if (n > 0) {
            if (!deadline_left(end)) return 0;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);   // EOF·오류면 닫는다
    }
}

static int others_waiting(void) {
    return g_pool && threadpool_has_waiting(g_pool);
}

// 요청의 첫 바이트를 기다린다. 0 = 읽을 데이터가 있음(EOF 포함), -1 = 연결을 닫는다.
// - 종료 중이면 이미 도착한 요청만 처리하고 조용한 연결은 바로 닫는다 (큐에 남은 연결 포함).
// - allow_yield: keep-alive의 다음 요청을 기다리는 중이면, 워커 하나가 유휴 연결 하나를 붙잡지 않도록
//   큐에서 기다리는 연결이 생겼을 때 유휴 연결을 닫고 양보한다. 서버가 유휴 keep-alive 연결을 먼저
//   닫는 것은 HTTP가 허용하는 동작이다 (RFC 9112 9.6). 새 연결의 첫 요청은 양보하지 않는다.
static int wait_request_start(const rio_t *rio, int allow_yield) {
    if (atomic_load_explicit(&g_stop, memory_order_relaxed) &&
        (allow_yield || !deadline_left(INT64_MAX))) return -1;
    if (rio->cnt) return 0;   // 파이프라이닝으로 이미 받은 요청
    deadline_ms start = deadline_after(0);
    deadline_ms end = start + KEEPALIVE_TIMEOUT_SEC * 1000;
    struct pollfd p = {.fd = rio->fd, .events = POLLIN};
    for (;;) {
        int stopping = atomic_load_explicit(&g_stop, memory_order_relaxed);
        int left = deadline_left(end);
        if (!left) return -1;
        int slice = stopping ? 0 : left < IDLE_POLL_SLICE_MS ? left : IDLE_POLL_SLICE_MS;
        int ready = poll(&p, 1, slice);
        if (ready > 0) return stopping && allow_yield ? -1 : 0;
        if (ready < 0 && errno != EINTR) return -1;
        if (stopping) return -1;
        if (allow_yield && deadline_after(0) - start >= IDLE_YIELD_GRACE_MS && others_waiting())
            return 1;
    }
}

void handle_connection(int connfd) {
    rio_t rio;
    rio_readinitb(&rio, connfd);
    for (int served = 0; ; served++) {
        int wait = wait_request_start(&rio, served > 0);
        if (wait) {
            // 양보로 닫을 때 클라이언트가 막 보낸 요청이 있으면 RST 대신 FIN을 받게 한다.
            if (wait > 0 || (served && atomic_load_explicit(&g_stop, memory_order_relaxed)))
                linger_close_input(connfd, FINAL_LINGER_MS);
            break;
        }
        http_request req;
        deadline_ms read_started = g_cfg && g_cfg->verbose ? deadline_after(0) : -1;
        int result = read_request(&rio, &req);
        if (read_started >= 0 && (result >= 0 || *req.method)) access_begin(read_started);
        int linger_ms = 0;
        if (result) {
            req.keep_alive = 0;
            if (result > 0) linger_ms = 250;
        } else {
            // 대기 중인 연결에 워커를 양보할 때는 이 응답을 마지막으로 보낸다.
            if ((req.keep_alive && others_waiting()) || atomic_load_explicit(&g_stop, memory_order_relaxed))
                req.keep_alive = 0;
            result = dispatch(connfd, &req);
        }
        if (atomic_load_explicit(&g_stop, memory_order_relaxed)) req.keep_alive = 0;
        if (!req.keep_alive && !linger_ms) linger_ms = FINAL_LINGER_MS;
        result = finish_request(connfd, &req, result, linger_ms);
        access_finish(&req, result);
        free_request(&req);
        if (result < 0 || !req.keep_alive) break;
    }
    close(connfd);
}

int run_server(const server_config *cfg) {
    int queue_size = fit_queue_to_fd_limit(cfg->threads, cfg->queue_size);
    if (queue_size < 0) return -1;
    g_cfg = cfg;
    g_routes.count = 0;
    if (cfg->routes_path && load_routes(cfg->routes_path, &g_routes) < 0) return -1;
    int error = init_local_root(&static_root, cfg->root);
    if (error) {
        fprintf(stderr, "%sstatic root %s: %s\n", cfg->root_explicit ? "" : "warning: ",
                cfg->root, strerror(error));
        if (cfg->root_explicit) return -1;
    }
    error = init_local_root(&cgi_root, cfg->cgi_dir);
    if (error) fprintf(stderr, "warning: CGI directory %s: %s\n", cfg->cgi_dir, strerror(error));
    int status = -1;
    int listenfd = open_listenfd(cfg->port);
    if (listenfd < 0) {
        fprintf(stderr, "cannot listen on port %s\n", cfg->port);
        goto close_roots;
    }
    int flags = fcntl(listenfd, F_GETFL);
    if (flags < 0 || fcntl(listenfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(listenfd);
        goto close_roots;
    }
    if (!*proxy_via_comment())
        fprintf(stderr, "warning: using only socket addresses for proxy loop detection\n");
    threadpool *pool = threadpool_create(cfg->threads, queue_size, handle_connection);
    if (!pool) {
        fprintf(stderr, "cannot create worker pool\n");
        close(listenfd);
        goto close_roots;
    }
    g_pool = pool;
    status = 0;
    // fds[0]은 듣기 소켓, fds[1..]은 503을 보내고 클라이언트가 닫기를 기다리는 연결
    struct pollfd fds[1 + MAX_REJECTED] = {{.fd = listenfd, .events = POLLIN}};
    deadline_ms linger_end[1 + MAX_REJECTED];
    int nfds = 1;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        // 503 대기 자리가 없으면 accept를 멈춘다. 새 연결은 커널의 listen backlog에서 기다린다.
        fds[0].events = nfds < 1 + MAX_REJECTED ? POLLIN : 0;
        // 종료 신호가 워커에서 처리되거나 poll 직전에 도착해도 accept에 갇히지 않는다.
        int ready = poll(fds, (nfds_t)nfds, nfds > 1 ? 50 : 250);
        if (atomic_load_explicit(&g_stop, memory_order_relaxed)) break;
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            status = -1;
            break;
        }
        for (int i = nfds - 1; i > 0; i--) {
            if (drain_rejected(fds[i].fd, fds[i].revents, linger_end[i])) continue;
            close(fds[i].fd);
            fds[i] = fds[--nfds];
            linger_end[i] = linger_end[nfds];
        }
        if (!(fds[0].revents & POLLIN)) continue;
        int fd = accept(listenfd, NULL, NULL);
        if (fd < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept");
                if (errno == EMFILE || errno == ENFILE) poll(NULL, 0, 100);
            }
            continue;
        }
        // Send small response bodies without waiting for the header's TCP acknowledgement.
        int nodelay = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        // BSD 계열에서는 accept 소켓이 listen 소켓의 O_NONBLOCK을 상속할 수 있다.
        struct timeval tv = {.tv_sec = KEEPALIVE_TIMEOUT_SEC};
        int accepted_flags = fcntl(fd, F_GETFL);
        if (accepted_flags < 0 || fcntl(fd, F_SETFL, accepted_flags & ~O_NONBLOCK) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
            close(fd);
        } else if (threadpool_submit(pool, fd) < 0) {
            // 요청을 읽지 않은 채 바로 close하면 RST가 503 응답을 앞질러 클라이언트가 응답을 잃는다.
            // 송신만 닫고, 클라이언트가 닫을 때까지 accept 루프가 입력을 버리다가 close한다.
            if (cfg->verbose) access_begin(-1);
            int sent = send_error(fd, 503, 0);
            access_finish(NULL, sent);
            if (sent < 0 || nfds == 1 + MAX_REJECTED || shutdown(fd, SHUT_WR) < 0) {
                close(fd);
            } else {
                fds[nfds] = (struct pollfd){.fd = fd, .events = POLLIN};
                linger_end[nfds++] = deadline_after(REJECT_LINGER_MS);
            }
        }
    }
    request_stop();
    set_shutdown_deadline(deadline_after(SHUTDOWN_GRACE_MS));
    for (int i = 1; i < nfds; i++) close(fds[i].fd);
    close(listenfd);
    threadpool_destroy(pool);
    g_pool = NULL;
close_roots:
    close_local_root(&static_root);
    close_local_root(&cgi_root);
    return status;
}
