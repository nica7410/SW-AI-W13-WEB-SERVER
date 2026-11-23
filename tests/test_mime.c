#include "test.h"
#include "mime.h"

int main(void) {
    CHECK_STR(mime_type("/dir/index.HTML"), "text/html");
    CHECK_STR(mime_type("a.css"), "text/css");
    CHECK_STR(mime_type("a.js"), "application/javascript");
    CHECK_STR(mime_type("a.png"), "image/png");
    CHECK_STR(mime_type("a.jpg"), "image/jpeg");
    CHECK_STR(mime_type("a.jpeg"), "image/jpeg");
    CHECK_STR(mime_type("/dir.v1/file"), "application/octet-stream");
    CHECK_STR(mime_type("a.unknown"), "application/octet-stream");
    return TEST_RESULT();
}
