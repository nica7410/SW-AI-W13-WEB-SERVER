#ifndef RESPONSE_STREAM_H
#define RESPONSE_STREAM_H
#include "http_request.h"
#include "deadline.h"

#define RESPONSE_TIMEOUT_MS 30000
int response_until(int clientfd, int source, const http_request *req, int from_cgi,
                   deadline_ms header_end, deadline_ms response_end);

// CGI·원 서버 응답을 클라이언트로 전달한다. 헤더를 보낸 뒤의 실패는 항상 -1.
int relay_response(int clientfd, int source, const http_request *req, int from_cgi);
#endif
