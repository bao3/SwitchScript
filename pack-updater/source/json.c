#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *find_key(const char *json, const char *key) {
    char pat[160];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    return strstr(json, pat);
}

static int read_json_string(const char *after_key, char *out, size_t out_sz) {
    const char *p = skip_ws(after_key);
    if (*p == ':') p = skip_ws(p + 1);
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                case '"':
                case '\\':
                case '/': out[i++] = *p; break;
                case 'u': /* skip \uXXXX */
                    if (p[1] && p[2] && p[3] && p[4]) p += 4;
                    break;
                default: out[i++] = *p; break;
            }
            p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = 0;
    return (*p == '"') ? 0 : -1;
}

static int read_json_number(const char *after_key, int64_t *out) {
    const char *p = skip_ws(after_key);
    if (*p == ':') p = skip_ws(p + 1);
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (!isdigit((unsigned char)*p)) return -1;
    int64_t v = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = neg ? -v : v;
    return 0;
}

static const char *after_key(const char *hit, const char *key) {
    return hit + strlen(key) + 2; /* quotes around key */
}

int gh_parse_latest_zip(const char *json, const char *asset_prefix,
                        GhAsset *out, char *err, size_t err_sz) {
    if (!json || !asset_prefix || !out) return -1;
    memset(out, 0, sizeof *out);

    const char *msg = find_key(json, "message");
    if (msg && !find_key(json, "tag_name")) {
        char m[160];
        if (read_json_string(after_key(msg, "message"), m, sizeof m) == 0) {
            snprintf(err, err_sz, "GitHub API: %s", m);
        } else {
            snprintf(err, err_sz, "GitHub API error");
        }
        return -1;
    }

    const char *tag = find_key(json, "tag_name");
    if (!tag || read_json_string(after_key(tag, "tag_name"), out->tag, sizeof out->tag) != 0) {
        snprintf(err, err_sz, "JSON missing tag_name");
        return -1;
    }

    const char *assets = strstr(json, "\"assets\"");
    if (!assets) {
        snprintf(err, err_sz, "JSON missing assets");
        return -1;
    }

    const char *p = assets;
    size_t prefix_len = strlen(asset_prefix);
    while ((p = strstr(p, "\"name\""))) {
        char name[256];
        if (read_json_string(after_key(p, "name"), name, sizeof name) != 0) {
            p += 6;
            continue;
        }
        size_t nlen = strlen(name);
        int match = (nlen > 4 && strcmp(name + nlen - 4, ".zip") == 0 &&
                     strncmp(name, asset_prefix, prefix_len) == 0);
        if (!match) {
            p += 6;
            continue;
        }

        /* Do not stop at the first '}' — GitHub asset objects nest "uploader":{...}. */
        char chunk[4096];
        size_t window = strlen(p);
        if (window >= sizeof chunk) window = sizeof chunk - 1;
        memcpy(chunk, p, window);
        chunk[window] = 0;

        const char *url_k = find_key(chunk, "browser_download_url");
        const char *size_k = find_key(chunk, "size");
        if (!url_k || read_json_string(after_key(url_k, "browser_download_url"),
                                       out->url, sizeof out->url) != 0) {
            snprintf(err, err_sz, "asset missing browser_download_url");
            return -1;
        }
        if (size_k) read_json_number(after_key(size_k, "size"), &out->size);
        snprintf(out->name, sizeof out->name, "%s", name);
        return 0;
    }

    snprintf(err, err_sz, "no asset matching %s*.zip", asset_prefix);
    return -1;
}
