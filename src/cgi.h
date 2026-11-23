// [cgi.h] 동적 요청 처리 — CGI 프로그램을 fork + execve로 실행하고 그 출력을 응답으로 보낸다
// 반환 규약은 static_file.h와 같음
#ifndef CGI_H
#define CGI_H

#include "http_request.h"

#define CGI_PREFIX "/cgi-bin/"   // 이 경로로 시작하는 요청을 CGI로 처리

// 독립 호출은 Connection: close로 응답한다. 서버 워커는 길이를 명시한 CGI 응답 뒤 연결을 재사용할 수 있다.
int serve_cgi(int fd, const char *cgi_dir, const http_request *req);
// CGI client redirect destination, including an optional fragment.
int is_cgi_redirect_uri(const char *value);

#endif
