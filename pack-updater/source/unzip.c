#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

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

static int path_eq(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) return 0;
    if (!strncmp(a, "sdmc:", 5)) a += 5;
    if (!strncmp(b, "sdmc:", 5)) b += 5;
    while (*a == '/' || *a == '\\') a++;
    while (*b == '/' || *b == '\\') b++;
    for (; *a && *b; a++, b++) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

#ifdef __SWITCH__
#include <switch.h>
#endif

typedef struct {
    FILE *fp;
    int io_err;
} ExtractWrite;

static size_t extract_write_cb(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
    ExtractWrite *w = opaque;
    (void)ofs;
    if (!w || !w->fp || w->io_err) return 0;
    size_t wrote = fwrite(buf, 1, n, w->fp);
    if (wrote != n)
        w->io_err = errno ? errno : -1;
    return wrote;
}

static void switch_commit(void) {
#ifdef __SWITCH__
    fsdevCommitDevice("sdmc");
#endif
}

static int replace_over(const char *tmp, const char *dest, char *err, size_t err_sz) {
    remove(dest);
    if (rename(tmp, dest) == 0) return 0;

    FILE *in = fopen(tmp, "rb");
    FILE *out = fopen(dest, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        snprintf(err, err_sz, "replace %s failed errno=%d", dest, errno);
        return -1;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            snprintf(err, err_sz, "copy %s failed errno=%d", dest, errno);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    remove(tmp);
    return 0;
}

/* Horizon/libnx: fopen("wb")+fwrite on an existing multi-MB file (package3)
 * often fails even with free space. Write to dest.tmp, pre-size, then rename. */
static int extract_index_to_path(mz_zip_archive *zip, mz_uint idx, const char *dest,
                                 char *err, size_t err_sz) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, idx, &st)) {
        snprintf(err, err_sz, "stat failed: %s",
                 mz_zip_get_error_string(mz_zip_get_last_error(zip)));
        return -1;
    }
    slash_normalize(st.m_filename);

    char dest_copy[1024];
    snprintf(dest_copy, sizeof dest_copy, "%s", dest);
    slash_normalize(dest_copy);
    mkdir_p_of_file(dest_copy);

    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", dest_copy);
    remove(tmp);

    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        snprintf(err, err_sz, "open %s errno=%d", tmp, errno);
        return -1;
    }
    if (st.m_uncomp_size > 0) {
        (void)ftruncate(fileno(fp), (off_t)st.m_uncomp_size);
        (void)fseek(fp, 0, SEEK_SET);
    }

    ExtractWrite wr = { .fp = fp, .io_err = 0 };
    mz_bool ok = mz_zip_reader_extract_to_callback(zip, idx, extract_write_cb, &wr, 0);
    int flush_err = fflush(fp);
    int close_err = fclose(fp);

    if (!ok || wr.io_err || flush_err || close_err) {
        mz_zip_error mzerr = mz_zip_get_last_error(zip);
        snprintf(err, err_sz, "extract failed: %s (%s errno=%d)",
                 st.m_filename,
                 mz_zip_get_error_string(mzerr),
                 wr.io_err ? wr.io_err : errno);
        remove(tmp);
        return -1;
    }

    if (replace_over(tmp, dest_copy, err, err_sz) != 0) {
        remove(tmp);
        return -1;
    }
    if (st.m_uncomp_size >= (mz_uint64)(1u << 20))
        switch_commit();
    return 0;
}

static int is_ams_core(const char *rel) {
    const char *n = rel;
    if (!n || !n[0]) return 0;
    if (!strncmp(n, "atmosphere/", 11) || !strncmp(n, "Atmosphere/", 11))
        n += 11;
    else
        return 0;
    if (strchr(n, '/') || strchr(n, '\\')) return 0;
    return path_eq(n, "package3") || path_eq(n, "stratosphere.romfs");
}

int pack_unzip(const char *zip_path, const char *dest_root,
               int (*progress)(int i, int n, const char *name, void *ud),
               void *ud, const char *extract_last, char *err, size_t err_sz) {
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

    int deferred = -1;
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
        if (is_ams_core(st.m_filename)) {
            size_t used = strlen(dest);
            if (used + 5 < sizeof dest)
                memcpy(dest + used, ".aio", 5);
        }

        if (st.m_is_directory || (st.m_filename[0] && st.m_filename[strlen(st.m_filename) - 1] == '/')) {
            mkdir_p_of_file(dest);
            mkdir(dest, 0777);
            continue;
        }

        if (extract_last && path_eq(dest, extract_last)) {
            deferred = i;
            continue;
        }

        mkdir_p_of_file(dest);
        if (extract_index_to_path(&zip, (mz_uint)i, dest, err, err_sz) != 0) {
            mz_zip_reader_end(&zip);
            return -1;
        }
    }

    if (deferred >= 0) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, (mz_uint)deferred, &st)) {
            slash_normalize(st.m_filename);
            char dest[1024];
            join_dest(dest, sizeof dest, dest_root, st.m_filename);
            mkdir_p_of_file(dest);
            if (extract_index_to_path(&zip, (mz_uint)deferred, dest, err, err_sz) != 0) {
                mz_zip_reader_end(&zip);
                return -1;
            }
        }
    }

    mz_zip_reader_end(&zip);
    switch_commit();
    return 0;
}

static const char *path_base(const char *p) {
    const char *s = p;
    for (; p && *p; p++) {
        if (*p == '/' || *p == '\\') s = p + 1;
    }
    return s ? s : "";
}

static int inner_matches(const char *name, const char *want) {
    if (path_eq(name, want)) return 1;
    return path_eq(path_base(name), path_base(want));
}

int pack_unzip_one(const char *zip_path, const char *inner_name,
                   const char *dest_file, char *err, size_t err_sz) {
    if (!zip_path || !inner_name || !dest_file || !inner_name[0] || !dest_file[0]) {
        snprintf(err, err_sz, "unzip_one: bad args");
        return -1;
    }
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        snprintf(err, err_sz, "open zip failed: %s", zip_path);
        return -1;
    }
    int n = (int)mz_zip_reader_get_num_files(&zip);
    int found = -1;
    char found_name[512];
    found_name[0] = 0;
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        slash_normalize(st.m_filename);
        if (!path_is_safe(st.m_filename)) continue;
        if (st.m_is_directory) continue;
        if (inner_matches(st.m_filename, inner_name)) {
            found = i;
            snprintf(found_name, sizeof found_name, "%s", st.m_filename);
            break;
        }
    }
    if (found < 0) {
        mz_zip_reader_end(&zip);
        snprintf(err, err_sz, "zip missing %s", inner_name);
        return -1;
    }
    char dest[1024];
    snprintf(dest, sizeof dest, "%s", dest_file);
    slash_normalize(dest);
    mkdir_p_of_file(dest);
    if (extract_index_to_path(&zip, (mz_uint)found, dest, err, err_sz) != 0) {
        mz_zip_reader_end(&zip);
        return -1;
    }
    mz_zip_reader_end(&zip);
    switch_commit();
    return 0;
}
