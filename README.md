# Pixelado cores

The engines [Pixelado Party](https://github.com/pixeladodev/pixelado-party) runs to play games together. Each core is a
program of its own: Party bundles it in its package, runs it as a separate process and talks to it over a socket. Party
never links or loads a core.

| Folder | Core | What |
|---|---|---|
| [`ds/`](ds/) | `pixelado-core-ds` | Nintendo DS: BananaDS (from SuperDrastic) on melonDS, with local wireless play over the network, DS Download Play, the room's pause and the screen handed to Party for its menu |
| [`common/`](common/) | | what the cores share with Party: the channel and the screen handover (described; the code is in `ds/` for now) |

Planned: `gba/` (gpSP, with netpacket), `snes/`, and a minimal libretro host they share.

Each folder pins its upstream sources (`upstream.lock`) and keeps our changes as patches, with a reproducible build:
the binary's sha-256 is in the folder's `manifest`. Releases are per core: tag `ds-v<version>`, asset
`pixelado-core-ds-<version>.zip`.

```sh
ds/build/build.sh       # Docker; ds/dist/pixelado-core-ds-<version>.zip
```

## Licence

GPLv3 (`LICENSE`), like BananaDS and melonDS; credits in `NOTICE`. The cores contain no BIOS, firmware or games and
are not affiliated with Nintendo, BananaOS, BananaDS or melonDS.

Parts of this repository were written with the help of AI coding assistants.

## Support

<a href="https://ko-fi.com/pixeladodev"><img src="docs/images/support-kofi.png" width="100%" alt="Pixelado Dev: pixel-art apps that let handhelds play together. Support me on Ko-fi."></a>
