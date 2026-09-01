#pragma once

#include <stddef.h>

/* Return 1 if inner_name exists (slash-normalized) in the zip. */
int pack_zip_has_file(const char *zip_path, const char *inner_name);

/* Extract zip into dest_root (e.g. "sdmc:/" or "/tmp/out/").
 * Skips unsafe paths (..), absolute paths, and Nintendo/.
 * progress: return 0 to continue, non-zero to abort.
 * Returns 0 on success. */
int pack_unzip(const char *zip_path, const char *dest_root,
               int (*progress)(int i, int n, const char *name, void *ud),
               void *ud, const char *extract_last, char *err, size_t err_sz);

/* Extract one zip member (exact inner path, or matching basename) to dest_file. */
int pack_unzip_one(const char *zip_path, const char *inner_name,
                   const char *dest_file, char *err, size_t err_sz);
