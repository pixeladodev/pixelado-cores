# common: what the cores share with Pixelado Party

Every core runs inside Pixelado Party the same way: Party starts it as a child process, already in the room's game,
hands it a channel, and draws the in-game menu itself. This folder is that contract, and the C code that implements
the core's side of it.

| File | What |
|---|---|
| `include/pixelado/partyctl.h`, `src/partyctl.c` | the channel with Party and the screen handover (PXF1 frames in a memfd) |
| `include/pixelado/roomctl.h`, `src/roomctl.c` | the room's local socket: `SESSION`, `GAME`, `END` |
| `include/pixelado/line.h`, `src/line.c` | the lines both speak (`VERB key=value`, `%XX` escapes) |
| `tests/test_partyctl.c` | a scripted Party and room against the code above: `make -C common test` |

C99 and POSIX, no other dependency. `retro/` (the GBA core, later SNES) builds these sources into its program. The DS
core keeps its own C++ copy for now (`ds/patches/bananads/0008-…`: `partyctl.{h,cpp}`, and `0006-…`: `roomctl.cpp`):
the same lines and timings, written into BananaDS's frontend before this folder existed. Moving it onto `common/`
means a new layer on BananaDS and a new DS release, so it waits for the next DS change.

## How Party runs a core

```
pixelado-core-<sys> [--room host --room-players N | --room join --room-addr IP] [--room-port P] [--player N]
                    [--nickname NAME] [--party-fd FD] [--room-ctl SOCK] [--room-game GID] [core options] ROM
```

- **A process of its own**, never loaded into Party (no `dlopen`, no linking): Party and the cores are separate
  programs that talk through the lines below, so each keeps its own licence.
- **Already in the game:** the room has picked the game, the host and the seats. The host's core listens, the guests'
  cores connect to it; the core says so on the room's socket (`SESSION state=listening|connected|failed|lost`).
- **The channel** is one end of a `socketpair(AF_UNIX, SOCK_STREAM)` on a file descriptor Party names (`--party-fd 3`,
  `--set party.fd=3` for the DS core, or `PIXELADO_PARTY_FD=3`). Without it the core plays as a standalone emulator,
  with its own menu (the DS core) or its own pause card (the retro cores).
- **The screen:** Party lets go of DRM master before starting the core; the core takes it at once.
- **Exit codes:** 0 the player quit, 10 the room ended the game (`END`), 11 no session (couldn't join), 12 the session
  was lost, anything else an error.

## The channel with Party

Lines of `VERB key=value ...`, one per `\n`, values with the bytes under 0x21, `%`, `=` and 0x7F as `%XX`.

| Message | Direction | What |
|---|---|---|
| `HELLO role=engine engine=<sys> proto=1 pid= caps=pause-own,screen resume= gid= title=` | core → Party | who it is, the game's name |
| `OPT key=volume\|brightness type=int value= min= max=`, `OPT key=fps type=bool value=`, `OPTS_END` | core → Party | the settings Party's menu shows |
| `LAN have= mode= state= np= max= lock= addr= msg= ...`, `LANP name= host= me= ping= hold=`, `LAN_END` | core → Party | the multiplayer session, for the menu's card and its multiplayer page |
| `HOLD n= names=` | core → Party | who holds the group's pause (names separated by `\n`) |
| `SCREEN state=released reason=menu\|hold seq=` (+ the frame's fd) | core → Party | the screen let go of |
| `SCREEN state=taken\|failed\|give\|want` | both | the handover (below) |
| `CMD op=set key= value=`, `CMD op=lan mode= players= addr=`, `CMD op=quit`, `CMD op=resume` | Party → core | what the player chose; answered `OK op=…` or `ERR op=… msg=…` |
| `HOLD state=on\|off` | Party → core | the player opened (or closed) Party's menu while Party already had the screen |
| `SOUND id=move\|select\|back` | Party → core | the menu's sounds, on the core's audio device |
| `PING t=` / `PONG t=` | both | every 2 s while Party has the screen |

## The screen handover (Linux, KMS)

1. The player opens the menu, or another player's menu pauses the group. The core stops at the end of the frame,
   writes both panels into a sealed `memfd` (`PXF1`: header of 128 bytes, the number of screens, then each one's
   output, width, height, stride, `XR24` format and offset; little endian), lets go of DRM master and sends
   `SCREEN state=released` with the memfd (`SCM_RIGHTS`) the first time and whenever its size changes.
2. Party takes DRM master, copies the frame, answers `SCREEN state=taken`, draws its menu or its "X is in the menu"
   screen over it and reads the input.
3. To give the screen back Party lets go of master and sends `SCREEN state=give`; the core takes master, repaints,
   answers `SCREEN state=taken` and resumes.
4. While another player holds the group the core keeps the screen with Party; when nobody holds it any more it sends
   `SCREEN state=want` and Party gives it back.
5. No `taken` within 1 s, Party silent for 6 s, or the channel closed: the core takes the screen back and opens its
   own menu. A menu button held 3 s while Party has the screen quits the game (Party may have hung with it).

On Android there is no DRM: the core draws into a `Surface` Party lends it, from a process of its own, with the same
lines over a socket.

## The group's pause

Every core pauses the whole group itself (`caps=pause-own`): Party only draws. The DS core does it with melonDS's
local wireless (`Cmd_Hold`); the retro cores with a `HOLD` message on their own link between the handhelds
(`retro/README.md`), so the link never times out while one side is paused: no frame runs anywhere until every menu is
closed.

## The room's socket

`--room-ctl SOCK` (`--set room.ctl=` for the DS core): the core connects to `party-room`'s socket and says
`HELLO role=engine engine=<sys> proto=1 gid= caps=pause-own`, then `SESSION state=listening` (the host, once it
listens), `SESSION state=connected` (a guest, once in the host's session), `SESSION state=failed|lost reason=`, and
`GAME state=ended reason=quit|end|error` as it exits. `END` from the room ends the game (exit code 10).

## The GBA core in Pixelado Party (integration)

What Party needs to run `pixelado-core-gba` (`gba/`), the same way it runs the DS core. Party-side changes, nothing
in the core:

**1. The package (`build/fetch-cores.sh`, `manifest`).** A second core in the app's package, as the DS one:

```
cores/gba/pixelado-core-gba   the engine (0755)
cores/gba/manifest            version=, binary_sha=
cores/gba/LICENSE, NOTICE     GPL-3 and the credits (gpSP is GPL-2.0-or-later)
cores/gba/SOURCE              "pixelado-core-gba <ver>" and the tree: .../pixelado-cores/tree/gba-v<ver>
```

Party's `manifest`: `core_gba_repo=pixeladodev/pixelado-cores`, `core_gba_version=0.1.0`,
`core_gba_sha=<gba/manifest's binary_sha>`. `fetch-cores.sh` copies `../pixelado-cores/gba/dist/pixelado-core-gba`
when it is there (with `gba/manifest`, `LICENSE`, `NOTICE`, and writes `SOURCE`), else downloads the release asset
`pixelado-core-gba-<ver>.zip` (tag `gba-v<ver>`) and checks the binary against `core_gba_sha`. No scripts go with it:
`run-engine.sh` runs the binary itself. `common.sh`: `CORE_GBA=$APP/cores/gba/pixelado-core-gba`.

**2. Settings > Emuladores and the library.** GBA is "included" when `cores/gba/pixelado-core-gba` is in the package,
on by default; off, it leaves the L2/R2 cycle as DS does. The room's `HELLO`/`MEMBER` say `emu=nds,gba` and
`eng.gba=<version>`. The GBA catalogue: `.gba` files and `.zip` files with a `.gba` inside, from BananaOS's GBA ROM
folder; keys `gba:<CRC32 of the ROM>` (ROOM-PROTOCOL §10.1; the `gba:?<file>` placeholder keys go). A GBA room seats
2 to 4 (`max=4`); no Download Play or multicart modes.

**3. The room (`party-room`).** `PICK`/`ROOM`/`BEACON` carry `sys=gba`. `LAUNCH` for a GBA game: `gp=7067` (the GBA
link's port, ROOM-PROTOCOL §4: 7067–7069 are retro/'s), `player=` each seat's index in the game (0 the host, then
1, 2, 3 in seat order: the core asks the host for that index as its link id), `nplayers=`. The host's core says
`SESSION state=listening` as soon as it listens (no waiting for guests); a guest's core tries the host for 15 s, then
says `connected` or `failed` (and exits 11). `launch.env` gains `PLAYER` (already written) and `ROOM_PORT=7067`.
Optional, later: `p.gba.serial=auto|rfu|mul_poke|mul_aw1|mul_aw2` in the allowed list (§14), passed to every
member as `--option gpsp_serial=<value>` (it must be the same on all), to force a link mode (e.g. FireRed with
Ruby over the cable protocol).

**4. `run-engine.sh` for `SYS=gba`** (and a `.gba`/`.zip` played alone from the library):

```sh
EMU=$CORE_GBA SAVES=/data/saves/gba            # BananaOS's one GBA save folder (RetroArch, BananaFront: <game>.srm)
[ -x /usr/libexec/banana/saves-flat ] && /usr/libexec/banana/saves-flat   # (a card's per-core folders, as BananaFront)
set -- --save-dir "$SAVES" --system-dir /data/bios --log "$LOGF"         # (gba_bios.bin there is used; else gpSP's own)
[ "$ROLE" = host ] && set -- "$@" --room host --room-players "$NPLAYERS" --room-port "${PORT:-7067}"
[ "$ROLE" = join ] && set -- "$@" --room join --room-addr "$ADDR" --room-port "${PORT:-7067}" --player "$PLAYER"
[ -n "$NICK" ] && set -- "$@" --nickname "$NICK"
[ -n "$CTL" ] && set -- "$@" --room-ctl "$CTL" --room-game "$GID"
case ${PIXELADO_PARTY_FD:-} in [0-9]*) set -- "$@" --party-fd "$PIXELADO_PARTY_FD" ;; esac
# a .zip: unpacked to a temporary folder as for DS (the .gba inside), and the save keeps the archive's name:
set -- "$@" --save-name "$(basename "${ORIG_ROM%.*}")" --title "<the library's title>"
export BANANADS_TOP=${BANANA_TOP:-DSI-2}       # the top panel's connector: the game goes there, the card below
"$EMU" "$@" "$ROM" &                           # then the same pid, game.pid, engine.pid and exit code handling
```

The DS-only parts stay DS-only: `nds-saves`, `core-ds.env`, the `BANANADS_*` settings, `mp_lan_set`, `@firmware`.
`banana-perf` with `[gba]`'s `perf` from `systems.conf` (BananaOS's own GBA profile) instead of `superdrastic`.

**5. During the game.** Nothing new: `HELLO engine=gba caps=pause-own,screen`, the same handover, `OPT` volume,
brightness and FPS (the FPS goes on the card, the second panel). The `LAN` lines describe the link (`mode=1` host,
`mode=2` guest, `state=2` ready, the players with their pings and who holds the pause) with `lock=1`: the
multiplayer page shows the session and offers no changes (`CMD op=lan` is answered `ERR … msg=unsupported`).
`SOUND` plays Party's menu sounds on the core's ALSA device. When the host quits, the guests' link drops (their game
shows its own link error) and the room sends `END` as for DS.

**6. On a computer** (Party's preview, tests): `--video null --audio null --input none` runs the core headless.
