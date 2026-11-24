# gba: pixelado-core-gba

The Game Boy Advance core: **gpSP** (libretro's, with its arm64 recompiler) in the frontend `retro/`, with the link
cable and the wireless adapter played over the network between the handhelds, launched by Pixelado Party's room
already in its session, with the room's pause and the menu handed to Party (`common/`). This folder holds no
upstream code: `upstream.lock` pins gpSP, and `patches/` holds our changes (none today).

## Link: what plays together

gpSP links through libretro's netpacket interface, which `retro/src/link.c` carries over TCP. It doesn't emulate the
cable byte by byte: it speaks a few protocols, picked per game (`gpsp_serial=auto`, its game database) or forced with
`--option gpsp_serial=…`:

| Mode | gpSP option | Games (gpSP's database, `auto`) |
|---|---|---|
| GBA Wireless Adapter | `rfu` | Pokémon FireRed, LeafGreen and Emerald (trades and battles in the Union Room), Mario Golf: Advance Tour, Mario Tennis: Power Tour, Mega Man Battle Network 5 and 6, Digimon Racing, Dragon Ball Z: Buu's Fury, Shrek SuperSlam, The Lord of the Rings: The Third Age, Hamtaro: Ham-Ham Games, The Chronicles of Narnia |
| Link cable, Pokémon Gen 3 | `mul_poke` | Pokémon Ruby and Sapphire (trades, battles), and Pokémon ROM hacks |
| Link cable, Advance Wars | `mul_aw1`, `mul_aw2` | Advance Wars, Advance Wars 2 |

- Up to 4 players in a Pixelado Party room (gpSP's cable modes take 4; its adapter more).
- Not supported by gpSP: the generic cable multiplayer other games use (e.g. Mario Kart: Super Circuit, Four
  Swords), the single-pak download play, the e-Reader, the GameCube link.
- Mixed games link when their modes match: FireRed with Emerald (both on the adapter). FireRed with Ruby needs both
  on the cable protocol (`gpsp_serial=mul_poke` on both: FireRed and LeafGreen have the cable too).
- Checked here with a test ROM built from source (`tests/linkrom/`, the wireless adapter's broadcast, connect and
  accept, both directions). Commercial games are not tested in this repository.

## Build

```sh
build/build.sh          # Docker; dist/pixelado-core-gba and dist/pixelado-core-gba-<version>.zip
tests/run.sh            # Docker, arm64: the tests (below)
```

The build is reproducible: Bootlin's aarch64 toolchain (the DS core's), gpSP at `/tmp/vmac-gba/src` and this
repository at `/cores` in the container, paths mapped away; the binary's sha-256 must equal `binary_sha` in
`manifest` (`PIXELADO_CORE_DRAFT=1` keeps one that differs, while trying a change). It links the console's glibc,
libdrm and alsa-lib; libgcc is static. Without network: `PIXELADO_CORE_GPSP_GIT` (a gpSP clone),
`PIXELADO_CORE_TOOLCHAIN_CACHE` (the toolchain and the console's libraries; the DS core's cache works).

## Tests

`tests/run.sh` runs in an arm64 Linux container (native on an arm64 Mac; GitHub's `ubuntu-24.04-arm` in CI):

- `common/`'s tests, and `retro/`'s with the fake core: the link's loopback (a guest before its host, packets both
  ways, the host's menu pausing the guest), and two cores with a scripted Party and room (the handover, `HOLD`,
  `SCREEN want`, `CMD op=quit`, `END`).
- gpSP built as on the handhelds, then the link test ROM: alone (it boots, draws, finds no host), and two cores linked
  over TCP, gpSP's wireless adapter on both: the guest finds the host's broadcast and connects, the host sees it; then
  the host's menu open for 6 s (longer than gpSP's 4 s link timeouts): the guest runs no frame meanwhile and the link
  stays up. The same on the release binary in `dist/` when it is there.

| Folder | What |
|---|---|
| `build/` | fetch gpSP, build it as a static libretro core (`build-core.sh`), link the frontend, pack |
| `patches/` | our changes to gpSP (none) |
| `tests/` | the test runner, its Docker image, the link test ROM |
| `docs/RUNTIME.md` | how it runs: the package, the files, the buttons, the exit codes |
