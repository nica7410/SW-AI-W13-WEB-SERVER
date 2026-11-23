#ifndef PROXY_INTERNAL_H
#define PROXY_INTERNAL_H
#include <stddef.h>

// Forward URI와 라우팅 설정에서 같은 host:port 문법을 사용한다.
int parse_authority(const char *text, size_t len, const char *default_port,
                    char *host, size_t hostlen, char *port, size_t portlen);
#endif
