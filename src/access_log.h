#ifndef ACCESS_LOG_H
#define ACCESS_LOG_H

#include "http_request.h"
#include "router.h"

void access_begin(void);
void access_attempt(const backend *candidate);
void access_connected(const backend *selected);
void access_headers_sent(int status);
// req == NULL이면 요청을 읽기 전 큐 초과로 거절한 연결이다.
void access_finish(const http_request *req, int result);

#endif
