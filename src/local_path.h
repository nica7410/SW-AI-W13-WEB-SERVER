#ifndef LOCAL_PATH_H
#define LOCAL_PATH_H
#include <limits.h>
#include <stddef.h>

// URL 경로의 퍼센트 인코딩을 한 번 해석한다.
int decode_url_path(const char *path, char *decoded, size_t capacity);
// URL 경로를 디코딩하고 root 안의 실제 파일 경로를 구한다.
int resolve_local_path(const char *root, const char *path, int directory_index,
                       char resolved[PATH_MAX]);
int file_error_status(int error);
#endif
