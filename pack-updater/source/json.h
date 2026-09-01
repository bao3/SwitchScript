#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char tag[64];
    char name[256];
    char url[768];
    int64_t size;
} GhAsset;

/* Parse GitHub /releases/latest JSON. Picks the first asset whose name
 * starts with asset_prefix and ends with .zip. Returns 0 on success. */
int gh_parse_latest_zip(const char *json, const char *asset_prefix,
                        GhAsset *out, char *err, size_t err_sz);
