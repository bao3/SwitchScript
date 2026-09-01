#!/usr/bin/env bash
# SwitchScript — build a Nintendo Switch SD-card homebrew layout.
# Components are declared once; comment out a URL to skip that component.
set -euo pipefail

### Credit: rentry.org/CFWGuides / Fraxalotl; maintained in bao3/SwitchScript

# -------------------------------------------
# CLI
# -------------------------------------------
DRY_RUN=0
CLEAN=1
usage() {
  cat <<'EOF'
Usage: ./switchScript.sh [--dry-run] [--no-clean] [--output DIR]

  --dry-run    Resolve download URLs and print the plan; write nothing.
  --no-clean   Do not wipe bootloader/atmosphere/config before extract.
  --output DIR Output root (default: ./NS SD Card)
EOF
}

OUTPUT_DIR="./NS SD Card"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run|-n) DRY_RUN=1; shift ;;
    --no-clean) CLEAN=0; shift ;;
    --output) OUTPUT_DIR="${2:?}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

# -------------------------------------------
# Component URLs — comment out a line to skip
# -------------------------------------------
HEKATE_URL="https://github.com/CTCaer/hekate/releases/latest"
ATMOSPHERE_URL="https://github.com/Atmosphere-NX/Atmosphere/releases/latest"
SIGPATCHES_URL="https://github.com/impeeza/sys-patch/releases/latest"
AKIRA_URL="https://github.com/xlanor/akira/releases/latest"
MISSION_CONTROL_URL="https://github.com/ndeadly/MissionControl/releases/latest"
DBI_URL="https://github.com/rashevskyv/dbi/releases/latest"
SPHAIRA_URL="https://github.com/ITotalJustice/sphaira/releases/latest"
EDIZON_SE_URL="https://github.com/tomvita/EdiZon-SE/releases/latest"
AIO_UPDATER_URL="https://github.com/HamletDuFromage/aio-switch-updater/releases/latest"
NX_SHELL_URL="https://github.com/Tproc-labs/NX-Shell-21.0.0/releases/latest"
ULTRAHAND_OVERLAY_URL="https://github.com/ppkantorski/Ultrahand-Overlay/releases/latest"
JKSV_URL="https://github.com/J-D-K/JKSV/releases/latest"
CYBERFOIL_URL="https://github.com/luketanti/CyberFoil/releases/latest"
MIG_DUMP_PAGE_URL="https://migflash.com/downloads/"

# -------------------------------------------
log()  { printf '==> %s\n' "$*"; }
warn() { printf '!!  %s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

run() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log "DRY-RUN: $*"
    return 0
  fi
  "$@"
}

ensure_jq() {
  if have jq; then
    return 0
  fi
  if [[ "$DRY_RUN" -eq 1 ]]; then
    warn "jq is not installed (needed for a real run)."
    return 0
  fi
  case "${OSTYPE:-}" in
    darwin*) brew install jq ;;
    linux-gnu*)
      if have apt-get; then
        sudo apt-get install -y jq
      else
        warn "Install jq, then re-run."; exit 1
      fi
      ;;
    msys*|cygwin*|mingw*)
      warn "Install jq (e.g. choco install jq) and re-run."; exit 1
      ;;
    *) warn "Install jq and re-run."; exit 1 ;;
  esac
}

# GitHub API helpers. Uses GITHUB_TOKEN when set (Actions or local export).
gh_auth_args() {
  if [[ -n "${GITHUB_TOKEN:-}" ]]; then
    printf '%s\n' -H "Authorization: Bearer ${GITHUB_TOKEN}"
  fi
}

github_repo_from_url() {
  # https://github.com/owner/repo[/releases/latest|/releases/download/...]
  local url=$1
  echo "$url" | sed -E 's|https?://github.com/||; s|/releases/.*||; s|\.git$||; s|/$||'
}

resolve_latest_asset() {
  # args: github_url  name_filter
  # name_filter: suffix (.zip) or exact filename (fusee.bin, sdout.zip)
  local url=$1
  local filter=$2
  local repo api json

  if [[ "$url" == *"/releases/download/"* ]]; then
    echo "$url"
    return 0
  fi

  repo=$(github_repo_from_url "$url")
  api="https://api.github.com/repos/${repo}/releases/latest"
  json=$(curl -fsSL --retry 2 --retry-delay 1 --retry-all-errors \
    -H "Accept: application/vnd.github+json" \
    -H "User-Agent: bao3-SwitchScript" \
    $(gh_auth_args) \
    "$api") || return 1

  echo "$json" | jq -r --arg f "$filter" '
    .assets
    | map(.browser_download_url)
    | map(select(
        ( ($f | startswith(".")) and (ascii_downcase | endswith($f)))
        or ((. | split("/") | last | ascii_downcase) == ($f | ascii_downcase))
      ))
    | .[0] // empty
  '
}

download_file() {
  local src=$1
  local dest=$2
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log "DRY-RUN: download $(basename "$dest") <- $src"
    return 0
  fi
  mkdir -p "$(dirname "$dest")"
  log "Downloading $(basename "$dest")"
  curl -fL --retry 2 --retry-delay 1 --retry-all-errors \
    -H "User-Agent: bao3-SwitchScript" \
    $(gh_auth_args) \
    -o "$dest" "$src"
}

fetch_github_asset() {
  # url dest filter  — skip if url empty
  local url=${1:-}
  local dest=$2
  local filter=${3:-.zip}
  [[ -n "$url" ]] || return 0
  local asset
  asset=$(resolve_latest_asset "$url" "$filter") || {
    warn "Failed to resolve asset for $dest"
    return 1
  }
  if [[ -z "$asset" ]]; then
    warn "No asset matching '$filter' for $url"
    return 1
  fi
  download_file "$asset" "$dest"
}

must_fetch() { fetch_github_asset "$@" || { warn "required download failed: $2"; exit 1; }; }
opt_fetch()  { fetch_github_asset "$@" || warn "optional download skipped: $2"; }

unzip_into() {
  local zip=$1
  local dest=$2
  local flags=${3:--o}
  [[ -f "$zip" || "$DRY_RUN" -eq 1 ]] || return 0
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log "DRY-RUN: unzip $flags $zip -> $dest"
    return 0
  fi
  unzip $flags "$zip" -d "$dest"
}

# Place a zip that may be a raw nro, switch/<name>/, or nested folder.
smart_place_homebrew() {
  local zip=$1
  local dest_subdir=$2   # e.g. switch/CyberFoil
  local nro_name=$3      # e.g. cyberfoil.nro
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log "DRY-RUN: smart-place $zip -> $OUTPUT_DIR/$dest_subdir/"
    return 0
  fi
  [[ -f "$zip" ]] || return 0
  local tmp
  tmp=$(mktemp -d)
  unzip -o "$zip" -d "$tmp" >/dev/null
  mkdir -p "$OUTPUT_DIR/$dest_subdir"
  if [[ -f "$tmp/$nro_name" ]]; then
    mv "$tmp/$nro_name" "$OUTPUT_DIR/$dest_subdir/"
  elif [[ -d "$tmp/$(basename "$dest_subdir")" ]]; then
    cp -a "$tmp/$(basename "$dest_subdir")/." "$OUTPUT_DIR/$dest_subdir/"
  elif [[ -d "$tmp/switch/$(basename "$dest_subdir")" ]]; then
    cp -a "$tmp/switch/$(basename "$dest_subdir")/." "$OUTPUT_DIR/$dest_subdir/"
  elif [[ -d "$tmp/switch" ]]; then
    cp -a "$tmp/switch/." "$OUTPUT_DIR/switch/"
  else
    cp -a "$tmp"/. "$OUTPUT_DIR/$dest_subdir/" 2>/dev/null || true
  fi
  rm -rf "$tmp"
}

write_text() {
  local path=$1
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log "DRY-RUN: write $path"
    return 0
  fi
  mkdir -p "$(dirname "$path")"
  cat > "$path"
}

# -------------------------------------------
log "SwitchScript  output=$OUTPUT_DIR  dry_run=$DRY_RUN  clean=$CLEAN"
if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
  if [[ -n "${GITHUB_TOKEN:-}" ]]; then
    log "Environment: GitHub Actions (token set)"
  else
    log "Environment: GitHub Actions (token unset — rate limits likely)"
  fi
else
  log "Environment: local"
fi
ensure_jq

WORKDIR=$(pwd)
HEKATE_ZIP="$WORKDIR/hekate.zip"
AMS_ZIP="$WORKDIR/atmosphere.zip"
FUSEE_BIN="$WORKDIR/fusee.bin"
SIG_ZIP="$WORKDIR/sigpatches.zip"
MC_ZIP="$WORKDIR/missioncontrol.zip"
EDIZON_ZIP="$WORKDIR/edizon-se.zip"
SPHAIRA_ZIP="$WORKDIR/sphaira.zip"
AIO_ZIP="$WORKDIR/aio-switch-updater.zip"
ULTRA_ZIP="$WORKDIR/sdout.zip"
CYBER_ZIP="$WORKDIR/cyberfoil.zip"

if [[ "$DRY_RUN" -eq 0 ]]; then
  mkdir -p "$OUTPUT_DIR"/{switch/DBI,switch/MigDumpTool,config/sys-patch,config/ftpsrv,atmosphere,bootloader/ini}
fi

# ---- downloads (optional components skip when URL is empty) ----
must_fetch "$HEKATE_URL" "$HEKATE_ZIP" ".zip"
must_fetch "$ATMOSPHERE_URL" "$AMS_ZIP" ".zip"
must_fetch "$ATMOSPHERE_URL" "$FUSEE_BIN" "fusee.bin"
opt_fetch "$SIGPATCHES_URL" "$SIG_ZIP" ".zip"
opt_fetch "$MISSION_CONTROL_URL" "$MC_ZIP" ".zip"
opt_fetch "$EDIZON_SE_URL" "$EDIZON_ZIP" ".zip"
opt_fetch "$SPHAIRA_URL" "$SPHAIRA_ZIP" ".zip"
opt_fetch "$AIO_UPDATER_URL" "$AIO_ZIP" ".zip"
opt_fetch "$ULTRAHAND_OVERLAY_URL" "$ULTRA_ZIP" "sdout.zip"
opt_fetch "$CYBERFOIL_URL" "$CYBER_ZIP" ".zip"

opt_fetch "$AKIRA_URL" "$OUTPUT_DIR/switch/akira.nro" ".nro"
opt_fetch "$DBI_URL" "$OUTPUT_DIR/switch/DBI/DBI.nro" ".nro"
opt_fetch "$NX_SHELL_URL" "$OUTPUT_DIR/switch/NX-Shell.nro" ".nro"
opt_fetch "$JKSV_URL" "$OUTPUT_DIR/switch/JKSV.nro" ".nro"

if [[ -n "${MIG_DUMP_PAGE_URL:-}" ]]; then
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log "DRY-RUN: scrape MigDumpTool.nro from $MIG_DUMP_PAGE_URL"
  else
    log "Resolving MigDumpTool.nro"
    MIG_DUMP_REAL_URL=$(curl -sL --connect-timeout 8 "$MIG_DUMP_PAGE_URL" \
      | grep -oE 'https://migflash.com/downloads/MigDumpTool-[^"]+\.nro' | head -n 1 || true)
    if [[ -z "${MIG_DUMP_REAL_URL:-}" ]]; then
      warn "MigDumpTool parse failed; using fallback v0.0.2"
      MIG_DUMP_REAL_URL="https://migflash.com/downloads/MigDumpTool-v0.0.2.nro"
    fi
    download_file "$MIG_DUMP_REAL_URL" "$OUTPUT_DIR/switch/MigDumpTool/MigDumpTool.nro"
  fi
fi

# ---- extract ----
if [[ "$DRY_RUN" -eq 0 && "$CLEAN" -eq 1 ]]; then
  log "Clean extract: removing previous bootloader/atmosphere/config"
  rm -rf "$OUTPUT_DIR/bootloader" "$OUTPUT_DIR/atmosphere" "$OUTPUT_DIR/config"
fi

unzip_into "$HEKATE_ZIP" "$OUTPUT_DIR" -u
unzip_into "$SIG_ZIP" "$OUTPUT_DIR" -u
unzip_into "$MC_ZIP" "$OUTPUT_DIR" -u
unzip_into "$EDIZON_ZIP" "$OUTPUT_DIR" -u
unzip_into "$AIO_ZIP" "$OUTPUT_DIR" -u
unzip_into "$ULTRA_ZIP" "$OUTPUT_DIR" -u
smart_place_homebrew "$SPHAIRA_ZIP" "switch/sphaira" "sphaira.nro"
smart_place_homebrew "$CYBER_ZIP" "switch/CyberFoil" "cyberfoil.nro"
# Atmosphere last so it wins on overlapping paths
unzip_into "$AMS_ZIP" "$OUTPUT_DIR" -o

if [[ "$DRY_RUN" -eq 0 ]]; then
  log "Removing zip scratch files"
  rm -f "$HEKATE_ZIP" "$AMS_ZIP" "$SIG_ZIP" "$MC_ZIP" "$EDIZON_ZIP" \
        "$SPHAIRA_ZIP" "$AIO_ZIP" "$ULTRA_ZIP" "$CYBER_ZIP"
fi

mkdir -p "$OUTPUT_DIR/bootloader/payloads"
if [[ "$DRY_RUN" -eq 1 ]]; then
  log "DRY-RUN: move fusee.bin -> bootloader/payloads/"
elif [[ -f "$FUSEE_BIN" ]]; then
  mv "$FUSEE_BIN" "$OUTPUT_DIR/bootloader/payloads/"
elif [[ -f "$OUTPUT_DIR/fusee.bin" ]]; then
  mv "$OUTPUT_DIR/fusee.bin" "$OUTPUT_DIR/bootloader/payloads/"
else
  warn "fusee.bin not found"
fi

# ---- config files ----
write_text "$OUTPUT_DIR/bootloader/hekate_ipl.ini" <<'ENDOFFILE'
[config]
autoboot=0
autoboot_list=0
bootwait=3
backlight=100
autohosoff=0
autonogc=1
updater2p=0
bootprotect=0

[CFW - emuMMC]
fss0=atmosphere/package3
icon=bootloader/res/icon_payload.bmp

[Stock - Pure Official]
fss0=atmosphere/package3
stock=1
emummc_force_disable=1
icon=bootloader/res/icon_switch.bmp
ENDOFFILE

write_text "$OUTPUT_DIR/bootloader/ini/CFW-sysNAND.ini" <<'ENDOFFILE'
[CFW-sysNAND]
pkg3=atmosphere/package3
emummc_force_disable=1
icon=bootloader/res/icon_switch.bmp
ENDOFFILE

write_text "$OUTPUT_DIR/exosphere.ini" <<'ENDOFFILE'
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

write_text "$OUTPUT_DIR/atmosphere/hosts/emummc.txt" <<'ENDOFFILE'
# Block Nintendo Servers (Only affects emuMMC)
127.0.0.1 *nintendo.*
127.0.0.1 *nintendo-europe.com
127.0.0.1 *nintendoswitch.*
95.216.149.205 *conntest.nintendowifi.net
95.216.149.205 *ctest.cdn.nintendo.net
ENDOFFILE

if [[ -n "${SIGPATCHES_URL:-}" ]]; then
  write_text "$OUTPUT_DIR/config/sys-patch/config.ini" <<'ENDOFFILE'
[options]
patch_sysmmc=1
patch_emummc=1
enable_logging=1
version_skip=1
ENDOFFILE
fi

if [[ -n "${SPHAIRA_URL:-}" ]]; then
  write_text "$OUTPUT_DIR/config/ftpsrv/config.ini" <<'ENDOFFILE'
##########
# sphaira and ftpsrv#
##########
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
fi

write_text "$OUTPUT_DIR/atmosphere/system_settings.ini" <<'ENDOFFILE'
[atmosphere]
dmnt_cheats_enabled_by_default = u8!0x0
ENDOFFILE

write_text "$OUTPUT_DIR/atmosphere/config/stratosphere.ini" <<'ENDOFFILE'
[stratosphere]
# 0 = do not hard-lock the game card slot; Hekate autonogc still protects burns
nogc = 0
ENDOFFILE

log "Success. Layout is in '$OUTPUT_DIR'"
if [[ "$DRY_RUN" -eq 1 ]]; then
  log "Dry-run only; no files were written."
fi
