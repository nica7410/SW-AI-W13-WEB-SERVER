// [http_response.h] HTTP 응답 생성 — 상태 라인 + 표준 헤더 + 본문 송신
#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>
#include <time.h>

#define HTTP_DATE_SIZE 30
int format_http_date(time_t when, char value[HTTP_DATE_SIZE]);

// 상태 코드 → 사유 문구 (200 → "OK", 404 → "Not Found"). 모르는 코드면 "Unknown"
const char *status_text(int status);

// 상태 라인 + 헤더(Content-Type, Content-Length, Connection) + 빈 줄까지만 송신. 성공 0, 실패 -1
// 큰 파일은 헤더를 먼저 보내고 본문을 나눠 보낼 때 사용
int send_headers(int fd, int status, const char *content_type, size_t content_length, int keep_alive);

// 헤더 + 본문을 한 번에 송신. 성공 0, 실패 -1
int send_response(int fd, int status, const char *content_type, const void *body, size_t len, int keep_alive);

// 오류 응답: 상태 코드에 맞는 짧은 HTML 본문을 만들어 송신. 성공 0, 실패 -1
int send_error(int fd, int status, int keep_alive);

#endif
