#include "rio.h"
#include "deadline.h"
#include <poll.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

ssize_t rio_writen(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t count = write(fd, p + sent, n - sent);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        sent += (size_t)count;
    }
    return (ssize_t)sent;
}

void rio_readinitb(rio_t *rp, int fd) {
    rp->fd = fd;
    rp->cnt = 0;
    rp->bufp = rp->buf;
}

static ssize_t refill(rio_t *rp, deadline_ms end) {
    if (rp->cnt == 0) {
        ssize_t count;
        if (end) {
            deadline_ms idle = deadline_after(SOCKET_IDLE_TIMEOUT_MS);
            if (idle < end) end = idle;
        }
        for (;;) {
            if (end) {
                if (wait_until(rp->fd, POLLIN, end) < 0) return -1;
                count = recv(rp->fd, rp->buf, sizeof(rp->buf), MSG_DONTWAIT);
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            } else count = read(rp->fd, rp->buf, sizeof(rp->buf));
            if (count >= 0 || errno != EINTR) break;
        }
        if (count <= 0) return count;
        rp->cnt = count;
        rp->bufp = rp->buf;
    }
    return rp->cnt;
}

static void consume(rio_t *rp, char *dst, size_t n) {
    memcpy(dst, rp->bufp, n);
    rp->bufp += n;
    rp->cnt -= (ssize_t)n;
}

ssize_t rio_readsome_until(rio_t *rp, void *buf, size_t n, deadline_ms end) {
    if (!deadline_left(end)) return -1;
    ssize_t available = refill(rp, end);
    if (available <= 0) return available;
    if (n > (size_t)available) n = (size_t)available;
    consume(rp, buf, n);
    return (ssize_t)n;
}

ssize_t rio_readline_until(rio_t *rp, void *buf, size_t maxlen, deadline_ms end) {
    char *out = buf;
    size_t used = 0;
    if (maxlen == 0) return 0;

    while (used < maxlen - 1) {
        if (end && !rp->cnt && !deadline_left(end)) return -1;   // 이미 받은 데이터는 마감과 무관
        ssize_t available = refill(rp, end);
        if (available < 0) return -1;
        if (available == 0) break;
        size_t n = maxlen - 1 - used;
        if (n > (size_t)available) n = (size_t)available;
        const char *newline = memchr(rp->bufp, '\n', n);
        if (newline) n = (size_t)(newline - rp->bufp) + 1;
        consume(rp, out + used, n);
        used += n;
        if (newline) break;
    }
    out[used] = '\0';
    return (ssize_t)used;
}

ssize_t rio_readlineb(rio_t *rp, void *buf, size_t maxlen) {
    return rio_readline_until(rp, buf, maxlen, 0);
}

ssize_t rio_readnb(rio_t *rp, void *buf, size_t n) {
    return rio_readn_until(rp, buf, n, 0);
}

ssize_t rio_readn_until(rio_t *rp, void *buf, size_t n, deadline_ms end) {
    char *out = buf;
    size_t used = 0;
    while (used < n) {
        if (end && !rp->cnt && !deadline_left(end)) return -1;   // 이미 받은 데이터는 마감과 무관
        ssize_t available = refill(rp, end);
        if (available < 0) return -1;
        if (available == 0) break;
        size_t take = n - used;
        if (take > (size_t)available) take = (size_t)available;
        consume(rp, out + used, take);
        used += take;
    }
    return (ssize_t)used;
}
