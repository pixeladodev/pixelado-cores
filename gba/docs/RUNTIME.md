# pixelado-core-gba at run time

## The package

`pixelado-core-gba-<version>.zip`, everything under `pixelado-core-gba/`:

| File | What |
|---|---|
| `pixelado-core-gba` | the engine: aarch64, glibc, libdrm and alsa-lib from the system (BananaOS has them) |
| `manifest` | `version=`, `binary_sha=` |
| `LICENSE`, `NOTICE` | GPL-3; the credits (gpSP GPL-2.0-or-later, libretro.h MIT) |
| `SOURCE` | the version and where its source is (tag `gba-v<version>`: `gba/`, `retro/`, `common/`) |

No BIOS is needed: gpSP has its own. A `gba_bios.bin` in `--system-dir` (BananaOS: `/data/bios`) is used when it is
there.

## What it reads and writes

| What | Where |
|---|---|
| the ROM | the last argument (a `.gba`; a `.zip` is unpacked by the launcher) |
| the save | `<save-dir>/<save-name>.srm`: BananaOS keeps every GBA core's saves in `/data/saves/gba/<game>.srm` (RetroArch, BananaFront), so the same save goes on in any of them. Without `--save-dir`, beside the ROM |
| the log | `--log FILE`, else stderr |
| the screen | DRM master on `/dev/dri/card0` (`PIXELADO_CORE_DRI`), the two panels |
| the sound | ALSA `default` (`PIXELADO_CORE_ALSA`) |
| the buttons | `/dev/input/event*`, read, never grabbed |
| the volume, the brightness | `/usr/libexec/banana/volume`, `/sys/class/backlight/backlight{0,1}`, `/data/banana/backlight` (only from Party's menu) |
| the link | TCP 7067 (`--room-port`): the host listens, the guests connect |

## Buttons

| Button | What |
|---|---|
| A, B, L, R, Start, Select, d-pad (or the left stick) | the GBA's |
| F alone, HOME, F2 or L3 (when let go) | the menu: Party's with Party, else the pause card on the second panel (A, B or Start resumes) |
| F + Start | quit |

## Exit codes

| Code | |
|---|---|
| 0 | the player quit |
| 10 | the room ended the game (`END`) |
| 11 | no session: the host didn't answer in 15 s, or refused (another core version, full) |
| 2 | an error (no display, the ROM didn't load) |

A lost link doesn't end the game: the game shows its own communication error, and the room says what to do.
