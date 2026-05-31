#!/bin/bash

### Credit to the Authors at https://rentry.org/CFWGuides
### Script created by Fraxalotl
### Modified for unified API download, path fixes, custom output directory, .nro/.ovl support, direct download URL support, and Full Conditional Modularization

# -------------------------------------------
# 定义基础 Release URL 变量（注释掉或删掉某一行即可完全跳过该组件）
# -------------------------------------------
HEKATE_URL="https://github.com/CTCaer/hekate/releases/latest"
ATMOSPHERE_URL="https://github.com/Atmosphere-NX/Atmosphere/releases/latest"
SIGPATCHES_URL="https://github.com/impeeza/sys-patch/releases/latest"
AKIRA_URL="https://github.com/xlanor/akira/releases/latest"
MISSION_CONTROL_URL="https://github.com/ndeadly/MissionControl/releases/latest"
DBI_URL="https://github.com/rashevskyv/dbi/releases/latest"
# SPHAIRA 使用固定直连
SPHAIRA_URL="https://github.com/ITTotalJustice/sphaira/releases/download/1.0.0/sphaira.zip"
EDIZON_SE_URL="https://github.com/tomvita/EdiZon-SE/releases/latest"
AIO_UPDATER_URL="https://github.com/HamletDuFromage/aio-switch-updater/releases/latest"
NX_SHELL_URL="https://github.com/Tproc-labs/NX-Shell-21.0.0/releases/latest"
# 【新增】：Ultrahand-Overlay 变量定义
ULTRAHAND_OVERLAY_URL="https://github.com/ppkantorski/Ultrahand-Overlay/releases/latest"

# MigFlash 官网下载页面 URL（不需要可以注释掉）
MIG_DUMP_PAGE_URL="https://migflash.com/downloads/"

# 定义统一的输出目标目录（末尾不加斜杠）
OUTPUT_DIR="./NS SD Card"

# -------------------------------------------
### 环境检测与 1.0 版本兼容性处理
# -------------------------------------------
if [[ "$GITHUB_ACTIONS" == "true" ]]; then
  echo "==> Detection: Running inside GitHub Actions Environment."
  if [[ -z "$GITHUB_TOKEN" ]]; then
    echo "==> Warning: GITHUB_ACTIONS is true but GITHUB_TOKEN is empty! API rate limits may apply."
  else
    echo "==> Authentication: GITHUB_TOKEN detected. Using GitHub Authenticated API Channel."
  fi
else
  echo "==> Detection: Running inside Local OS / Standard Bash Environment."
fi

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
mkdir -p "$OUTPUT_DIR/config/sys-patch"
mkdir -p "$OUTPUT_DIR/config/ftpsrv"
mkdir -p "$OUTPUT_DIR/atmosphere"

# -------------------------------------------
### 统一的 API 下载函数（智能环境感知、动态认证与直链识别机制）
# -------------------------------------------
download_latest_asset() {
  local repo_url=$1
  local output_path=$2
  local extension=${3:-".zip"} # 第三参数默认为 .zip
  
  echo "Processing $(basename "$output_path")...."
  
  # 如果传入的原本就是个已经包含 releases/download 的直链，直接下载并跳过 API 解析
  if [[ "$repo_url" == *"/releases/download/"* ]]; then
    echo "=> Notice: Direct download link provided. Skipping GitHub API parsing."
    echo "Downloading from direct link: $repo_url"
    curl -sL "$repo_url" -o "$output_path"
    if [[ $? -eq 0 && -f "$output_path" ]]; then
      echo "$(basename "$output_path") downloaded successfully from direct link."
      return 0
    else
      echo "Error: Failed to download from direct link: $repo_url"
      return 1
    fi
  fi

  # 将标准 GitHub URL 转换为 API URL
  local api_url=$(echo "$repo_url" | sed 's|github.com|api.github.com/repos|')
  local download_url=""

  # 仅原生注入 GITHUB_TOKEN 的 Authorization 头部
  local auth_header=()
  if [[ "$GITHUB_ACTIONS" == "true" && -n "$GITHUB_TOKEN" ]]; then
    auth_header=(-H "Authorization: token $GITHUB_TOKEN")
  fi

  # 拉取 API 核心 JSON 原始数据
  local api_response=$(curl "${auth_header[@]}" -sL "$api_url")

  # 针对特定仓库订制硬性筛选规则（大小写模糊匹配）
  if [[ "$repo_url" == *"/sphaira/"* ]]; then
    download_url=$(echo "$api_response" | jq -r 'try (.assets[] | select(.name | ascii_downcase == "sphaira.zip") | .browser_download_url) catch null' | head -n 1)
  elif [[ "$repo_url" == *"/Ultrahand-Overlay/"* ]]; then
    # 【新增】：精确锁死抓取官方指定的资产文件名 sdout.zip
    download_url=$(echo "$api_response" | jq -r 'try (.assets[] | select(.name | ascii_downcase == "sdout.zip") | .browser_download_url) catch null' | head -n 1)
  else
    # 通用后缀筛选逻辑（如 .zip, .nro, .ovl）
    download_url=$(echo "$api_response" | jq -r --arg ext "$extension" 'try (.assets[] | select(.name | ascii_downcase | endswith($ext | ascii_downcase)) | .browser_download_url) catch null' | head -n 1)
  fi
  
  # 降级保底容错
  if [[ -z "$download_url" || "$download_url" == "null" ]]; then
    echo "=> Notice: Primary API parsing returned null. Activating Fallback Anti-Null Mode..."
    download_url=$(echo "$api_response" | jq -r --arg ext "$extension" 'try (.assets[] | select(.name | ascii_downcase | endswith($ext | ascii_downcase)) | .browser_download_url) catch null' | head -n 1)
  fi

  if [[ -z "$download_url" || "$download_url" == "null" ]]; then
    echo "Error: Failed to fetch download URL for $output_path after dual-channel retries."
    echo "Waiting 5 seconds before continuing..."
    sleep 5
    return 1
  fi
  
  echo "Downloading from: $download_url"
  curl -sL "$download_url" -o "$output_path"
  echo "$(basename "$output_path") downloaded successfully."
  
  echo "Sleeping 5 seconds to prevent GitHub API rate limiting..."
  sleep 5
}

# =========================================================
# --- 第一阶段：条件判定下载核心/独立组件 ---
# =========================================================

# 1. 核心解压组件队列
[[ -n "$HEKATE_URL" ]] && download_latest_asset "$HEKATE_URL" "hekate.zip"
[[ -n "$ATMOSPHERE_URL" ]] && download_latest_asset "$ATMOSPHERE_URL" "atmosphere.zip"
[[ -n "$SIGPATCHES_URL" ]] && download_latest_asset "$SIGPATCHES_URL" "sigpatches.zip"
[[ -n "$MISSION_CONTROL_URL" ]] && download_latest_asset "$MISSION_CONTROL_URL" "missioncontrol.zip"
[[ -n "$EDIZON_SE_URL" ]] && download_latest_asset "$EDIZON_SE_URL" "edizon-se.zip"
[[ -n "$SPHAIRA_URL" ]] && download_latest_asset "$SPHAIRA_URL" "sphaira.zip"
[[ -n "$AIO_UPDATER_URL" ]] && download_latest_asset "$AIO_UPDATER_URL" "aio-switch-updater.zip"
# 【新增】：条件下载 Ultrahand 发布的 sdout.zip
[[ -n "$ULTRAHAND_OVERLAY_URL" ]] && download_latest_asset "$ULTRAHAND_OVERLAY_URL" "sdout.zip"

# 2. 独立单文件组件队列 (.nro / .ovl 格式直接重命名归档)
[[ -n "$AKIRA_URL" ]] && download_latest_asset "$AKIRA_URL" "$OUTPUT_DIR/switch/akira.nro" ".nro"
[[ -n "$DBI_URL" ]] && download_latest_asset "$DBI_URL" "$OUTPUT_DIR/switch/DBI/DBI.nro" ".nro"
[[ -n "$NX_SHELL_URL" ]] && download_latest_asset "$NX_SHELL_URL" "$OUTPUT_DIR/switch/NX-Shell.nro" ".nro"

# 3. 自定义非 GitHub 组件：MigDumpTool 动态解析与保底判定
if [[ -n "$MIG_DUMP_PAGE_URL" ]]; then
  echo "Processing MigDumpTool.nro..."
  MIG_DUMP_REAL_URL=$(curl -sL --connect-timeout 5 "$MIG_DUMP_PAGE_URL" | grep -o 'https://migflash.com/downloads/MigDumpTool-[^"]*\.nro' | head -n 1)

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
fi

# =========================================================
# --- 第二阶段：条件判定解压 Packages 到目标目录 ---
# =========================================================

echo "Unzipping Packages into $OUTPUT_DIR..."
rm -rf "$OUTPUT_DIR/bootloader" "$OUTPUT_DIR/atmosphere"

[[ -f "hekate.zip" ]] && unzip -u hekate.zip -d "$OUTPUT_DIR"
[[ -f "atmosphere.zip" ]] && unzip -u atmosphere.zip -d "$OUTPUT_DIR"
[[ -f "sigpatches.zip" ]] && unzip -u sigpatches.zip -d "$OUTPUT_DIR"
[[ -f "missioncontrol.zip" ]] && unzip -u missioncontrol.zip -d "$OUTPUT_DIR"
[[ -f "edizon-se.zip" ]] && unzip -u edizon-se.zip -d "$OUTPUT_DIR"
[[ -f "sphaira.zip" ]] && unzip -u sphaira.zip -d "$OUTPUT_DIR"
[[ -f "aio-switch-updater.zip" ]] && unzip -u aio-switch-updater.zip -d "$OUTPUT_DIR"
# 【新增】：条件解压 sdout.zip
[[ -f "sdout.zip" ]] && unzip -u sdout.zip -d "$OUTPUT_DIR"
echo "Unzip Stage Done!"

# =========================================================
# --- 第三阶段：条件判定清理临时 Zip 文件 ---
# =========================================================

echo "Cleaning up zip files..."
rm -f hekate.zip atmosphere.zip sigpatches.zip missioncontrol.zip edizon-se.zip sphaira.zip aio-switch-updater.zip sdout.zip
echo "Cleanup Stage Done!"

# =========================================================
# --- 第四阶段：移动核心 Payload 引导文件 ---
# =========================================================
mkdir -p "$OUTPUT_DIR/bootloader/payloads"

if [[ -f "fusee.bin" ]]; then
  if [[ "$OSTYPE" == "msys" ]]; then
    move fusee.bin "$OUTPUT_DIR/bootloader/payloads/"
  else
    mv fusee.bin "$OUTPUT_DIR/bootloader/payloads/"
  fi
  echo "fusee.bin moved to $OUTPUT_DIR/bootloader/payloads/"
elif [[ -f "$OUTPUT_DIR/fusee.bin" ]]; then
  mv "$OUTPUT_DIR/fusee.bin" "$OUTPUT_DIR/bootloader/payloads/"
  echo "fusee.bin relocated from output root to payloads."
else
  echo "Warning: fusee.bin not found."
fi

# =========================================================
# --- 第五阶段：条件写入定制化配置文件 ---
# =========================================================

### Write hekate_ipl.ini
if [[ -d "$OUTPUT_DIR/bootloader" ]]; then
  echo "Writing hekate_ipl.ini..."
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
fi

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
if [[ -d "$OUTPUT_DIR/atmosphere" ]]; then
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
fi

# -------------------------------------------

### Write sys-patch config.ini
if [[ -d "$OUTPUT_DIR/config/sys-patch" && -f "$OUTPUT_DIR/switch/sys-patch.nro" || -n "$SIGPATCHES_URL" ]]; then
  echo "Writing sys-patch config.ini..."
  cat > "$OUTPUT_DIR/config/sys-patch/config.ini" << ENDOFFILE
[options]
patch_sysmmc=0   ; 1=(default) patch sysmmc, 0=don't patch sysmmc
patch_emummc=1   ; 1=(default) patch emummc, 0=don't patch emummc
enable_logging=1 ; 1=(default) output /config/sys-patch/log.ini 0=no log
version_skip=1   ; 1=(default) skips out of date patterns, 0=search all patterns
ENDOFFILE
  echo "Done!"
fi

# -------------------------------------------

### Write sphaira / ftpsrv config.ini
if [[ -n "$SPHAIRA_URL" ]]; then
  echo "Writing ftpsrv config.ini..."
  cat > "$OUTPUT_DIR/config/ftpsrv/config.ini" << ENDOFFILE
##########
# sphaira and ftpsrv#
##########

#######################################################################
# Rename config.ini.template to config.ini for changes to take effect.#
#######################################################################

[Login]
anon = 1
user = ""
pass = ""

[Network]
port = 21
timeout = 60

[Misc]
use_localtime = 1

[Log]
log = 0

[Nx]
led = 1
skip_ascii_convert = 0
ENDOFFILE
  echo "Done!"
fi

# -------------------------------------------

### Write atmosphere system_settings.ini
if [[ -d "$OUTPUT_DIR/atmosphere" ]]; then
  echo "Writing atmosphere system_settings.ini..."
  cat > "$OUTPUT_DIR/atmosphere/system_settings.ini" << ENDOFFILE
[atmosphere]
dmnt_cheats_enabled_by_default = u8!0x0
ENDOFFILE
  echo "Done!"
fi

# -------------------------------------------

echo "Success! Your modular Switch SD card structure is beautifully prepared in '$OUTPUT_DIR'!"
