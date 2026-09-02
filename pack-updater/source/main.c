#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "ams_bpc.h"
#include "http.h"
#include "json.h"
#include "unzip.h"
#include "TegraExplorer_bin.h"

#define VERSION "1.5.1"
#define WORK_DIR "sdmc:/switch/PackUpdater"
#define ZIP_PATH WORK_DIR "/update.zip"
#define INSTALLED_PATH WORK_DIR "/installed.txt"
#define CONFIG_PATH WORK_DIR "/config.ini"
#define DEFAULT_REPO "bao3/SwitchScript"
#define DEFAULT_PREFIX "NS-SD-Card-Atmosphere-"
#define DEFAULT_EXTRACT "sdmc:/"
#define DEFAULT_GH_PROXY "https://gh.heibang.club"
#define REQUIRED_INNER "atmosphere/package3"
#define SELF_INNER "switch/PackUpdater/PackUpdater.nro"
#define MIN_BATTERY_PCT 20
#define MIN_FREE_MB 512
#define NRO_MIN_BYTES (100 * 1024)
#define NRO_MAX_BYTES (16 * 1024 * 1024)
#define TE_PATH WORK_DIR "/TegraExplorer.bin"
#define STARTUP_TE "sdmc:/startup.te"
#define PKG3_AIO "sdmc:/atmosphere/package3.aio"
#define STRAT_AIO "sdmc:/atmosphere/stratosphere.romfs.aio"
#define IRAM_PAYLOAD_MAX_SIZE 0x2F000

typedef struct {
    char repo[128];
    char asset_prefix[128];
    char proxy[256];
    char gh_proxy[256];
    char token[128];
    char extract_to[64];
    int min_zip_mb;
    int min_battery_pct;
    int min_free_mb;
} Config;

typedef struct {
    PadState *pad;
    const char *label;
    int abort;
} Pump;

static Config g_cfg;
static char g_err[256];
static char g_installed[64];
static char g_self_nro[512];
static GhAsset g_asset;
static int g_have_asset;

static void write_startup_te(void);
static void ensure_te_on_sd(void);

static void trim(char *s) {
    char *a = s;
    while (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

static void cfg_defaults(Config *c) {
    memset(c, 0, sizeof *c);
    snprintf(c->repo, sizeof c->repo, "%s", DEFAULT_REPO);
    snprintf(c->asset_prefix, sizeof c->asset_prefix, "%s", DEFAULT_PREFIX);
    snprintf(c->extract_to, sizeof c->extract_to, "%s", DEFAULT_EXTRACT);
    snprintf(c->gh_proxy, sizeof c->gh_proxy, "%s", DEFAULT_GH_PROXY);
    c->min_zip_mb = 10;
    c->min_battery_pct = MIN_BATTERY_PCT;
    c->min_free_mb = MIN_FREE_MB;
}

static void write_default_config(void) {
    FILE *fp = fopen(CONFIG_PATH, "w");
    if (!fp) return;
    fputs(
        "# PackUpdater config\n"
        "[updater]\n"
        "repo = bao3/SwitchScript\n"
        "asset_prefix = NS-SD-Card-Atmosphere-\n"
        "extract_to = sdmc:/\n"
        "min_zip_mb = 10\n"
        "min_battery_pct = 20\n"
        "min_free_mb = 512\n"
        "gh_proxy = https://gh.heibang.club\n"
        "# HTTP CONNECT proxy (usually leave empty; gh_proxy is enough in CN):\n"
        "# proxy = http://192.168.50.10:7893\n"
        "# token =\n",
        fp);
    fclose(fp);
}

static void load_config(void) {
    cfg_defaults(&g_cfg);
    FILE *fp = fopen(CONFIG_PATH, "r");
    if (!fp) {
        write_default_config();
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line;
        char *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcmp(k, "repo")) snprintf(g_cfg.repo, sizeof g_cfg.repo, "%s", v);
        else if (!strcmp(k, "asset_prefix")) snprintf(g_cfg.asset_prefix, sizeof g_cfg.asset_prefix, "%s", v);
        else if (!strcmp(k, "proxy")) snprintf(g_cfg.proxy, sizeof g_cfg.proxy, "%s", v);
        else if (!strcmp(k, "gh_proxy")) snprintf(g_cfg.gh_proxy, sizeof g_cfg.gh_proxy, "%s", v);
        else if (!strcmp(k, "token")) snprintf(g_cfg.token, sizeof g_cfg.token, "%s", v);
        else if (!strcmp(k, "extract_to")) snprintf(g_cfg.extract_to, sizeof g_cfg.extract_to, "%s", v);
        else if (!strcmp(k, "min_zip_mb")) g_cfg.min_zip_mb = atoi(v);
        else if (!strcmp(k, "min_battery_pct")) g_cfg.min_battery_pct = atoi(v);
        else if (!strcmp(k, "min_free_mb")) g_cfg.min_free_mb = atoi(v);
    }
    fclose(fp);
}

static void load_installed(void) {
    g_installed[0] = 0;
    FILE *fp = fopen(INSTALLED_PATH, "r");
    if (!fp) return;
    if (fgets(g_installed, sizeof g_installed, fp)) trim(g_installed);
    fclose(fp);
}

static void save_installed(const char *tag) {
    FILE *fp = fopen(INSTALLED_PATH, "w");
    if (!fp) return;
    fprintf(fp, "%s\n", tag);
    fclose(fp);
}

static void bar(char *out, size_t n, int64_t now, int64_t total) {
    int width = 24;
    int fill = 0;
    if (total > 0) fill = (int)((now * width) / total);
    if (fill < 0) fill = 0;
    if (fill > width) fill = width;
    char tmp[40];
    for (int i = 0; i < width; i++) tmp[i] = (i < fill) ? '#' : '.';
    tmp[width] = 0;
    if (total > 0)
        snprintf(out, n, "[%s] %lld / %lld MB", tmp,
                 (long long)(now / (1024 * 1024)),
                 (long long)(total / (1024 * 1024)));
    else
        snprintf(out, n, "[%s] %lld MB", tmp, (long long)(now / (1024 * 1024)));
}

static int pump_cb(int64_t now, int64_t total, void *ud) {
    Pump *p = ud;
    if (!appletMainLoop()) {
        p->abort = 1;
        return 1;
    }
    padUpdate(p->pad);
    if (padGetButtonsDown(p->pad) & HidNpadButton_Plus) {
        p->abort = 1;
        return 1;
    }
    char b[80];
    bar(b, sizeof b, now, total);
    printf("\x1b[20;1H%s\n\x1b[21;1H%s                    \n", p->label, b);
    consoleUpdate(NULL);
    return 0;
}

static int unzip_progress(int i, int n, const char *name, void *ud) {
    Pump *p = ud;
    if (!appletMainLoop()) {
        p->abort = 1;
        return 1;
    }
    padUpdate(p->pad);
    if (padGetButtonsDown(p->pad) & HidNpadButton_Plus) {
        p->abort = 1;
        return 1;
    }
    printf("\x1b[20;1HExtracting %d / %d          \n\x1b[21;1H%.50s                    \n",
           i + 1, n, name ? name : "");
    consoleUpdate(NULL);
    return 0;
}

static void banner(const char *status) {
    printf("\x1b[1;1H");
    printf("================================\n");
    printf(" PackUpdater %s\n", VERSION);
    printf("================================\n");
    printf("repo   : %s\n", g_cfg.repo);
    printf("gh     : %s\n", g_cfg.gh_proxy[0] ? g_cfg.gh_proxy : "(direct)");
    printf("proxy  : %s\n", g_cfg.proxy[0] ? g_cfg.proxy : "(none)");
    printf("installed : %s\n", g_installed[0] ? g_installed : "(unknown)");
    if (g_have_asset) {
        printf("latest    : %s\n", g_asset.tag);
        printf("asset     : %s\n", g_asset.name);
        printf("size      : %lld MB\n", (long long)(g_asset.size / (1024 * 1024)));
    } else {
        printf("latest    : (not fetched)\n");
        printf("asset     : -\n");
        printf("size      : -\n");
    }
    printf("--------------------------------\n");
    printf("%s\n", status ? status : "");
    if (g_err[0]) printf("error: %s\n", g_err);
    printf("--------------------------------\n");
    printf("[A] download + extract to SD root\n");
    printf("[B] update PackUpdater first\n");
    printf("[Y] recheck GitHub\n");
    printf("[X] reboot\n");
    printf("[+] quit\n");
    printf("\nThis overlays atmosphere/bootloader/switch from\n");
    printf("the pack. Nintendo/ and games are not touched.\n");
    printf("package3 is applied on reboot via TegraExplorer.\n");
    printf("A does a full update in one shot. B is optional.\n");
}

static void redraw(const char *status) {
    consoleClear();
    banner(status);
    consoleUpdate(NULL);
}

static void via_gh(char *out, size_t n, const char *url) {
    if (!g_cfg.gh_proxy[0]) {
        snprintf(out, n, "%s", url);
        return;
    }
    if (!strncmp(url, g_cfg.gh_proxy, strlen(g_cfg.gh_proxy))) {
        snprintf(out, n, "%s", url);
        return;
    }
    size_t bl = strlen(g_cfg.gh_proxy);
    while (bl && g_cfg.gh_proxy[bl - 1] == '/') bl--;
    snprintf(out, n, "%.*s/%s", (int)bl, g_cfg.gh_proxy, url);
}

static int fetch_latest(PadState *pad) {
    g_have_asset = 0;
    g_err[0] = 0;
    Pump p = {.pad = pad, .label = "GitHub", .abort = 0};
    const char *tok = g_cfg.gh_proxy[0] ? NULL : g_cfg.token;

    if (g_cfg.gh_proxy[0]) {
        char src[256], url[768], final[768];
        snprintf(src, sizeof src, "https://github.com/%s/releases/latest", g_cfg.repo);
        via_gh(url, sizeof url, src);
        redraw("Checking gh.heibang.club ...");
        if (http_follow(url, g_cfg.proxy, tok, pump_cb, &p, final, sizeof final, NULL,
                        g_err, sizeof g_err) != 0) {
            redraw("Check failed.");
            return -1;
        }
        if (gh_tag_from_effective_url(final, g_asset.tag, sizeof g_asset.tag) != 0) {
            snprintf(g_err, sizeof g_err, "no /releases/tag/ in %s", final);
            redraw("Parse failed.");
            return -1;
        }
        snprintf(g_asset.name, sizeof g_asset.name, "%s%s.zip", g_cfg.asset_prefix, g_asset.tag);
        snprintf(src, sizeof src, "https://github.com/%s/releases/download/%s/%s",
                 g_cfg.repo, g_asset.tag, g_asset.name);
        via_gh(g_asset.url, sizeof g_asset.url, src);
        int64_t cl = -1;
        char ignore[128];
        if (http_content_length(g_asset.url, g_cfg.proxy, tok, &cl, ignore, sizeof ignore) == 0 && cl > 0)
            g_asset.size = cl;
        else
            g_asset.size = 0;
        g_have_asset = 1;
    } else {
        char url[256];
        snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", g_cfg.repo);
        redraw("Checking GitHub API...");
        HttpMem mem = {0};
        if (http_get_mem(url, g_cfg.proxy, tok, pump_cb, &p, &mem, g_err, sizeof g_err) != 0) {
            redraw("Check failed.");
            return -1;
        }
        if (gh_parse_latest_zip(mem.data, g_cfg.asset_prefix, &g_asset, g_err, sizeof g_err) != 0) {
            free(mem.data);
            redraw("Parse failed.");
            return -1;
        }
        free(mem.data);
        g_have_asset = 1;
    }

    if (g_installed[0] && strcmp(g_installed, g_asset.tag) == 0)
        redraw("Already up to date. A still re-applies.");
    else
        redraw("Update available.");
    return 0;
}

static int64_t file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

static int battery_ok_to_extract(char *err, size_t err_sz) {
    int need = g_cfg.min_battery_pct > 0 ? g_cfg.min_battery_pct : MIN_BATTERY_PCT;
    if (R_FAILED(psmInitialize())) return 0; /* cannot read: do not block */
    u32 pct = 100;
    PsmChargerType ch = PsmChargerType_Unconnected;
    psmGetBatteryChargePercentage(&pct);
    psmGetChargerType(&ch);
    psmExit();
    int charging = (ch != PsmChargerType_Unconnected);
    if ((int)pct < need && !charging) {
        snprintf(err, err_sz, "battery %u%%, not charging (need >= %d%% or plug in)", pct, need);
        return -1;
    }
    return 0;
}

static int sd_has_free(int64_t need, char *err, size_t err_sz) {
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) return 0;
    s64 free_bytes = 0;
    if (R_FAILED(fsFsGetFreeSpace(fs, "/", &free_bytes))) return 0;
    if (free_bytes < need) {
        snprintf(err, err_sz, "SD free %lld MB < need %lld MB",
                 (long long)(free_bytes / (1024 * 1024)),
                 (long long)(need / (1024 * 1024)));
        return -1;
    }
    return 0;
}

static int sd_ok_to_extract(int64_t zip_bytes, char *err, size_t err_sz) {
    int min_mb = g_cfg.min_free_mb > 0 ? g_cfg.min_free_mb : MIN_FREE_MB;
    int64_t need = (int64_t)min_mb * 1024 * 1024;
    if (zip_bytes > 0) {
        int64_t with_zip = zip_bytes + 64LL * 1024 * 1024;
        if (with_zip > need) need = with_zip;
    }
    return sd_has_free(need, err, err_sz);
}

static const char *self_nro_path(void) {
    if (g_self_nro[0]) return g_self_nro;
    return WORK_DIR "/PackUpdater.nro";
}

static int zip_looks_valid(void) {
    int64_t min_bytes = (int64_t)g_cfg.min_zip_mb * 1024 * 1024;
    if (file_size(ZIP_PATH) < min_bytes) return 0;
    return pack_zip_has_file(ZIP_PATH, REQUIRED_INNER);
}

static int nro_size_ok(int64_t sz) {
    return sz >= (int64_t)NRO_MIN_BYTES && sz <= (int64_t)NRO_MAX_BYTES;
}

static int replace_file(const char *from, const char *to, char *err, size_t err_sz) {
    if (!from || !to || !from[0] || !to[0]) {
        snprintf(err, err_sz, "replace_file: bad args");
        return -1;
    }
    remove(to);
    if (rename(from, to) != 0) {
        snprintf(err, err_sz, "could not write %s", to);
        return -1;
    }
    return 0;
}

static int extract_self_from_zip(void) {
    return pack_unzip_one(ZIP_PATH, SELF_INNER, self_nro_path(), g_err, sizeof g_err);
}

static int ensure_zip(PadState *pad) {
    int64_t min_bytes = (int64_t)g_cfg.min_zip_mb * 1024 * 1024;
    if (zip_looks_valid()) {
        redraw("Using already-downloaded zip.");
        return 0;
    }
    remove(ZIP_PATH);
    Pump p = {.pad = pad, .label = "Downloading pack", .abort = 0};
    redraw("Downloading...");
    if (http_get_file(g_asset.url, ZIP_PATH, g_cfg.proxy,
                      g_cfg.gh_proxy[0] ? NULL : g_cfg.token,
                      pump_cb, &p, g_err, sizeof g_err) != 0) {
        redraw("Download failed.");
        return -1;
    }
    if (p.abort) {
        remove(ZIP_PATH);
        redraw("Aborted.");
        return -1;
    }
    int64_t sz = file_size(ZIP_PATH);
    if (sz < min_bytes) {
        snprintf(g_err, sizeof g_err, "downloaded %lld bytes, need >= %d MB",
                 (long long)sz, g_cfg.min_zip_mb);
        remove(ZIP_PATH);
        redraw("Refusing tiny zip.");
        return -1;
    }
    if (!pack_zip_has_file(ZIP_PATH, REQUIRED_INNER)) {
        snprintf(g_err, sizeof g_err, "zip missing %s", REQUIRED_INNER);
        remove(ZIP_PATH);
        redraw("Not a valid SD pack.");
        return -1;
    }
    return 0;
}

static int do_self_update(PadState *pad) {
    if (!g_have_asset) {
        snprintf(g_err, sizeof g_err, "check GitHub first (press Y)");
        redraw("No release info.");
        return -1;
    }
    if (battery_ok_to_extract(g_err, sizeof g_err) != 0) {
        redraw("Charge the Switch first.");
        return -1;
    }
    if (sd_has_free(16LL * 1024 * 1024, g_err, sizeof g_err) != 0) {
        redraw("Not enough SD free space.");
        return -1;
    }

    appletSetAutoSleepDisabled(true);
    mkdir(WORK_DIR, 0777);

    char src[256], url[768], tmp[1024];
    snprintf(src, sizeof src, "https://github.com/%s/releases/download/%s/PackUpdater.nro",
             g_cfg.repo, g_asset.tag);
    via_gh(url, sizeof url, src);
    snprintf(tmp, sizeof tmp, "%s.new", self_nro_path());
    remove(tmp);

    Pump p = {.pad = pad, .label = "Downloading PackUpdater.nro", .abort = 0};
    redraw("Downloading PackUpdater.nro ...");
    int got_nro = 0;
    if (http_get_file(url, tmp, g_cfg.proxy,
                      g_cfg.gh_proxy[0] ? NULL : g_cfg.token,
                      pump_cb, &p, g_err, sizeof g_err) == 0 && !p.abort) {
        int64_t sz = file_size(tmp);
        if (nro_size_ok(sz)) {
            if (replace_file(tmp, self_nro_path(), g_err, sizeof g_err) == 0)
                got_nro = 1;
        } else {
            snprintf(g_err, sizeof g_err, "nro size %lld not plausible", (long long)sz);
        }
    }
    remove(tmp);

    if (!got_nro) {
        g_err[0] = 0;
        redraw("NRO asset missed; downloading full zip ...");
        if (ensure_zip(pad) != 0) {
            appletSetAutoSleepDisabled(false);
            return -1;
        }
        if (extract_self_from_zip() != 0) {
            appletSetAutoSleepDisabled(false);
            redraw("Could not extract PackUpdater.nro.");
            return -1;
        }
    }

    appletSetAutoSleepDisabled(false);
    g_err[0] = 0;
    redraw("PackUpdater written. Press + then reopen, then A for the pack.");
    return 0;
}

static int do_update(PadState *pad) {
    if (!g_have_asset) {
        snprintf(g_err, sizeof g_err, "check GitHub first (press Y)");
        redraw("No release info.");
        return -1;
    }
    int64_t min_bytes = (int64_t)g_cfg.min_zip_mb * 1024 * 1024;
    if (g_asset.size > 0 && g_asset.size < min_bytes) {
        snprintf(g_err, sizeof g_err, "remote zip too small (%lld bytes)", (long long)g_asset.size);
        redraw("Refusing tiny zip (broken release).");
        return -1;
    }
    if (battery_ok_to_extract(g_err, sizeof g_err) != 0) {
        redraw("Charge the Switch first.");
        return -1;
    }
    if (sd_ok_to_extract(g_asset.size, g_err, sizeof g_err) != 0) {
        redraw("Not enough SD free space.");
        return -1;
    }

    appletSetAutoSleepDisabled(true);
    mkdir(WORK_DIR, 0777);

    if (ensure_zip(pad) != 0) {
        appletSetAutoSleepDisabled(false);
        return -1;
    }

    Pump p = {.pad = pad, .label = "Extracting", .abort = 0};
    redraw("Extracting onto sdmc:/ ...");
    if (pack_unzip(ZIP_PATH, g_cfg.extract_to, unzip_progress, &p,
                   g_self_nro[0] ? g_self_nro : NULL, g_err, sizeof g_err) != 0) {
        appletSetAutoSleepDisabled(false);
        redraw("Extract failed.");
        return -1;
    }

    save_installed(g_asset.tag);
    snprintf(g_installed, sizeof g_installed, "%s", g_asset.tag);
    remove(ZIP_PATH);
    appletSetAutoSleepDisabled(false);
    g_err[0] = 0;
    if (access(PKG3_AIO, F_OK) == 0 || access(STRAT_AIO, F_OK) == 0) {
        write_startup_te();
        redraw("Done. Press X to reboot and apply package3.");
    } else {
        redraw("Done. Reboot recommended (press X).");
    }
    return 0;
}

static int aio_pending(void) {
    return access(PKG3_AIO, F_OK) == 0 || access(STRAT_AIO, F_OK) == 0;
}

static void ensure_te_on_sd(void) {
    struct stat st;
    if (stat(TE_PATH, &st) == 0 && st.st_size >= 0x1000)
        return;
    FILE *f = fopen(TE_PATH, "wb");
    if (!f)
        return;
    fwrite(TegraExplorer_bin, 1, TegraExplorer_bin_size, f);
    fclose(f);
}

static void write_startup_te(void) {
    FILE *fp = fopen(STARTUP_TE, "w");
    if (!fp) return;
    fputs(
        "#REQUIRE SD\n"
        "#REQUIRE VER 4.0.0\n"
        "#REQUIRE MINERVA\n"
        "if (fsexists(\"sd:/atmosphere/package3.aio\")) {\n"
        "  if (copyfile(\"sd:/atmosphere/package3.aio\", \"sd:/atmosphere/package3\")) {\n"
        "    println(\"package3 apply failed\")\n"
        "  } .else() {\n"
        "    delfile(\"sd:/atmosphere/package3.aio\")\n"
        "  }\n"
        "}\n"
        "if (fsexists(\"sd:/atmosphere/stratosphere.romfs.aio\")) {\n"
        "  if (copyfile(\"sd:/atmosphere/stratosphere.romfs.aio\", \"sd:/atmosphere/stratosphere.romfs\")) {\n"
        "    println(\"stratosphere apply failed\")\n"
        "  } .else() {\n"
        "    delfile(\"sd:/atmosphere/stratosphere.romfs.aio\")\n"
        "  }\n"
        "}\n"
        "delfile(\"sd:/startup.te\")\n"
        "if (fsexists(\"sd:/bootloader/update.bin\")) {\n"
        "  payload(\"sd:/bootloader/update.bin\")\n"
        "} .else() {\n"
        "  if (fsexists(\"sd:/atmosphere/reboot_payload.bin\")) {\n"
        "    payload(\"sd:/atmosphere/reboot_payload.bin\")\n"
        "  }\n"
        "}\n",
        fp);
    fclose(fp);
}

static int reboot_via_te(void) {
    FILE *f = fopen(TE_PATH, "rb");
    if (!f) {
        snprintf(g_err, sizeof g_err, "missing %s", TE_PATH);
        return -1;
    }
    static u8 payload[IRAM_PAYLOAD_MAX_SIZE] __attribute__((aligned(0x1000)));
    memset(payload, 0, sizeof payload);
    size_t n = fread(payload, 1, sizeof payload, f);
    fclose(f);
    if (n < 0x1000) {
        snprintf(g_err, sizeof g_err, "TegraExplorer.bin too small");
        return -1;
    }
    size_t send = n;
    if (send < 0x24000) send = 0x24000;
    if (send > sizeof payload) send = sizeof payload;
    if (R_FAILED(spsmInitialize())) return -1;
    Result rc = amsBpcInitialize();
    if (R_SUCCEEDED(rc))
        rc = amsBpcSetRebootPayload(payload, send);
    if (R_SUCCEEDED(rc)) {
        spsmShutdown(true);
        return 0;
    }
    snprintf(g_err, sizeof g_err, "amsBpc reboot failed %x", rc);
    return -1;
}

static void do_reboot(void) {
    if (aio_pending()) {
        write_startup_te();
        if (reboot_via_te() == 0)
            return;
        redraw("Payload reboot failed; trying normal reboot.");
    }
    if (R_SUCCEEDED(spsmInitialize())) {
        spsmShutdown(true);
    }
}

int main(int argc, char **argv) {
    if (argc > 0 && argv[0] && argv[0][0])
        snprintf(g_self_nro, sizeof g_self_nro, "%s", argv[0]);

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    mkdir("sdmc:/switch", 0777);
    mkdir(WORK_DIR, 0777);
    ensure_te_on_sd();
    load_config();
    load_installed();

    Result sock = socketInitializeDefault();
    if (R_FAILED(sock)) {
        snprintf(g_err, sizeof g_err, "socketInit failed %x", sock);
    }
    http_global_init();

    if (R_SUCCEEDED(sock)) fetch_latest(&pad);
    else redraw("No network.");

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 k = padGetButtonsDown(&pad);
        if (k & HidNpadButton_Plus) break;
        if (k & HidNpadButton_Y) fetch_latest(&pad);
        if (k & HidNpadButton_A) do_update(&pad);
        if (k & HidNpadButton_B) do_self_update(&pad);
        if (k & HidNpadButton_X) do_reboot();
        consoleUpdate(NULL);
    }

    http_global_cleanup();
    socketExit();
    consoleExit(NULL);
    return 0;
}
