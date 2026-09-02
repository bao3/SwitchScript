# PackUpdater

Homebrew NRO: on the Switch, download the latest `bao3/SwitchScript` SD-card zip
and extract it onto `sdmc:/`.

## On the Switch

1. Copy `PackUpdater.nro` to `sdmc:/switch/PackUpdater/`
   (the daily zip already includes this folder). First launch writes
   vendored `TegraExplorer.bin` next to the NRO if it is missing.
2. Open it from hbmenu. It checks GitHub Releases for
   `NS-SD-Card-Atmosphere-*.zip`.
3. Press A to download and extract onto the SD root in one shot.
   `atmosphere/package3` and `stratosphere.romfs` are written as `.aio`
   sidecars (not live files). Press X: reboot into TegraExplorer, which
   copies them into place before Atmosphere starts (AIO-style).
   Other files use tmp + pre-size + rename. `Nintendo/` is never touched.
   The zip also puts `TegraExplorer.bin` and `Lockpick_RCM.bin` in
   `bootloader/payloads/` for Hekate's Payloads menu.
4. Optional: from the main screen, B writes only `PackUpdater.nro`.
5. Press X to reboot when a full extract finishes.

Tiny / empty releases are refused (must contain `atmosphere/package3` and
be at least 10 MB). Config: `sdmc:/switch/PackUpdater/config.ini`.

Default download path is the Cloudflare Worker
`https://gh.heibang.club/https://github.com/...` (China-reachable). Clear
`gh_proxy` to talk to GitHub directly. `proxy =` is an HTTP CONNECT proxy
and is usually empty.

## Build

Needs [devkitPro](https://devkitpro.org/) `switch-dev` + `switch-curl` (the
`devkitpro/devkita64` Docker image has both).

```
docker run --rm -v "$PWD:/src" -w /src/pack-updater devkitpro/devkita64:latest make
```

Host tests (no toolchain):

```
gcc -O2 -Wall -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES \
  -o pack-updater/tests/host_test \
  pack-updater/tests/host_test.c \
  pack-updater/source/json.c \
  pack-updater/source/unzip.c \
  pack-updater/source/miniz.c
./pack-updater/tests/host_test
```

GitHub Actions builds the NRO, drops it into the SD zip, and also attaches
`PackUpdater.nro` as a separate release asset.
