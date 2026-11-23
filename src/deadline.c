#include "deadline.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>

deadline_ms deadline_after(int milliseconds) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + milliseconds;
}

// Published by the accept loop before joining workers. Each worker closes its own FDs.
static _Atomic deadline_ms shutdown_end;

void set_shutdown_deadline(deadline_ms end) {
    atomic_store(&shutdown_end, end);
}

deadline_ms effective_deadline(deadline_ms end) {
    deadline_ms stop = atomic_load(&shutdown_end);
    return stop && stop < end ? stop : end;
}

int deadline_left(deadline_ms end) {
    int64_t left = effective_deadline(end) - deadline_after(0);
    if (left <= 0) { errno = ETIMEDOUT; return 0; }
    return left > INT_MAX ? INT_MAX : (int)left;
}

int wait_until(int fd, short events, deadline_ms end) {
    struct pollfd p = {.fd = fd, .events = events};
    for (;;) {
        int left = deadline_left(end);
        if (!left) return -1;
        int ready = poll(&p, 1, left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) errno = ETIMEDOUT;
        if (ready <= 0 || !deadline_left(end)) return -1;
        return 0; // Let recv/send/getsockopt report EOF or the actual socket error.
    }
}

int send_until(int fd, const void *buf, size_t n, deadline_ms end) {
    const char *p = buf;
    deadline_ms write_end = end;
    while (n) {
        if (!deadline_left(write_end)) return -1;
        ssize_t count = send(fd, p, n, MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (write_end == end) {
                deadline_ms idle = deadline_after(SOCKET_IDLE_TIMEOUT_MS);
                if (idle < write_end) write_end = idle;
            }
            if (wait_until(fd, POLLOUT, write_end) < 0) return -1;
            continue;
        }
        if (count <= 0) return -1;
        p += count;
        n -= (size_t)count;
        write_end = end;
    }
    return 0;
}
