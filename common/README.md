# common: what the cores share with Pixelado Party

Reserved for the code every core needs to run inside Pixelado Party. Today it lives in the DS core's patches; it moves
here when a second core (GBA) needs it.

## The channel with Party

Party runs a core as its child, with one end of a `socketpair(AF_UNIX, SOCK_STREAM)` on a file descriptor it names
(`--set party.fd=3`, or `PIXELADO_PARTY_FD=3`). Lines of `KEY key=value ...`, one message each. A core without the
channel plays as a standalone emulator, with its own menu.

| Message | Direction | What |
|---|---|---|
| `HELLO role=engine engine=ds proto=1 pid= caps=pause-own,screen resume= title=` | core → Party | who it is, the game's name |
| `OPT key= type=int\|bool value [min max]`, `OPTS_END` | core → Party | the settings Party's menu shows (volume, brightness, FPS counter) |
| `SCREEN state=released\|want\|taken\|give\|failed reason= seq=` | both | the screen handed over (below) |
| `CMD op=resume\|quit\|set\|lan ...` | Party → core | what the player chose; answered `OK op` or `ERR op msg` |
| `HOLD state=on\|off`, `HOLD n= names=` | both | the room's pause: who holds it |
| `LAN ...`, `LANP`, `LANH`, `LAN_END` | core → Party | the local wireless session, for Party's multiplayer page |
| `SOUND id=move\|select\|back` | Party → core | the menu's sounds, on the core's audio device |
| `PING` / `PONG` | both | every 2 s while one waits for the other |

## The screen handover (Linux, KMS)

1. The player opens the menu, or another player's menu pauses the room. The core pauses, waits for its last page flip,
   writes both panels into a sealed `memfd` and sends it with `SCM_RIGHTS` in `SCREEN state=released`, then drops DRM
   master.
2. Party takes DRM master, copies the frame (header `PXF1`: the number of screens, then each one's output, width,
   height, stride, `XR24` format and offset), draws its menu or its wait screen over it and reads the input.
3. To give it back Party drops master and sends `SCREEN state=give`; the core takes master, repaints, sends
   `SCREEN state=taken` and resumes.
4. No answer within 1 s, Party silent for 6 s, or the channel closed: the core takes the screen back and opens its own
   menu.

On Android there is no DRM: the core draws into a `Surface` Party lends it, from a process of its own, with the same
lines over a socket.

## Where it is today

- `ds/patches/bananads/0008-Party-menu-…`: `bananads/src/partyctl.h` and `partyctl.cpp` (the channel and the handover),
  and the frontend's side in `frontend_kms.cpp`.
- `ds/patches/bananads/0006-Room-engine-…`: `roomctl.cpp`, the room's own socket (`room.ctl`).
