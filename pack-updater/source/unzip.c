#include "unzip.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "miniz.h"

static void slash_normalize(char *s) {
    for (; s && *s; s++) {
        if (*s == '\\') *s = '/';
    }
}

static int path_is_safe(const char *name) {
    if (!name || !*name) return 0;
    if (name[0] == '/' || name[0] == '\\') return 0;
    if (strstr(name, "..")) return 0;
    if (strchr(name, ':')) return 0;
    if (strncmp(name, "Nintendo/", 9) == 0 || strcmp(name, "Nintendo") == 0) return 0;
    if (strncmp(name, "nintendo/", 9) == 0) return 0;
    return 1;
}

static void join_dest(char *out, size_t n, const char *root, const char *rel) {
    size_t rl = strlen(root);
    if (rl && (root[rl - 1] == '/' || root[rl - 1] == '\\'))
        snprintf(out, n, "%s%s", root, rel);
    else
        snprintf(out, n, "%s/%s", root, rel);
    slash_normalize(out);
}

static int mkdir_p_of_file(char *path) {
    char *p = path;
    /* skip sdmc: prefix */
    if (!strncmp(p, "sdmc:", 5)) p += 5;
    if (*p == '/') p++;
    for (; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0777);
            *p = '/';
        }
    }
    return 0;
}

int pack_zip_has_file(const char *zip_path, const char *inner_name) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) return 0;
    int n = (int)mz_zip_reader_get_num_files(&zip);
    int found = 0;
    char want[512];
    snprintf(want, sizeof want, "%s", inner_name);
    slash_normalize(want);
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        slash_normalize(st.m_filename);
        if (strcmp(st.m_filename, want) == 0) {
            found = 1;
            break;
        }
    }
    mz_zip_reader_end(&zip);
    return found;
}

int pack_unzip(const char *zip_path, const char *dest_root,
               int (*progress)(int i, int n, const char *name, void *ud),
               void *ud, char *err, size_t err_sz) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        snprintf(err, err_sz, "open zip failed: %s", zip_path);
        return -1;
    }

    int n = (int)mz_zip_reader_get_num_files(&zip);
    if (n <= 0) {
        mz_zip_reader_end(&zip);
        snprintf(err, err_sz, "zip is empty");
        return -1;
    }

    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        slash_normalize(st.m_filename);

        if (progress && progress(i, n, st.m_filename, ud) != 0) {
            mz_zip_reader_end(&zip);
            snprintf(err, err_sz, "extract aborted");
            return -1;
        }

        if (!path_is_safe(st.m_filename)) continue;

        char dest[1024];
        join_dest(dest, sizeof dest, dest_root, st.m_filename);

        if (st.m_is_directory || (st.m_filename[0] && st.m_filename[strlen(st.m_filename) - 1] == '/')) {
            mkdir_p_of_file(dest);
            mkdir(dest, 0777);
            continue;
        }

        mkdir_p_of_file(dest);
        if (!mz_zip_reader_extract_to_file(&zip, (mz_uint)i, dest, 0)) {
            snprintf(err, err_sz, "extract failed: %s", st.m_filename);
            mz_zip_reader_end(&zip);
            return -1;
        }
    }

    mz_zip_reader_end(&zip);
    return 0;
}
