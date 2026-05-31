#!/bin/bash

### Credit to the Authors at https://rentry.org/CFWGuides
### Script created by Fraxalotl
### Modified for unified API download, path fixes, custom output directory, .nro/.ovl support, precise zip filtering, and aio-switch-updater integration

# -------------------------------------------
# 定义基础 Release URL 变量
# -------------------------------------------
HEKATE_URL="https://github.com/CTCaer/hekate/releases/latest"
ATMOSPHERE_URL="https://github.com/Atmosphere-NX/Atmosphere/releases/latest"
SIGPATCHES_URL="https://github.com/impeeza/sys-patch/releases/latest"
AKIRA_URL="https://github.com/xlanor/akira/releases/latest"
TESLA_MENU_URL="https://github.com/WerWolv/Tesla-Menu/releases/latest"
OVLLOADER_URL="https://github.com/WerWolv/nx-ovlloader/releases/latest"
MISSION_CONTROL_URL="https://github.com/ndeadly/MissionControl/releases/latest"
DBI_URL="https://github.com/rashevskyv/dbi/releases/latest"
SPHAIRA_URL="https://github.com/ITTotalJustice/sphaira/releases/latest"
EDIZON_SE_URL="https://github.com/tomvita/EdiZon-SE/releases/latest"
AIO_UPDATER_URL="https://github.com/HamletDuFromage/aio-switch-updater/releases/latest"

# MigFlash 官网下载页面 URL
MIG_DUMP_PAGE_URL="https://migflash.com/downloads/"

# 定义统一的输出目标目录（末尾不加斜杠）
OUTPUT_DIR="./NS SD Card"

# -------------------------------------------
### Install jq if not already installed
if [[ "$OSTYPE" == "msys" ]]; then
  # Windows
  @"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -InputFormat None -ExecutionPolicy Bypass -Command "[System.Net.ServicePointManager]::SecurityProtocol = 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))" && SET "PATH=%PATH%;%ALLUSERSPROFILE%\chocolatey\bin"
  chocolatey install jq
elif [[ "$OSTYPE" == "darwin" ]]; then
  # MacOS
  brew install jq
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
  # linux
  sudo apt-get install jq
fi;

# 确保输出目标目录以及所需的所有子架构目录存在
mkdir -p "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/switch"
mkdir -p "$OUTPUT_DIR/switch/DBI"
mkdir -p "$OUTPUT_DIR/switch/MigDumpTool"
mkdir -p "$OUTPUT_DIR/switch/.overlays"
mkdir -p "$OUTPUT_DIR/config/sys-patch"
mkdir -p "$OUTPUT_DIR/config/ftpsrv"
mkdir -p "$OUTPUT_DIR/atmosphere"

# -------------------------------------------
### 统一的 API 下载函数（包含精准过滤与睡眠抗限机制）
# -------------------------------------------
download_latest_asset() {
  local repo_url=$1
  local output_path=$2
  local extension=${3:-".zip"} # 第三参数默认为 .zip
  
  echo "Processing $(basename "$output_path")...."
  
  # 将标准 GitHub URL 转换为 API URL
  local api_url=$(echo "$repo_url" | sed 's|github.com|api.github.com/repos|')
  
  # 核心抓取逻辑变量
  local download_url=""

  # 针对特定仓库订制硬性筛选规则，精准匹配目标资产
  if [[ "$repo_url" == *"/Tesla-Menu/"* ]]; then
    # Tesla Menu 明确抓取 ovlmenu.zip
    download_url=$(curl -sL "$api_url" | jq -r '.assets[] | select(.name == "ovlmenu.zip") | .browser_download_url' | head -n 1)
  elif [[ "$repo_url" == *"/sphaira/"* ]]; then
    # Sphaira 完全精准抓取不带版本号的 sphaira.zip
    download_url=$(curl -sL "$api_url" | jq -r '.assets[] | select(.name == "sphaira.zip") | .browser_download_url' | head -n 1)
  else
    # 通用筛选逻辑
    download_url=$(curl -sL "$api_url" | jq -r --arg ext "$extension" 'try (.assets[] | select(.name | ascii_downcase | endswith($ext | ascii_downcase)) | .browser_download_url) catch null' | head -n 1)
  fi
  
  if [[ -z "$download_url" || "$download_url" == "null" ]]; then
    echo "Error: Failed to fetch download URL for $output_path"
    echo "Waiting 5 seconds before continuing..."
    sleep 5
    return 1
  fi
  
  echo "Downloading from: $download_url"
  curl -sL "$download_url" -o "$output_path"
  echo "$(basename "$output_path") downloaded successfully."
  
  # 每次请求完成后休眠 5 秒，防止触发 GitHub API 的 Rate Limit
  echo "Sleeping 5 seconds to prevent GitHub API rate limiting..."
  sleep 5
}

# --- 执行下载核心组件（下载到当前目录准备解压） ---
download_latest_asset "$HEKATE_URL" "hekate.zip"
download_latest_asset "$ATMOSPHERE_URL" "atmosphere.zip"
download_latest_asset "$SIGPATCHES_URL" "sigpatches.zip"
download_latest_asset "$OVLLOADER_URL" "nx-ovlloader.zip"
download_latest_asset "$MISSION_CONTROL_URL" "missioncontrol.zip"
download_latest_asset "$EDIZON_SE_URL" "edizon-se.zip"
download_latest_asset "$TESLA_MENU_URL" "ovlmenu.zip"       # 下载为 ovlmenu.zip
download_latest_asset "$SPHAIRA_URL" "sphaira.zip"         # 下载为 sphaira.zip
download_latest_asset "$AIO_UPDATER_URL" "aio-switch-updater.zip" # 新增：下载为 aio-switch-updater.zip

# --- 执行下载独立单文件组件（直接过滤对应的后缀并存入目标目录） ---
# 下载 Akira (.nro)
download_latest_asset "$AKIRA_URL" "$OUTPUT_DIR/switch/akira.nro" ".nro"
# 下载 Tesla-Menu (.ovl) 作为双重保险
download_latest_asset "$TESLA_MENU_URL" "$OUTPUT_DIR/switch/.overlays/tesla.ovl" ".ovl"
# 下载 DBI (.nro)
download_latest_asset "$DBI_URL" "$OUTPUT_DIR/switch/DBI/DBI.nro" ".nro"

# 5. 动态解析并下载最新的 MigDumpTool (.nro)
echo "Processing MigDumpTool.nro..."
MIG_DUMP_REAL_URL=$(curl -sL --connect-timeout 5 "$MIG_DUMP_PAGE_URL" | grep -o 'https://migflash.com/downloads/MigDumpTool-[^"]*\.nro' | head -n 1)

# 防御性逻辑：如果动态抓取失败，采用固定 URL 作为保底方案
if [[ -z "$MIG_DUMP_REAL_URL" ]]; then
  echo "Warning: Failed to parse latest URL. Using fallback static URL..."
  MIG_DUMP_REAL_URL="https://migflash.com/downloads/MigDumpTool-v0.0.2.nro"
fi

echo "Downloading MigDumpTool from: $MIG_DUMP_REAL_URL"
curl -sLf "$MIG_DUMP_REAL_URL" -o "$OUTPUT_DIR/switch/MigDumpTool/MigDumpTool.nro"

if [[ $? -eq 0 ]]; then
  echo "MigDumpTool.nro downloaded successfully."
else
  echo "Error: Failed to download MigDumpTool. Please check your network connection."
fi

# -------------------------------------------
### Unzip Downloaded Packages to Target Directory

echo "Unzipping Zips into $OUTPUT_DIR..."
# 提前清理目标目录中可能影响解压的旧目录，确保全新覆盖更新
rm -rf "$OUTPUT_DIR/bootloader" "$OUTPUT_DIR/atmosphere"

# 使用 -d 参数指定解压到目标文件夹
unzip -u hekate.zip -d "$OUTPUT_DIR"
unzip -u atmosphere.zip -d "$OUTPUT_DIR"
unzip -u sigpatches.zip -d "$OUTPUT_DIR"
unzip -u nx-ovlloader.zip -d "$OUTPUT_DIR"
unzip -u missioncontrol.zip -d "$OUTPUT_DIR"
unzip -u edizon-se.zip -d "$OUTPUT_DIR"
unzip -u ovlmenu.zip -d "$OUTPUT_DIR"
unzip -u sphaira.zip -d "$OUTPUT_DIR"
unzip -u aio-switch-updater.zip -d "$OUTPUT_DIR"  # 新增：解压 aio-switch-updater
echo "Done!"

### Cleanup Downloaded Zips

echo "Cleaning up zip files..."
rm -f hekate.zip
rm -f atmosphere.zip
rm -f sigpatches.zip
rm -f nx-ovlloader.zip
rm -f missioncontrol.zip
rm -f edizon-se.zip
rm -f ovlmenu.zip
rm -f sphaira.zip
rm -f aio-switch-updater.zip  # 新增：清理临时压缩包
echo "Done!"

# -------------------------------------------
### 移动 fusee.bin 到目标目录下的 payloads
# -------------------------------------------
mkdir -p "$OUTPUT_DIR/bootloader/payloads"

# 如果 fusee.bin 被解压到了当前脚本根目录，将其移动到目标位置
if [[ -f "fusee.bin" ]]; then
  if [[ "$OSTYPE" == "msys" ]]; then
    move fusee.bin "$OUTPUT_DIR/bootloader/payloads/"
  else
    mv fusee.bin "$OUTPUT_DIR/bootloader/payloads/"
  fi
  echo "fusee.bin moved to $OUTPUT_DIR/bootloader/payloads/"
# 防御性代码
elif [[ -f "$OUTPUT_DIR/fusee.bin" ]]; then
  mv "$OUTPUT_DIR/fusee.bin" "$OUTPUT_DIR/bootloader/payloads/"
  echo "fusee.bin relocated from output root to payloads."
else
  echo "Warning: fusee.bin not found."
fi

# -------------------------------------------
### 写入配置文件 到目标目录下
# -------------------------------------------

### Write hekate_ipl.ini
echo "Writing hekate_ipl.ini..."
mkdir -p "$OUTPUT_DIR/bootloader"
cat > "$OUTPUT_DIR/bootloader/hekate_ipl.ini" << ENDOFFILE
[config]
autoboot=0
autoboot_list=0
bootwait=3
backlight=100
autohosoff=0
autonogc=1
updater2p=0
bootprotect=0

[Atmosphere CFW]
payload=bootloader/payloads/fusee.bin
icon=bootloader/res/icon_payload.bmp

[Stock SysNAND]
fss0=atmosphere/package3
stock=1
emummc_force_disable=1
icon=bootloader/res/icon_switch.bmp
ENDOFFILE
echo "Done!"

# -------------------------------------------

### write exosphere.ini
echo "Writing exosphere.ini..."
cat > "$OUTPUT_DIR/exosphere.ini" << ENDOFFILE
[exosphere]
debugmode=1
debugmode_user=0
disable_user_exception_handlers=0
enable_user_pmu_access=0
blank_prodinfo_sysmmc=0
blank_prodinfo_emummc=1
allow_writing_to_cal_sysmmc=0
log_port=0
log_baud_rate=115200
log_inverted=0
ENDOFFILE
echo "Done!"

# -------------------------------------------

### Write default.txt
echo "Writing default.txt..."
mkdir -p "$OUTPUT_DIR/atmosphere/hosts"
cat > "$OUTPUT_DIR/atmosphere/hosts/default.txt" << ENDOFFILE
# Block Nintendo Servers
127.0.0.1 *nintendo.*
127.0.0.1 *nintendo-europe.com
127.0.0.1 *nintendoswitch.*
95.216.149.205 *conntest.nintendowifi.net
95.216.149.205 *ctest.cdn.nintendo.net
ENDOFFILE
echo "Done!"

# -------------------------------------------

### Write sys-patch config.ini
echo "Writing sys-patch config.ini..."
cat > "$OUTPUT_DIR/config/sys-patch/config.ini" << ENDOFFILE
[options]
patch_sysmmc=0   ; 1=(default) patch sysmmc, 0=don't patch sysmmc
patch_emummc=1   ; 1=(default) patch emummc, 0=don't patch emummc
enable_logging=1 ; 1=(default) output /config/sys-patch/log.ini 0=no log
version_skip=1   ; 1=(default) skips out of date patterns, 0=search all patterns
ENDOFFILE
echo "Done!"

# -------------------------------------------

### Write sphaira / ftpsrv config.ini
echo "Writing ftpsrv config.ini..."
cat > "$OUTPUT_DIR/config/ftpsrv/config.ini" << ENDOFFILE
##########
# sphaira and ftpsrv#
##########

#######################################################################
# Rename config.ini.template to config.ini for changes to take effect.#
#######################################################################

[Login]
# disabled by default, do not enable if using ldn_mitm as
# it's a security risk - you have been warned!
anon = 1

# if anon is disabled, then user and pass must be set.
user = ""
pass = ""

[Network]
# port 21 is the default port for an ftp server, some platforms may not
# support using privileged ports, change if needed.
port = 21

timeout = 60

[Misc]
# use local time zone over gm (UTC) time zone.
use_localtime = 1

[Log]
log = 0

# options specific to Nintendo Switch
[Nx]
led = 1
skip_ascii_convert = 0
ENDOFFILE
echo "Done!"

# -------------------------------------------

### Write atmosphere system_settings.ini (Disable cheats by default)
echo "Writing atmosphere system_settings.ini..."
cat > "$OUTPUT_DIR/atmosphere/system_settings.ini" << ENDOFFILE
[atmosphere]
dmnt_cheats_enabled_by_default = u8!0x0
ENDOFFILE
echo "Done!"

# -------------------------------------------

echo "Success! Your Switch SD card structure (including aio-switch-updater) is beautifully prepared in '$OUTPUT_DIR'!"
