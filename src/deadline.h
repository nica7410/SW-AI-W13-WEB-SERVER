#ifndef DEADLINE_H
#define DEADLINE_H

#include "rio.h"
#include <stdint.h>

// Absolute CLOCK_MONOTONIC milliseconds. A zero line-read deadline uses ordinary RIO.
typedef int64_t deadline_ms;
deadline_ms deadline_after(int milliseconds);
int deadline_left(deadline_ms end);
int wait_until(int fd, short events, deadline_ms end);
int send_until(int fd, const void *buf, size_t n, deadline_ms end);
ssize_t rio_readline_until(rio_t *rp, void *buf, size_t maxlen, deadline_ms end);
// 정확히 n바이트를 end까지 읽는다. 조금씩 도착해도 마감은 늘어나지 않는다.
// 읽은 바이트 수(EOF면 n보다 작음), 실패·마감 초과 -1 (errno ETIMEDOUT). end가 0이면 rio_readnb와 같다
ssize_t rio_readn_until(rio_t *rp, void *buf, size_t n, deadline_ms end);
#endif
