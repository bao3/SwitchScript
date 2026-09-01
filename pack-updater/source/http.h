#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t len;
} HttpMem;

typedef int (*http_pump_fn)(int64_t now, int64_t total, void *ud);

int http_global_init(void);
void http_global_cleanup(void);

/* GET url into a malloc'd buffer (caller frees data). */
int http_get_mem(const char *url, const char *proxy, const char *token,
                 http_pump_fn pump, void *ud,
                 HttpMem *out, char *err, size_t err_sz);

/* GET url to a file path, with progress. */
int http_get_file(const char *url, const char *dest,
                  const char *proxy, const char *token,
                  http_pump_fn pump, void *ud,
                  char *err, size_t err_sz);

/* Follow redirects, discard body, return final URL (and optional Content-Length). */
int http_follow(const char *url, const char *proxy, const char *token,
                http_pump_fn pump, void *ud,
                char *effective, size_t effective_sz,
                int64_t *content_length,
                char *err, size_t err_sz);

/* HEAD (or GET-nobody) for Content-Length. content_length may be -1 if unknown. */
int http_content_length(const char *url, const char *proxy, const char *token,
                        int64_t *content_length, char *err, size_t err_sz);
