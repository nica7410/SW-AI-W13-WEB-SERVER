#ifndef HTTP_FRAMING_H
#define HTTP_FRAMING_H
#include <stdint.h>

// Keep the final-coding flag across repeated Transfer-Encoding fields.
int parse_transfer_codings(const char *value, int *chunked);
int parse_chunk_size(const char *line, uintmax_t *size);
#endif
