#ifndef HTTP_FRAMING_H
#define HTTP_FRAMING_H
#include <stddef.h>
#include <stdint.h>

// Keep the final-coding flag across repeated Transfer-Encoding fields.
int parse_transfer_codings(const char *value, int *chunked);
// When requested, count the complete extension suffix including its leading whitespace.
int parse_chunk_size(const char *line, uintmax_t *size, size_t *extension_bytes);
#endif
