# PackUpdater

Homebrew NRO: on the Switch, download the latest `bao3/SwitchScript` SD-card zip
and extract it onto `sdmc:/`.

## On the Switch

1. Copy `PackUpdater.nro` to `sdmc:/switch/PackUpdater/`
   (the daily zip already includes this folder).
2. Open it from hbmenu. It checks GitHub Releases for
   `NS-SD-Card-Atmosphere-*.zip`.
3. Press A to download. After the zip is ready you choose:
   A = extract the full pack onto the SD root, or
   B = write only `PackUpdater.nro` (quit with +, reopen, then A).
   `Nintendo/` and game folders are never touched.
4. From the main screen, B downloads just `PackUpdater.nro` (~1 MB) so you
   can update the updater first without pulling the 60 MB pack.
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
