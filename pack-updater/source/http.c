#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UA "PackUpdater/1.0 (bao3/SwitchScript)"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} GrowBuf;

typedef struct {
    http_pump_fn pump;
    void *ud;
} PumpWrap;

static size_t write_grow(char *ptr, size_t size, size_t nmemb, void *userdata) {
    GrowBuf *g = userdata;
    size_t n = size * nmemb;
    if (g->len + n + 1 > g->cap) {
        size_t cap = g->cap ? g->cap : 4096;
        while (cap < g->len + n + 1) cap *= 2;
        if (cap > 4u * 1024u * 1024u) return 0; /* API JSON should never be this big */
        char *p = realloc(g->data, cap);
        if (!p) return 0;
        g->data = p;
        g->cap = cap;
    }
    memcpy(g->data + g->len, ptr, n);
    g->len += n;
    g->data[g->len] = 0;
    return n;
}

static size_t write_file(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return fwrite(ptr, size, nmemb, (FILE *)userdata);
}

static int xfer(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal;
    (void)ulnow;
    PumpWrap *w = clientp;
    if (!w || !w->pump) return 0;
    return w->pump((int64_t)dlnow, (int64_t)dltotal, w->ud);
}

static void apply_common(CURL *curl, const char *url, const char *proxy,
                         const char *token, struct curl_slist **hdrs,
                         PumpWrap *pump) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    /* Switch does not ship Mozilla CAs; GitHub + S3 are HTTPS. */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    if (proxy && proxy[0]) curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
    if (token && token[0]) {
        char h[192];
        snprintf(h, sizeof h, "Authorization: Bearer %s", token);
        *hdrs = curl_slist_append(*hdrs, h);
    }
    if (url && strstr(url, "api.github.com")) {
        *hdrs = curl_slist_append(*hdrs, "Accept: application/vnd.github+json");
    }
    if (*hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *hdrs);
    if (pump) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, pump);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
}

int http_global_init(void) {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

void http_global_cleanup(void) { curl_global_cleanup(); }

int http_get_mem(const char *url, const char *proxy, const char *token,
                 http_pump_fn pump, void *ud,
                 HttpMem *out, char *err, size_t err_sz) {
    memset(out, 0, sizeof *out);
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, err_sz, "curl_easy_init failed");
        return -1;
    }
    GrowBuf g = {0};
    PumpWrap wrap = {pump, ud};
    struct curl_slist *hdrs = NULL;
    apply_common(curl, url, proxy, token, &hdrs, pump ? &wrap : NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_grow);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &g);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(hdrs);
    if (rc != CURLE_OK) {
        snprintf(err, err_sz, "GET %s: %s", url, curl_easy_strerror(rc));
        free(g.data);
        return -1;
    }
    out->data = g.data ? g.data : calloc(1, 1);
    out->len = g.len;
    return 0;
}

int http_get_file(const char *url, const char *dest,
                  const char *proxy, const char *token,
                  http_pump_fn pump, void *ud,
                  char *err, size_t err_sz) {
    FILE *fp = fopen(dest, "wb");
    if (!fp) {
        snprintf(err, err_sz, "cannot write %s", dest);
        return -1;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        snprintf(err, err_sz, "curl_easy_init failed");
        return -1;
    }
    PumpWrap wrap = {pump, ud};
    struct curl_slist *hdrs = NULL;
    apply_common(curl, url, proxy, token, &hdrs, pump ? &wrap : NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(hdrs);
    fclose(fp);
    if (rc != CURLE_OK) {
        snprintf(err, err_sz, "download failed: %s", curl_easy_strerror(rc));
        remove(dest);
        return -1;
    }
    return 0;
}

static size_t write_discard(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

int http_follow(const char *url, const char *proxy, const char *token,
                http_pump_fn pump, void *ud,
                char *effective, size_t effective_sz,
                int64_t *content_length,
                char *err, size_t err_sz) {
    if (effective && effective_sz) effective[0] = 0;
    if (content_length) *content_length = -1;
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, err_sz, "curl_easy_init failed");
        return -1;
    }
    PumpWrap wrap = {pump, ud};
    struct curl_slist *hdrs = NULL;
    apply_common(curl, url, proxy, token, &hdrs, pump ? &wrap : NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_discard);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        snprintf(err, err_sz, "GET %s: %s", url, curl_easy_strerror(rc));
        curl_easy_cleanup(curl);
        curl_slist_free_all(hdrs);
        return -1;
    }
    if (effective && effective_sz) {
        char *fin = NULL;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &fin);
        if (fin) snprintf(effective, effective_sz, "%s", fin);
    }
    if (content_length) {
        curl_off_t cl = -1;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        *content_length = (int64_t)cl;
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(hdrs);
    return 0;
}

int http_content_length(const char *url, const char *proxy, const char *token,
                        int64_t *content_length, char *err, size_t err_sz) {
    if (content_length) *content_length = -1;
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, err_sz, "curl_easy_init failed");
        return -1;
    }
    struct curl_slist *hdrs = NULL;
    apply_common(curl, url, proxy, token, &hdrs, NULL);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        snprintf(err, err_sz, "HEAD %s: %s", url, curl_easy_strerror(rc));
        curl_easy_cleanup(curl);
        curl_slist_free_all(hdrs);
        return -1;
    }
    if (content_length) {
        curl_off_t cl = -1;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        *content_length = (int64_t)cl;
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(hdrs);
    return 0;
}
