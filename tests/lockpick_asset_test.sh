#!/usr/bin/env bash
# Asset selection for impeeza/Lockpick_RCMDecScots.
# Latest releases dropped the raw Lockpick_RCM.bin and ship versioned zips.
# Lockpick_RCM_unc.bin is the pre-loader payload and must not be installed.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT="$ROOT/switchScript.sh"

if ! grep -q '^pick_lockpick_asset_urls()' "$SCRIPT"; then
  echo "FAIL: pick_lockpick_asset_urls is not defined" >&2
  exit 1
fi
if ! grep -q 'SWITCHSCRIPT_SOURCE_ONLY' "$SCRIPT"; then
  echo "FAIL: switchScript.sh cannot be sourced without running downloads" >&2
  exit 1
fi

# shellcheck disable=SC1090
SWITCHSCRIPT_SOURCE_ONLY=1 source "$SCRIPT"

assert_eq() {
  local name=$1 expected=$2 actual=$3
  if [[ "$actual" != "$expected" ]]; then
    printf 'FAIL: %s\n  expected: %s\n  actual:   %s\n' "$name" "$expected" "$actual" >&2
    exit 1
  fi
}

pick() {
  pick_lockpick_asset_urls <<EOF
$1
EOF
}

v201=$(pick '{
  "assets": [
    {"name":"Lockpick_RCM-2.0.1.zip","browser_download_url":"https://example.invalid/plain.zip"},
    {"name":"Lockpick_RCM-2.0.1_Hekate.zip","browser_download_url":"https://example.invalid/hekate.zip"}
  ]
}')
assert_eq "v2.0.1 prefers Hekate zip, then plain zip" \
  $'https://example.invalid/hekate.zip\nhttps://example.invalid/plain.zip' \
  "$v201"

v1920=$(pick '{
  "assets": [
    {"name":"Lockpick_RCM-1.9.20.zip","browser_download_url":"https://example.invalid/plain.zip"},
    {"name":"Lockpick_RCM.bin","browser_download_url":"https://example.invalid/raw.bin"}
  ]
}')
assert_eq "1.9.20 prefers the raw payload over the zip" \
  $'https://example.invalid/raw.bin\nhttps://example.invalid/plain.zip' \
  "$v1920"

v1917=$(pick '{
  "assets": [
    {"name":"Lockpick_RCM.zip","browser_download_url":"https://example.invalid/plain.zip"},
    {"name":"Lockpick_RCM_Hekate.zip","browser_download_url":"https://example.invalid/hekate.zip"}
  ]
}')
assert_eq "1.9.17 prefers Lockpick_RCM_Hekate.zip" \
  $'https://example.invalid/hekate.zip\nhttps://example.invalid/plain.zip' \
  "$v1917"

v1916=$(pick '{
  "assets": [
    {"name":"Lockpick_RCM.zip","browser_download_url":"https://example.invalid/plain.zip"}
  ]
}')
assert_eq "zip-only release still selects the zip" \
  "https://example.invalid/plain.zip" \
  "$v1916"

versioned_bin=$(pick '{
  "assets": [
    {"name":"Lockpick_RCM-9.9.9.zip","browser_download_url":"https://example.invalid/a.zip"},
    {"name":"Lockpick_RCM-9.9.9.bin","browser_download_url":"https://example.invalid/a.bin"}
  ]
}')
assert_eq "versioned .bin beats a zip" \
  $'https://example.invalid/a.bin\nhttps://example.invalid/a.zip' \
  "$versioned_bin"

unc_only=$(pick '{
  "assets": [
    {"name":"Lockpick_RCM_unc.bin","browser_download_url":"https://example.invalid/unc.bin"},
    {"name":"README.txt","browser_download_url":"https://example.invalid/readme.txt"}
  ]
}')
assert_eq "uncompressed payload is not a candidate" "" "$unc_only"

case_bin=$(pick '{
  "assets": [
    {"name":"LOCKPICK_RCM.BIN","browser_download_url":"https://example.invalid/raw.bin"}
  ]
}')
assert_eq "asset name match is case-insensitive" \
  "https://example.invalid/raw.bin" \
  "$case_bin"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

printf 'hekate-payload' > "$tmp/Lockpick_RCM.bin"
printf 'uncompressed' > "$tmp/Lockpick_RCM_unc.bin"
# _unc is stored first so a "first *.bin" matcher would pick the wrong payload.
(cd "$tmp" && zip -q both.zip Lockpick_RCM_unc.bin Lockpick_RCM.bin)
mkdir -p "$tmp/nested/bootloader/payloads"
printf 'nested-payload' > "$tmp/nested/bootloader/payloads/Lockpick_RCM.bin"
(cd "$tmp/nested" && zip -q -r "$tmp/hekate.zip" bootloader)
(cd "$tmp" && zip -q unc.zip Lockpick_RCM_unc.bin)

got="$tmp/out.bin"
extract_zip_basename "$tmp/both.zip" "$got" "Lockpick_RCM.bin"
assert_eq "plain zip extracts wrapped payload, not _unc" "hekate-payload" "$(cat "$got")"

rm -f "$got"
extract_zip_basename "$tmp/hekate.zip" "$got" "Lockpick_RCM.bin"
assert_eq "hekate zip extracts bootloader/payloads member" "nested-payload" "$(cat "$got")"

if extract_zip_basename "$tmp/unc.zip" "$tmp/should-not-exist" "Lockpick_RCM.bin"; then
  echo "FAIL: unc-only archive should not yield Lockpick_RCM.bin" >&2
  exit 1
fi

# fetch_lockpick: bad Hekate zip falls through to the plain zip.
fetch_json='{
  "assets": [
    {"name":"Lockpick_RCM-9_Hekate.zip","browser_download_url":"https://example.invalid/bad.zip"},
    {"name":"Lockpick_RCM-9.zip","browser_download_url":"https://example.invalid/good.zip"}
  ]
}'
curl_gh() { printf '%s\n' "$fetch_json"; }
download_file() {
  case "$1" in
    */bad.zip) cp "$tmp/unc.zip" "$2" ;;
    */good.zip) cp "$tmp/both.zip" "$2" ;;
    *) echo "unexpected download $1" >&2; return 1 ;;
  esac
}
dest="$tmp/fetched.bin"
DRY_RUN=0
fetch_lockpick "https://github.com/impeeza/Lockpick_RCMDecScots/releases/latest" "$dest"
assert_eq "fallback zip supplies Lockpick_RCM.bin" "hekate-payload" "$(cat "$dest")"

# Dry-run reports the preferred asset and writes nothing.
dry_dest="$tmp/dry.bin"
dry_out=$(
  DRY_RUN=1
  fetch_lockpick "https://github.com/impeeza/Lockpick_RCMDecScots/releases/latest" "$dry_dest"
)
if [[ -e "$dry_dest" ]]; then
  echo "FAIL: dry-run wrote $dry_dest" >&2
  exit 1
fi
case "$dry_out" in
  *bad.zip*) ;;
  *) echo "FAIL: dry-run did not name the preferred asset: $dry_out" >&2; exit 1 ;;
esac

if grep -q 'must_fetch "$LOCKPICK_URL".*Lockpick_RCM\.bin' "$SCRIPT"; then
  echo "FAIL: script still requires an exact Lockpick_RCM.bin release asset" >&2
  exit 1
fi

echo "ok: lockpick asset selection"
