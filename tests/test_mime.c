#include "test.h"
#include "mime.h"

int main(void) {
    CHECK_STR(mime_type("/dir/index.HTML"), "text/html");
    CHECK_STR(mime_type("a.css"), "text/css");
    CHECK_STR(mime_type("a.js"), "application/javascript");
    CHECK_STR(mime_type("a.png"), "image/png");
    CHECK_STR(mime_type("a.jpg"), "image/jpeg");
    CHECK_STR(mime_type("a.jpeg"), "image/jpeg");
    CHECK_STR(mime_type("logo.SVG"), "image/svg+xml");
    CHECK_STR(mime_type("favicon.ico"), "image/vnd.microsoft.icon");
    CHECK_STR(mime_type("photo.webp"), "image/webp");
    CHECK_STR(mime_type("font.woff2"), "font/woff2");
    CHECK_STR(mime_type("document.pdf"), "application/pdf");
    CHECK_STR(mime_type("/dir.v1/file"), "application/octet-stream");
    CHECK_STR(mime_type("a.unknown"), "application/octet-stream");
    return TEST_RESULT();
}
