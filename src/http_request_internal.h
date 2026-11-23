#ifndef HTTP_REQUEST_INTERNAL_H
#define HTTP_REQUEST_INTERNAL_H

#include "deadline.h"
#include "http_request.h"

// Parse the request line and headers into a zero-initialized request.
int read_request_head_until(rio_t *rp, http_request *req, deadline_ms end);

#endif
