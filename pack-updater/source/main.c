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
#include "ui.h"
#include "TegraExplorer_bin.h"

#define VERSION "1.6.1"
#define EXTRACT_DONE_MSG "解压完成。请按 X 重启，然后在 payload 中选择 Tegra Explorer。"
#define COL_BG    ui_rgba(16, 18, 24, 255)
#define COL_TITLE ui_rgba(232, 197, 71, 255)
#define COL_TEXT  ui_rgba(236, 236, 240, 255)
#define COL_DIM   ui_rgba(160, 168, 180, 255)
#define COL_OK    ui_rgba(110, 220, 140, 255)
#define COL_WARN  ui_rgba(240, 200, 80, 255)
#define COL_ERR   ui_rgba(255, 110, 110, 255)
#define COL_HINT  ui_rgba(120, 200, 255, 255)
#define UI_X 56
#define UI_SZ 20.0f
#define UI_TITLE_SZ 24.0f
#define UI_LH 26
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
static char g_status[160];
static char g_prog1[96];
static char g_prog2[96];

static void write_startup_te(void);
static void ensure_te_on_sd(void);
static void redraw(const char *status);

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
    snprintf(g_prog1, sizeof g_prog1, "%s", p->label);
    snprintf(g_prog2, sizeof g_prog2, "%s", b);
    redraw(NULL);
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
    snprintf(g_prog1, sizeof g_prog1, "正在解压 %d / %d", i + 1, n);
    snprintf(g_prog2, sizeof g_prog2, "%.70s", name ? name : "");
    redraw(NULL);
    return 0;
}

static void draw_extract_done_status(int y)
{
    int x = UI_X;
    x = ui_text(x, y, UI_SZ, COL_WARN, "解压完成。请按 ");
    x = ui_text(x, y, UI_SZ, COL_ERR, "X");
    x = ui_text(x, y, UI_SZ, COL_WARN, " 重启，然后在 ");
    x = ui_text(x, y, UI_SZ, COL_ERR, "payload");
    x = ui_text(x, y, UI_SZ, COL_WARN, " 中选择 ");
    x = ui_text(x, y, UI_SZ, COL_ERR, "Tegra Explorer");
    ui_text(x, y, UI_SZ, COL_WARN, "。");
}

static void banner(const char *status) {
    int y = 28;
    ui_text(UI_X, y, UI_TITLE_SZ, COL_TITLE, "整合包更新  PackUpdater " VERSION);
    y += 40;
    ui_text(UI_X, y, UI_SZ, COL_DIM, "================================");
    y += UI_LH;
    char line[384];
    snprintf(line, sizeof line, "仓库     : %s", g_cfg.repo);
    ui_text(UI_X, y, UI_SZ, COL_TEXT, line); y += UI_LH;
    snprintf(line, sizeof line, "加速     : %s", g_cfg.gh_proxy[0] ? g_cfg.gh_proxy : "(直连)");
    ui_text(UI_X, y, UI_SZ, COL_TEXT, line); y += UI_LH;
    snprintf(line, sizeof line, "代理     : %s", g_cfg.proxy[0] ? g_cfg.proxy : "(无)");
    ui_text(UI_X, y, UI_SZ, COL_TEXT, line); y += UI_LH;
    snprintf(line, sizeof line, "已安装   : %s", g_installed[0] ? g_installed : "(未知)");
    ui_text(UI_X, y, UI_SZ, COL_TEXT, line); y += UI_LH;
    if (g_have_asset) {
        snprintf(line, sizeof line, "最新     : %s", g_asset.tag);
        ui_text(UI_X, y, UI_SZ, COL_TEXT, line); y += UI_LH;
        snprintf(line, sizeof line, "文件     : %s", g_asset.name);
        ui_text(UI_X, y, UI_SZ, COL_TEXT, line); y += UI_LH;
        snprintf(line, sizeof line, "大小     : %lld MB", (long long)(g_asset.size / (1024 * 1024)));
        ui_text(UI_X, y, UI_SZ, COL_TEXT, line); y += UI_LH;
    } else {
        ui_text(UI_X, y, UI_SZ, COL_DIM, "最新     : (未获取)"); y += UI_LH;
        ui_text(UI_X, y, UI_SZ, COL_DIM, "文件     : -"); y += UI_LH;
        ui_text(UI_X, y, UI_SZ, COL_DIM, "大小     : -"); y += UI_LH;
    }
    ui_text(UI_X, y, UI_SZ, COL_DIM, "--------------------------------"); y += UI_LH;
    if (status && status[0]) {
        if (strcmp(status, EXTRACT_DONE_MSG) == 0)
            draw_extract_done_status(y);
        else
            ui_text(UI_X, y, UI_SZ, COL_WARN, status);
        y += UI_LH;
    }
    if (g_err[0]) {
        snprintf(line, sizeof line, "错误: %s", g_err);
        ui_text(UI_X, y, UI_SZ, COL_ERR, line); y += UI_LH;
    }
    if (g_prog1[0]) {
        ui_text(UI_X, y, UI_SZ, COL_OK, g_prog1); y += UI_LH;
    }
    if (g_prog2[0]) {
        ui_text(UI_X, y, UI_SZ, COL_TEXT, g_prog2); y += UI_LH;
    }
    ui_text(UI_X, y, UI_SZ, COL_DIM, "--------------------------------"); y += UI_LH;
    ui_text(UI_X, y, UI_SZ, COL_HINT, "[A] 下载并解压到内存卡"); y += UI_LH;
    ui_text(UI_X, y, UI_SZ, COL_HINT, "[B] 先更新本程序（可选）"); y += UI_LH;
    ui_text(UI_X, y, UI_SZ, COL_HINT, "[Y] 重新检查更新"); y += UI_LH;
    ui_text(UI_X, y, UI_SZ, COL_HINT, "[X] 重启"); y += UI_LH;
    ui_text(UI_X, y, UI_SZ, COL_HINT, "[+] 退出"); y += UI_LH + 8;
    ui_text(UI_X, y, UI_SZ, COL_DIM, "会覆盖 atmosphere / bootloader / switch，"); y += UI_LH;
    ui_text(UI_X, y, UI_SZ, COL_DIM, "不会改 Nintendo/ 和游戏。"); y += UI_LH;
    ui_text(UI_X, y, UI_SZ, COL_DIM, "package3 在重启后由 TegraExplorer 写入。"); y += UI_LH;
    ui_text(UI_X, y, UI_SZ, COL_DIM, "按 A 一次完成更新。B 可跳过。");
}

static void redraw(const char *status) {
    if (status) {
        snprintf(g_status, sizeof g_status, "%s", status);
        g_prog1[0] = 0;
        g_prog2[0] = 0;
    }
    ui_begin(COL_BG);
    banner(g_status);
    ui_end();
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
    Pump p = {.pad = pad, .label = "正在连接 GitHub", .abort = 0};
    const char *tok = g_cfg.gh_proxy[0] ? NULL : g_cfg.token;

    if (g_cfg.gh_proxy[0]) {
        char src[256], url[768], final[768];
        snprintf(src, sizeof src, "https://github.com/%s/releases/latest", g_cfg.repo);
        via_gh(url, sizeof url, src);
        redraw("正在检查加速节点...");
        if (http_follow(url, g_cfg.proxy, tok, pump_cb, &p, final, sizeof final, NULL,
                        g_err, sizeof g_err) != 0) {
            redraw("检查失败。");
            return -1;
        }
        if (gh_tag_from_effective_url(final, g_asset.tag, sizeof g_asset.tag) != 0) {
            snprintf(g_err, sizeof g_err, "地址里没有 /releases/tag/ : %s", final);
            redraw("解析失败。");
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
        redraw("正在检查 GitHub API...");
        HttpMem mem = {0};
        if (http_get_mem(url, g_cfg.proxy, tok, pump_cb, &p, &mem, g_err, sizeof g_err) != 0) {
            redraw("检查失败。");
            return -1;
        }
        if (gh_parse_latest_zip(mem.data, g_cfg.asset_prefix, &g_asset, g_err, sizeof g_err) != 0) {
            free(mem.data);
            redraw("解析失败。");
            return -1;
        }
        free(mem.data);
        g_have_asset = 1;
    }

    if (g_installed[0] && strcmp(g_installed, g_asset.tag) == 0)
        redraw("已是最新。按 A 仍会重新安装。");
    else
        redraw("有可用更新。");
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
        snprintf(err, err_sz, "电量 %u%% 且未充电（需要 >= %d%% 或插电）", pct, need);
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
        snprintf(err, err_sz, "内存卡剩余 %lld MB，需要 %lld MB",
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
        snprintf(err, err_sz, "无法写入 %s", to);
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
        redraw("使用已下载的压缩包。");
        return 0;
    }
    remove(ZIP_PATH);
    Pump p = {.pad = pad, .label = "正在下载整合包", .abort = 0};
    redraw("正在下载...");
    if (http_get_file(g_asset.url, ZIP_PATH, g_cfg.proxy,
                      g_cfg.gh_proxy[0] ? NULL : g_cfg.token,
                      pump_cb, &p, g_err, sizeof g_err) != 0) {
        redraw("下载失败。");
        return -1;
    }
    if (p.abort) {
        remove(ZIP_PATH);
        redraw("已取消。");
        return -1;
    }
    int64_t sz = file_size(ZIP_PATH);
    if (sz < min_bytes) {
        snprintf(g_err, sizeof g_err, "下载了 %lld 字节，需要 >= %d MB",
                 (long long)sz, g_cfg.min_zip_mb);
        remove(ZIP_PATH);
        redraw("压缩包过小，已拒绝。");
        return -1;
    }
    if (!pack_zip_has_file(ZIP_PATH, REQUIRED_INNER)) {
        snprintf(g_err, sizeof g_err, "压缩包缺少 %s", REQUIRED_INNER);
        remove(ZIP_PATH);
        redraw("不是有效的整合包。");
        return -1;
    }
    return 0;
}

static int do_self_update(PadState *pad) {
    if (!g_have_asset) {
        snprintf(g_err, sizeof g_err, "请先按 Y 检查 GitHub");
        redraw("没有版本信息。");
        return -1;
    }
    if (battery_ok_to_extract(g_err, sizeof g_err) != 0) {
        redraw("请先充电。");
        return -1;
    }
    if (sd_has_free(16LL * 1024 * 1024, g_err, sizeof g_err) != 0) {
        redraw("内存卡空间不足。");
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

    Pump p = {.pad = pad, .label = "正在下载 PackUpdater.nro", .abort = 0};
    redraw("正在下载 PackUpdater.nro ...");
    int got_nro = 0;
    if (http_get_file(url, tmp, g_cfg.proxy,
                      g_cfg.gh_proxy[0] ? NULL : g_cfg.token,
                      pump_cb, &p, g_err, sizeof g_err) == 0 && !p.abort) {
        int64_t sz = file_size(tmp);
        if (nro_size_ok(sz)) {
            if (replace_file(tmp, self_nro_path(), g_err, sizeof g_err) == 0)
                got_nro = 1;
        } else {
            snprintf(g_err, sizeof g_err, "nro 大小 %lld 不合理", (long long)sz);
        }
    }
    remove(tmp);

    if (!got_nro) {
        g_err[0] = 0;
        redraw("独立 NRO 失败，改下完整包...");
        if (ensure_zip(pad) != 0) {
            appletSetAutoSleepDisabled(false);
            return -1;
        }
        if (extract_self_from_zip() != 0) {
            appletSetAutoSleepDisabled(false);
            redraw("无法解出 PackUpdater.nro。");
            return -1;
        }
    }

    appletSetAutoSleepDisabled(false);
    g_err[0] = 0;
    redraw("本程序已写入。请按 + 退出后重新打开，再按 A 更新整合包。");
    return 0;
}

static int do_update(PadState *pad) {
    if (!g_have_asset) {
        snprintf(g_err, sizeof g_err, "请先按 Y 检查 GitHub");
        redraw("没有版本信息。");
        return -1;
    }
    int64_t min_bytes = (int64_t)g_cfg.min_zip_mb * 1024 * 1024;
    if (g_asset.size > 0 && g_asset.size < min_bytes) {
        snprintf(g_err, sizeof g_err, "远程压缩包过小（%lld 字节）", (long long)g_asset.size);
        redraw("压缩包过小（发布损坏）。");
        return -1;
    }
    if (battery_ok_to_extract(g_err, sizeof g_err) != 0) {
        redraw("请先充电。");
        return -1;
    }
    if (sd_ok_to_extract(g_asset.size, g_err, sizeof g_err) != 0) {
        redraw("内存卡空间不足。");
        return -1;
    }

    appletSetAutoSleepDisabled(true);
    mkdir(WORK_DIR, 0777);

    if (ensure_zip(pad) != 0) {
        appletSetAutoSleepDisabled(false);
        return -1;
    }

    Pump p = {.pad = pad, .label = "正在解压", .abort = 0};
    redraw("正在解压到内存卡...");
    if (pack_unzip(ZIP_PATH, g_cfg.extract_to, unzip_progress, &p,
                   g_self_nro[0] ? g_self_nro : NULL, g_err, sizeof g_err) != 0) {
        appletSetAutoSleepDisabled(false);
        redraw("解压失败。");
        return -1;
    }

    save_installed(g_asset.tag);
    snprintf(g_installed, sizeof g_installed, "%s", g_asset.tag);
    remove(ZIP_PATH);
    appletSetAutoSleepDisabled(false);
    g_err[0] = 0;
    if (access(PKG3_AIO, F_OK) == 0 || access(STRAT_AIO, F_OK) == 0)
        write_startup_te();
    redraw(EXTRACT_DONE_MSG);
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
        snprintf(g_err, sizeof g_err, "缺少 %s", TE_PATH);
        return -1;
    }
    static u8 payload[IRAM_PAYLOAD_MAX_SIZE] __attribute__((aligned(0x1000)));
    memset(payload, 0, sizeof payload);
    size_t n = fread(payload, 1, sizeof payload, f);
    fclose(f);
    if (n < 0x1000) {
        snprintf(g_err, sizeof g_err, "TegraExplorer.bin 太小");
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
    snprintf(g_err, sizeof g_err, "amsBpc 重启失败 %x", rc);
    return -1;
}

static void do_reboot(void) {
    if (aio_pending()) {
        write_startup_te();
        if (reboot_via_te() == 0)
            return;
        redraw("载荷重启失败，尝试普通重启。");
    }
    if (R_SUCCEEDED(spsmInitialize())) {
        spsmShutdown(true);
    }
}

int main(int argc, char **argv) {
    if (argc > 0 && argv[0] && argv[0][0])
        snprintf(g_self_nro, sizeof g_self_nro, "%s", argv[0]);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    if (ui_init() != 0) {
        consoleInit(NULL);
        printf("UI init failed\nPress + to quit\n");
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
        return 1;
    }

    mkdir("sdmc:/switch", 0777);
    mkdir(WORK_DIR, 0777);
    ensure_te_on_sd();
    load_config();
    load_installed();

    Result sock = socketInitializeDefault();
    if (R_FAILED(sock)) {
        snprintf(g_err, sizeof g_err, "网络初始化失败 %x", sock);
    }
    http_global_init();

    if (R_SUCCEEDED(sock)) fetch_latest(&pad);
    else redraw("无网络。");

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 k = padGetButtonsDown(&pad);
        if (k & HidNpadButton_Plus) break;
        if (k & HidNpadButton_Y) fetch_latest(&pad);
        if (k & HidNpadButton_A) do_update(&pad);
        if (k & HidNpadButton_B) do_self_update(&pad);
        if (k & HidNpadButton_X) do_reboot();
        ui_idle();
    }

    http_global_cleanup();
    socketExit();
    ui_exit();
    return 0;
}
