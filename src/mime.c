#include "mime.h"
#include <string.h>
#include <strings.h>

const char *mime_type(const char *path) {
    static const struct { const char *ext, *type; } types[] = {
        {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"},
        {"js", "application/javascript"}, {"json", "application/json"},
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"gif", "image/gif"}, {"txt", "text/plain"}, {"svg", "image/svg+xml"},
        {"ico", "image/vnd.microsoft.icon"}, {"webp", "image/webp"},
        {"woff2", "font/woff2"}, {"pdf", "application/pdf"}
    };
    const char *file = strrchr(path, '/');
    const char *ext = strrchr(file ? file + 1 : path, '.');
    if (ext) {
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
            if (strcasecmp(ext + 1, types[i].ext) == 0) return types[i].type;
    }
    return "application/octet-stream";
}
