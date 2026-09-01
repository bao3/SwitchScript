#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"
#include "miniz.h"
#include "unzip.h"

static int fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    return 1;
}

static int unzip_nop(int i, int n, const char *name, void *ud) {
    (void)i; (void)n; (void)name; (void)ud;
    return 0;
}

int main(void) {
    const char *json =
        "{"
        "\"tag_name\":\"1.11.2\","
        "\"name\":\"Nintendo Switch SD Card Build - Atmosphere 1.11.2\","
        "\"assets\":["
        "{\"name\":\"PackUpdater.nro\",\"size\":123},"
        "{\"name\":\"NS-SD-Card-Atmosphere-1.11.2.zip\",\"size\":51850967,"
        "\"browser_download_url\":\"https://github.com/bao3/SwitchScript/releases/download/1.11.2/NS-SD-Card-Atmosphere-1.11.2.zip\"}"
        "]"
        "}";

    GhAsset a;
    char err[128];
    if (gh_parse_latest_zip(json, "NS-SD-Card-Atmosphere-", &a, err, sizeof err) != 0)
        return fail(err);
    if (strcmp(a.tag, "1.11.2") != 0) return fail("tag");
    if (strcmp(a.name, "NS-SD-Card-Atmosphere-1.11.2.zip") != 0) return fail("name");
    if (a.size != 51850967) return fail("size");
    if (!strstr(a.url, "NS-SD-Card-Atmosphere-1.11.2.zip")) return fail("url");
    printf("json ok tag=%s size=%lld\n", a.tag, (long long)a.size);

    const char *rate = "{\"message\":\"API rate limit exceeded for 1.2.3.4.\"}";
    if (gh_parse_latest_zip(rate, "NS-SD-Card-Atmosphere-", &a, err, sizeof err) == 0)
        return fail("rate limit should fail");
    if (!strstr(err, "rate limit")) return fail("rate message");
    printf("rate-limit json ok\n");

    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    const char *zpath = "tests/sample.zip";
    mkdir("tests", 0777);
    if (!mz_zip_writer_init_file(&zip, zpath, 0)) return fail("zip writer init");
    const char *payload = "package3-bytes";
    if (!mz_zip_writer_add_mem(&zip, "atmosphere/package3", payload, strlen(payload), MZ_DEFAULT_COMPRESSION))
        return fail("add package3");
    if (!mz_zip_writer_add_mem(&zip, "bootloader/hekate_ipl.ini", "x", 1, MZ_DEFAULT_COMPRESSION))
        return fail("add ini");
    if (!mz_zip_writer_add_mem(&zip, "../evil.txt", "no", 2, MZ_DEFAULT_COMPRESSION))
        return fail("add evil");
    if (!mz_zip_writer_add_mem(&zip, "Nintendo/save.bin", "no", 2, MZ_DEFAULT_COMPRESSION))
        return fail("add nintendo");
    if (!mz_zip_writer_add_mem(&zip, "switch/PackUpdater/PackUpdater.nro", "nro-bytes", 9, MZ_DEFAULT_COMPRESSION))
        return fail("add nro");
    if (!mz_zip_writer_finalize_archive(&zip)) return fail("finalize");
    mz_zip_writer_end(&zip);

    if (!pack_zip_has_file(zpath, "atmosphere/package3")) return fail("has package3");
    if (pack_zip_has_file(zpath, "missing.bin")) return fail("missing should be 0");

    const char *out = "tests/out";
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", out, out);
    if (system(cmd) != 0) return fail("mkdir out");
    if (pack_unzip(zpath, out, unzip_nop, NULL, NULL, err, sizeof err) != 0) return fail(err);

    struct stat st;
    if (stat("tests/out/atmosphere/package3", &st) != 0) return fail("extracted package3");
    if (stat("tests/out/bootloader/hekate_ipl.ini", &st) != 0) return fail("extracted ini");
    if (stat("tests/evil.txt", &st) == 0) return fail("zip-slip leaked");
    if (stat("tests/out/Nintendo/save.bin", &st) == 0) return fail("Nintendo/ should be skipped");
    printf("unzip ok (zip-slip and Nintendo/ skipped)\n");

    if (pack_unzip(zpath, out, unzip_nop, NULL, "tests/out/bootloader/hekate_ipl.ini", err, sizeof err) != 0)
        return fail(err);
    if (stat("tests/out/bootloader/hekate_ipl.ini", &st) != 0) return fail("deferred file missing");
    printf("extract_last still wrote the nro-equivalent file\n");

    if (system("rm -rf tests/one && mkdir -p tests/one") != 0) return fail("mkdir one");
    if (pack_unzip_one(zpath, "switch/PackUpdater/PackUpdater.nro",
                       "tests/one/PackUpdater.nro", err, sizeof err) != 0)
        return fail(err);
    FILE *fp = fopen("tests/one/PackUpdater.nro", "r");
    if (!fp) return fail("unzip_one dest missing");
    char buf[16] = {0};
    if (!fgets(buf, sizeof buf, fp)) { fclose(fp); return fail("unzip_one empty"); }
    fclose(fp);
    if (strcmp(buf, "nro-bytes") != 0) return fail("unzip_one content");
    if (stat("tests/one/atmosphere/package3", &st) == 0) return fail("unzip_one extracted extra");
    if (pack_unzip_one(zpath, "PackUpdater.nro", "tests/one/by-base.nro", err, sizeof err) != 0)
        return fail(err);
    if (stat("tests/one/by-base.nro", &st) != 0) return fail("basename match");
    printf("unzip_one ok (exact path and basename)\n");

    char tag[64];
    if (gh_tag_from_effective_url(
            "https://gh.heibang.club/https://github.com/bao3/SwitchScript/releases/tag/1.11.2",
            tag, sizeof tag) != 0)
        return fail("tag from gh proxy url");
    if (strcmp(tag, "1.11.2") != 0) return fail("tag value");
    if (gh_tag_from_effective_url("https://github.com/bao3/SwitchScript/releases/tag/1.11.2?foo=1",
                                  tag, sizeof tag) != 0)
        return fail("tag from github url");
    if (strcmp(tag, "1.11.2") != 0) return fail("tag query strip");
    if (gh_tag_from_effective_url("https://example.com/nope", tag, sizeof tag) == 0)
        return fail("bad url should fail");
    printf("tag parse ok\n");

    printf("ALL HOST TESTS PASSED\n");
    return 0;
}
