#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "http.h"
#include "json.h"
#include "unzip.h"

#define VERSION "1.0.0"
#define WORK_DIR "sdmc:/switch/PackUpdater"
#define ZIP_PATH WORK_DIR "/update.zip"
#define INSTALLED_PATH WORK_DIR "/installed.txt"
#define CONFIG_PATH WORK_DIR "/config.ini"
#define DEFAULT_REPO "bao3/SwitchScript"
#define DEFAULT_PREFIX "NS-SD-Card-Atmosphere-"
#define DEFAULT_EXTRACT "sdmc:/"
#define REQUIRED_INNER "atmosphere/package3"

typedef struct {
    char repo[128];
    char asset_prefix[128];
    char proxy[256];
    char token[128];
    char extract_to[64];
    int min_zip_mb;
} Config;

typedef struct {
    PadState *pad;
    const char *label;
    int abort;
} Pump;

static Config g_cfg;
static char g_err[256];
static char g_installed[64];
static GhAsset g_asset;
static int g_have_asset;

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
    c->min_zip_mb = 10;
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
        else if (!strcmp(k, "token")) snprintf(g_cfg.token, sizeof g_cfg.token, "%s", v);
        else if (!strcmp(k, "extract_to")) snprintf(g_cfg.extract_to, sizeof g_cfg.extract_to, "%s", v);
        else if (!strcmp(k, "min_zip_mb")) g_cfg.min_zip_mb = atoi(v);
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
    printf("[Y] recheck GitHub\n");
    printf("[X] reboot\n");
    printf("[+] quit\n");
    printf("\nThis overlays atmosphere/bootloader/switch from\n");
    printf("the pack. Nintendo/ and games are not touched.\n");
}

static void redraw(const char *status) {
    consoleClear();
    banner(status);
    consoleUpdate(NULL);
}

static int fetch_latest(PadState *pad) {
    g_have_asset = 0;
    g_err[0] = 0;
    char url[256];
    snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", g_cfg.repo);
    redraw("Checking GitHub...");
    Pump p = {.pad = pad, .label = "API", .abort = 0};
    HttpMem mem = {0};
    if (http_get_mem(url, g_cfg.proxy, g_cfg.token, pump_cb, &p, &mem, g_err, sizeof g_err) != 0) {
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

    appletSetAutoSleepDisabled(true);
    mkdir(WORK_DIR, 0777);
    remove(ZIP_PATH);

    Pump p = {.pad = pad, .label = "Downloading pack", .abort = 0};
    redraw("Downloading...");
    if (http_get_file(g_asset.url, ZIP_PATH, g_cfg.proxy, g_cfg.token,
                      pump_cb, &p, g_err, sizeof g_err) != 0) {
        appletSetAutoSleepDisabled(false);
        redraw("Download failed.");
        return -1;
    }
    if (p.abort) {
        appletSetAutoSleepDisabled(false);
        remove(ZIP_PATH);
        redraw("Aborted.");
        return -1;
    }

    int64_t sz = file_size(ZIP_PATH);
    if (sz < min_bytes) {
        snprintf(g_err, sizeof g_err, "downloaded %lld bytes, need >= %d MB",
                 (long long)sz, g_cfg.min_zip_mb);
        remove(ZIP_PATH);
        appletSetAutoSleepDisabled(false);
        redraw("Refusing tiny zip.");
        return -1;
    }
    if (!pack_zip_has_file(ZIP_PATH, REQUIRED_INNER)) {
        snprintf(g_err, sizeof g_err, "zip missing %s", REQUIRED_INNER);
        remove(ZIP_PATH);
        appletSetAutoSleepDisabled(false);
        redraw("Not a valid SD pack.");
        return -1;
    }

    p.label = "Extracting";
    redraw("Extracting onto sdmc:/ ...");
    if (pack_unzip(ZIP_PATH, g_cfg.extract_to, unzip_progress, &p, g_err, sizeof g_err) != 0) {
        appletSetAutoSleepDisabled(false);
        redraw("Extract failed.");
        return -1;
    }

    save_installed(g_asset.tag);
    snprintf(g_installed, sizeof g_installed, "%s", g_asset.tag);
    remove(ZIP_PATH);
    appletSetAutoSleepDisabled(false);
    g_err[0] = 0;
    redraw("Done. Reboot recommended (press X).");
    return 0;
}

static void do_reboot(void) {
    if (R_SUCCEEDED(spsmInitialize())) {
        spsmShutdown(true);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    mkdir("sdmc:/switch", 0777);
    mkdir(WORK_DIR, 0777);
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
        if (k & HidNpadButton_X) do_reboot();
        consoleUpdate(NULL);
    }

    http_global_cleanup();
    socketExit();
    consoleExit(NULL);
    return 0;
}
