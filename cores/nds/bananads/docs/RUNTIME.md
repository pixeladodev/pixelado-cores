# pixelado-core-bananads on BananaOS

## The package

`build/package.sh` packs `dist/pixelado-core-bananads-<version>.zip`, everything under `pixelado-core-bananads/`:

| File | What |
|---|---|
| `pixelado-core-bananads` | the engine |
| `run-emu`, `lan.sh`, `status.sh`, `common.sh` | the BananaOS scripts (below) |
| `manifest` | `version`, `binary_sha` |
| `LICENSE`, `NOTICE`, `SOURCE` | the GPL, the credits, and where the source of this exact version is |

An app bundles the folder as it is (Pixelado Party: `cores/ds/`). The engine is always a program of its own: the
app runs it and talks to it over a socket (`party.fd`, the in-game menu; `room.ctl`, the room), never loads it.

The engine never replaces or mounts over `/usr/bin/bananads`, the system's own BananaDS.

It keeps no resume state: it is made for multiplayer. BananaOS's `bananad` asks BananaDS (by its process name) to
save a resume state at *Power off after sleeping*; this engine is not BananaDS, so it gets `SIGTERM` like any other
emulator, and writes the game's own save before quitting.

## Where it writes (run alone through `run-emu`)

| What | Where |
|---|---|
| In-game saves | the system's (`BANANADS_SAVE_DIR` from `/usr/libexec/banana/run-bananads`, usually `/data/saves/nds`) |
| Save states | `/data/states/pixelado-core-bananads` (a state is tied to the engine's build) |
| Settings | `/data/bananads/bananads.conf` (`wifi.local`, `wifi.local_players`…) |
| Logs | `/data/pixelado-party/logs/core-bananads` (the app's own data folder) |

An app that runs the engine itself (Pixelado Party) chooses its own states and logs folders.

## The scripts (`runtime/bananaos/`)

| Script | What |
|---|---|
| `run-emu <rom>` | Plays a game: BananaOS's own launcher with three changes (our binary, our log and states folders). `@firmware` opens the DS menu (DS Download Play) |
| `lan.sh on\|off` | Local multiplayer for the games it opens (`wifi.local`) |
| `status.sh` | `supported=yes\|no` and the state |
| `common.sh` | Shared helpers |

## The in-game menu

With `party.fd` (patch `0008`) the menu is the app's: the engine hands it the screen and does what it asks
(`ROOM-PROTOCOL` §12 in the app's documentation; the lines are in `bananads/src/partyctl.h`). Without a channel, or
when the app doesn't answer in time, the engine opens BananaDS's own menu (`bananads/src/menu.c`).
