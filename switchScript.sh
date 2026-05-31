#!/bin/bash

### Credit to the Authors at https://rentry.org/CFWGuides
### Script created by Fraxalotl
### Modified for unified API download and path fixes

# -------------------------------------------
# 定义基础 Release URL 变量（统一使用标准官方 Release 首页）
# -------------------------------------------
HEKATE_URL="https://github.com/CTCaer/hekate/releases/latest"
ATMOSPHERE_URL="https://github.com/Atmosphere-NX/Atmosphere/releases/latest"
SIGPATCHES_URL="https://github.com/impeeza/sys-patch/releases/latest"

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

# -------------------------------------------
### 统一的 API 下载函数
# -------------------------------------------
download_latest_asset() {
  local repo_url=$1
  local output_name=$2
  echo "Processing $output_name..."
  
  # 将标准 GitHub URL 转换为 API URL
  local api_url=$(echo "$repo_url" | sed 's|github.com|api.github.com/repos|')
  
  # 请求 API 并精准抓取第一个满足条件的 .zip 资产下载链接
  local download_url=$(curl -sL "$api_url" | jq -r '.assets[] | select(.name | endswith(".zip")) | .browser_download_url' | head -n 1)
  
  if [[ -z "$download_url" || "$download_url" == "null" ]]; then
    echo "Error: Failed to fetch download URL for $output_name"
    return 1
  fi
  
  echo "Downloading from: $download_url"
  curl -sL "$download_url" -o "$output_name"
  echo "$output_name downloaded successfully."
}

# 执行统一构建下载
download_latest_asset "$HEKATE_URL" "hekate.zip"
download_latest_asset "$ATMOSPHERE_URL" "atmosphere.zip"
download_latest_asset "$SIGPATCHES_URL" "sigpatches.zip"

# -------------------------------------------
### Unzip Downloaded Packages

echo "Unzipping Zips..."
# 提前清理可能影响解压的旧目录，确保全新覆盖更新
rm -rf bootloader atmosphere
unzip -u hekate.zip
unzip -u atmosphere.zip
unzip -u sigpatches.zip
echo "Done!"

### Cleanup Downloaded Zips

echo "Cleaning up zip files..."
rm -f hekate.zip
rm -f atmosphere.zip
rm -f sigpatches.zip
echo "Done!"

# -------------------------------------------
### 移动 fusee.bin (修复路径并增加防御性容错)
# -------------------------------------------
mkdir -p bootloader/payloads

if [[ -f "fusee.bin" ]]; then
  if [[ "$OSTYPE" == "msys" ]]; then
    move fusee.bin bootloader/payloads/
  else
    mv fusee.bin bootloader/payloads/
  fi
  echo "fusee.bin moved to bootloader/payloads/"
else
  echo "Warning: fusee.bin not found in root. Checking if it is already in place or misplaced..."
fi

# -------------------------------------------
### 写入配置文件 (全部修正为相对路径，去掉开头的 '/')
# -------------------------------------------

### Write hekate_ipl.ini in bootloader/ directory
echo "Writing hekate_ipl.ini in bootloader/ directory..."
mkdir -p bootloader
cat > bootloader/hekate_ipl.ini << ENDOFFILE
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

### write exosphere.ini in root of SD Card
echo "Writing exosphere.ini..."
cat > exosphere.ini << ENDOFFILE
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

### Write default.txt in atmosphere/hosts
echo "Writing default.txt in atmosphere/hosts..."
mkdir -p atmosphere/hosts
cat > atmosphere/hosts/default.txt << ENDOFFILE
# Block Nintendo Servers
127.0.0.1 *nintendo.*
127.0.0.1 *nintendo-europe.com
127.0.0.1 *nintendoswitch.*
95.216.149.205 *conntest.nintendowifi.net
95.216.149.205 *ctest.cdn.nintendo.net
ENDOFFILE
echo "Done!"

# -------------------------------------------

echo "Your Switch SD card directory structure is prepared in the current folder!"
