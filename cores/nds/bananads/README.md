# nds/bananads: pixelado-core-bananads

The Nintendo DS core: **BananaDS** (from SuperDrastic) on **melonDS**, with our layers for playing together. This folder
holds no upstream code: `upstream.lock` pins BananaDS and melonDS, and `patches/` holds our changes.

- Local wireless play over the network the consoles are on (melonDS's LAN), DS Download Play, a direct Wi-Fi link (TDLS)
  between consoles, real latency measurement.
- Launched by Pixelado Party's room already in its session (`room.ctl`), with the room's pause: a player's menu pauses the
  game for everyone.
- The in-game menu handed to Pixelado Party (`party.fd`); without it, BananaDS's own menu.

## Build

```sh
build/build.sh          # Docker; dist/pixelado-core-bananads and dist/pixelado-core-bananads-<version>.zip
```

The build is reproducible: the binary's sha-256 must equal `binary_sha` in `manifest` (`PIXELADO_CORE_DRAFT=1` keeps a
binary that differs, while trying a change). Without network, from local copies: `PIXELADO_CORE_SUPERDRASTIC_GIT`,
`PIXELADO_CORE_MELONDS_TREE`, `PIXELADO_CORE_TOOLCHAIN_CACHE` (`build/build.sh`'s header).

## Change it

`dev/tree.sh` makes the sources editable (two git repositories: the pinned upstream and one commit per patch); edit and
commit there, then `dev/export.sh` writes `patches/` back and checks that, applied from scratch, they give the same tree.
See `patches/README.md`.

| Folder | What |
|---|---|
| `build/` | fetch the pinned sources, apply the layers, compile in Docker, pack |
| `dev/` | the editable tree: `tree.sh`, `export.sh` |
| `patches/` | `bananads/` (on BananaDS's code) and `melonds/` (on melonDS, after BananaDS's own) |
| `runtime/bananaos/` | the scripts that run it on BananaOS |
| `docs/RUNTIME.md` | the package, where it writes, the menu |
