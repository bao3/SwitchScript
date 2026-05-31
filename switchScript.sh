#!/bin/bash

### Credit to the Authors at https://rentry.org/CFWGuides
### Script created by Fraxalotl
### Modified for unified API download, path fixes, and custom output directory

# -------------------------------------------
# 定义基础 Release URL 变量
# -------------------------------------------
HEKATE_URL="https://github.com/CTCaer/hekate/releases/latest"
ATMOSPHERE_URL="https://github.com/Atmosphere-NX/Atmosphere/releases/latest"
SIGPATCHES_URL="https://github.com/impeeza/sys-patch/releases/latest"

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

# 确保输出目标目录存在
mkdir -p "$OUTPUT_DIR"

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
### Unzip Downloaded Packages to Target Directory

echo "Unzipping Zips into $OUTPUT_DIR..."
# 提前清理目标目录中可能影响解压的旧目录，确保全新覆盖更新
rm -rf "$OUTPUT_DIR/bootloader" "$OUTPUT_DIR/atmosphere"

# 使用 -d 参数指定解压到目标文件夹
unzip -u hekate.zip -d "$OUTPUT_DIR"
unzip -u atmosphere.zip -d "$OUTPUT_DIR"
unzip -u sigpatches.zip -d "$OUTPUT_DIR"
echo "Done!"

### Cleanup Downloaded Zips

echo "Cleaning up zip files..."
rm -f hekate.zip
rm -f atmosphere.zip
rm -f sigpatches.zip
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
# 防御性代码：有时某些整合包可能会直接解压到目标根目录，这里做个二次校验
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

echo "Success! Your Switch SD card structure is beautifully prepared in '$OUTPUT_DIR'!"
