// [http_request.h] HTTP 요청 파서 — 요청 라인 / 헤더 / 본문(Content-Length 또는 chunked)을 읽어 구조체로 만든다
//
// ⚠ 공통 인터페이스: 이 구조체와 함수 시그니처는 모든 모듈이 사용한다. 바꾸면 모든 핸들러와 테스트를 함께 고쳐야 한다
//
// 반환 규약 (이 파일의 int 반환 함수 공통)
//    0  = 성공
//   >0  = 클라이언트에게 보낼 HTTP 오류 상태 코드 (400, 413, 414, 431, 501, 505 ...)
//   -1  = 연결 종료 (EOF, 읽기 에러) → 응답 없이 close
#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stddef.h>
#include "rio.h"

#define MAX_LINE         8192            // 요청 라인·헤더 한 줄 최대 길이
#define MAX_URI_LEN      2048            // 넘으면 414
#define MAX_HEADERS      32              // 넘으면 431
#define MAX_HEADER_NAME  64
#define MAX_HEADER_VALUE 1024            // 넘으면 431
#define MAX_BODY_LEN     (1024 * 1024)   // 넘으면 413
#define MAX_CHUNK_EXTENSION_BYTES 8192   // 요청 한 건의 청크 확장 총량

typedef struct {
    char name[MAX_HEADER_NAME];
    char value[MAX_HEADER_VALUE];        // 앞뒤 공백 제거한 값
} http_header;

typedef struct {
    char method[8];                      // "GET", "POST", "HEAD" (그 외는 501)
    char uri[MAX_URI_LEN];               // 원본 URI 그대로 ("/cgi-bin/adder?a=1&b=2" 또는 Forward Proxy의 "http://host/a.html")
    char path[MAX_URI_LEN];              // '?' 앞 ("/cgi-bin/adder"). 절대 URI면 호스트 뒤의 경로 부분
    char query[MAX_URI_LEN];             // '?' 뒤, 없으면 "" ("a=1&b=2")
    char version[16];                    // "HTTP/1.0" 또는 "HTTP/1.1"
    http_header headers[MAX_HEADERS];
    int header_count;
    size_t content_length;               // 본문 바이트 수, 없으면 0
    char *body;                          // malloc한 본문, 없으면 NULL → free_request로 해제
    int keep_alive;                      // 1이면 응답 후 연결 유지
} http_request;

// 요청 라인 한 줄("GET /a?x=1 HTTP/1.1\r\n") 파싱 → method, uri, path, query, version 채움
int parse_request_line(const char *line, http_request *req);

// 헤더 한 줄("Host: localhost\r\n") 파싱 → headers에 추가. Content-Length, Connection이면 해당 필드도 갱신
int parse_header_line(const char *line, http_request *req);

// 소켓에서 요청 하나를 끝까지 읽기: 요청 라인 → 빈 줄까지 헤더 → 길이 기반 본문 또는 chunked 본문 디코딩
int read_request(rio_t *rp, http_request *req);

// 헤더 값 찾기 (이름은 대소문자 무시). 없으면 NULL
const char *get_header(const http_request *req, const char *name);

// req가 malloc한 메모리(body) 해제
void free_request(http_request *req);

#endif
