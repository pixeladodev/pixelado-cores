# Pixelado cores

The engines [Pixelado Party](https://github.com/pixeladodev/pixelado-party) runs to play games together. Each core is a
program of its own: Party bundles it in its package, runs it as a separate process and talks to it over a socket. Party
never links or loads a core.

| Folder | Core | What |
|---|---|---|
| `cores/<sistema>/<core>/` | | one core per folder: the system (`nds`, `gba`, `snes`) and the core (its emulator, with its profile when it has one: `bsnes-mercury`, `beetle-supafaust`). A system may have several cores |
| [`cores/nds/bananads/`](cores/nds/bananads/) | `pixelado-core-bananads` | Nintendo DS: BananaDS (from SuperDrastic) on melonDS, with local wireless play over the network, DS Download Play, the room's pause and the screen handed to Party for its menu |
| [`cores/gba/gpsp/`](cores/gba/gpsp/) | `pixelado-core-gpsp` | Game Boy Advance: gpSP on `retro/`, with the link cable and the wireless adapter played over the network, the room's pause and the screen handed to Party |
| [`retro/`](retro/) | | the minimal libretro frontend the retro cores are built on (not RetroArch): KMS, ALSA, evdev, the link between the handhelds |
| [`common/`](common/) | | what the cores share with Party: the channel, the screen handover and the room's socket, in C, and how Party runs a core |

Planned: `cores/snes/bsnes-mercury/` (and other SNES cores), on `retro/`.

Each core's folder pins its upstream sources (`upstream.lock`) and keeps our changes as patches, with a reproducible
build: the binary's sha-256 is in the folder's `manifest`. Releases are per core: tags `bananads-v<version>` and
`gpsp-v<version>`, assets `pixelado-core-bananads-<version>.zip` and `pixelado-core-gpsp-<version>.zip`.

```sh
cores/nds/bananads/build/build.sh    # Docker; cores/nds/bananads/dist/pixelado-core-bananads-<version>.zip
cores/gba/gpsp/build/build.sh        # Docker; cores/gba/gpsp/dist/pixelado-core-gpsp-<version>.zip
cores/gba/gpsp/tests/run.sh          # Docker (arm64): common/, retro/ and the GBA link tests
```

## Licence

GPLv3 (`LICENSE`), like BananaDS and melonDS (gpSP is GPL-2.0-or-later); credits in `NOTICE`. The cores contain no
BIOS, firmware or games from Nintendo and are not affiliated with Nintendo, BananaOS, BananaDS, melonDS, gpSP or libretro.

Parts of this repository were written with the help of AI coding assistants.

## Support

<a href="https://ko-fi.com/pixeladodev"><img src="docs/images/support-kofi.png" width="100%" alt="Pixelado Dev: pixel-art apps that let handhelds play together. Support me on Ko-fi."></a>
