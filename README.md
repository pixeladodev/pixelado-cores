# Pixelado cores

The engines [Pixelado Party](https://github.com/pixeladodev/pixelado-party) runs to play games together. Each core is a
program of its own: Party bundles it in its package, runs it as a separate process and talks to it over a socket. Party
never links or loads a core.

| Folder | Core | What |
|---|---|---|
| [`ds/`](ds/) | `pixelado-core-ds` | Nintendo DS: BananaDS (from SuperDrastic) on melonDS, with local wireless play over the network, DS Download Play, the room's pause and the screen handed to Party for its menu |
| [`gba/`](gba/) | `pixelado-core-gba` | Game Boy Advance: gpSP on `retro/`, with the link cable and the wireless adapter played over the network, the room's pause and the screen handed to Party |
| [`retro/`](retro/) | | the minimal libretro frontend the retro cores are built on (not RetroArch): KMS, ALSA, evdev, the link between the handhelds |
| [`common/`](common/) | | what the cores share with Party: the channel, the screen handover and the room's socket, in C, and how Party runs a core |

Planned: `snes/`, on `retro/`.

Each core's folder pins its upstream sources (`upstream.lock`) and keeps our changes as patches, with a reproducible
build: the binary's sha-256 is in the folder's `manifest`. Releases are per core: tags `ds-v<version>` and
`gba-v<version>`, assets `pixelado-core-ds-<version>.zip` and `pixelado-core-gba-<version>.zip`.

```sh
ds/build/build.sh       # Docker; ds/dist/pixelado-core-ds-<version>.zip
gba/build/build.sh      # Docker; gba/dist/pixelado-core-gba-<version>.zip
gba/tests/run.sh        # Docker (arm64): common/, retro/ and the GBA link tests
```

## Licence

GPLv3 (`LICENSE`), like BananaDS and melonDS (gpSP is GPL-2.0-or-later); credits in `NOTICE`. The cores contain no
BIOS, firmware or games from Nintendo and are not affiliated with Nintendo, BananaOS, BananaDS, melonDS, gpSP or libretro.

Parts of this repository were written with the help of AI coding assistants.

## Support

<a href="https://ko-fi.com/pixeladodev"><img src="docs/images/support-kofi.png" width="100%" alt="Pixelado Dev: pixel-art apps that let handhelds play together. Support me on Ko-fi."></a>
