# SwitchScript

Build a Nintendo Switch SD-card layout: Hekate, Atmosphere, sys-patch, homebrew
(including CyberFoil), and **PackUpdater** (on-device zip update).

```bash
./switchScript.sh --dry-run          # resolve URLs, write nothing
./switchScript.sh                     # fill ./NS SD Card/
./switchScript.sh --output /mnt/sd
```

Comment out any `*_URL` in `switchScript.sh` to skip that component.

## What it installs

- Hekate + Nyx, Atmosphere + fusee.bin, sys-patch
- MissionControl, EdiZon-SE, Sphaira, AIO Switch Updater, Ultrahand Overlay
- CyberFoil (`switch/CyberFoil/`) preloaded with the LAN AeroFoil remote; DBI, JKSV, Akira, NX-Shell, MigDumpTool
- PackUpdater (`switch/PackUpdater/`) — download the latest GitHub zip on the Switch and extract it to the SD root
- `hekate_ipl.ini`, `exosphere.ini`, emuMMC DNS block, stratosphere `nogc=0`

GitHub Actions builds a zip release daily and compiles PackUpdater.nro.
Set `GITHUB_TOKEN` locally to avoid API rate limits.

On the Switch: hbmenu -> PackUpdater -> B to update PackUpdater first, or A for the full pack. See `pack-updater/README.md`.

Based on community CFW guides; not written or endorsed by those authors.
