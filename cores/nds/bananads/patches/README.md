# The patch layers

## `bananads/`

Our changes to BananaDS's own code (`bananads/src`, its CMake file and its docs), as plain unified diffs
against the SuperDrastic tree at `upstream.lock`. Applied in name order with `patch -p1` from the tree's
root. Text before the first `diff --git` line is the patch's description.

| Patch | What |
|---|---|
| `0001-LAN-multiplayer-…` | Local multiplayer over the network, the Party session, DS Download Play from the firmware, the MAC from `/etc/machine-id`, saved WFC settings, Auto / Auto Join, the in-game menu's row; the three patches BananaOS applies to BananaDS at build time |
| `0002-Party-room-…` | The room survives DS Download Play, rejoin after a network drop, the UDP latency echo (port 7065) |
| `0003-Menu-…` | What Pixelado Party's menu needs from BananaDS's own menu (`menu.c`, the engine's menu without Party): the volume, brightness and FPS counter set as `menu.c` sets them (`bmenu_ext_*`), the game's name (the game chosen in a room; the banner's title for a file name that says nothing), BananaOS's Inter as a fallback font |
| `0004-Room-pause-…` | The menu always pauses; in a local wireless session the whole room (melonDS `0013`). The wait screen ("X is in the menu") is Party's |
| `0005-Room-engine-…` | Launched by Pixelado Party's own room: `room.ctl` (its local socket: listening / connected, GAME, END quits with 10), `wifi.local_join_retry_s`, `firmware.color`, `firmware.profile` (the profile in the user's firmware copy), the nickname as UTF-8 |
| `0006-kms-lowlat-…` | The newest frame's vblank on the deadlines' grid (no steady 30 fps when the panels' phase is far apart) |
| `0007-Party-menu-…` | `party.fd`: the screen handed to Pixelado Party for the in-game menu and the room's wait screen (memfd `PXF1`, DRM master), the settings and the quit back as `CMD` lines; without a channel, or no answer in time, BananaDS's menu |
| `0008-Room-pause-…` | A client waiting for its host pauses where it waits (melonDS `0014`) |

## `melonds/`

Our patches to melonDS, numbered on their own from 0001. The build applies BananaDS's own melonDS patches
first (all of them, `bananads/patches/melonds/` in the SuperDrastic tree, in name order), then these, in
name order. A BananaDS patch and one of ours can share a number: they never mix.

| Patch | What |
|---|---|
| 0001 | Wifi: local wireless play as on the hardware: the radio after the beacon window, a transceiver turned off is no longer a MP client, a client whose host went quiet keeps running |
| 0002 | LAN: the session: the host waits only for its DS group, a child repeats its auth/association request, `LAN::GetHostPing` |
| 0003 | LAN: the transport: the MP receive timeout (`BANANADS_LAN_TIMEOUT_MS`), DSCP 46, TDLS (`BANANADS_LAN_TDLS`), ENet's throttle off |
| 0004 | Diagnostic: the reply delay and the frame timing |
| 0005 | Diagnostic: would an early MP reply help |
| 0006 | Early reply: a client's reply sent as the CMD arrives (only a frame; a default or blank at the end) |
| 0007 | DS Download Play host takes MP replies as they come (an experiment, off: `BANANADS_LAN_DLPLAY_PIPELINE`) |
| 0008 | Diagnostic: the room a host's CMD has and its replies (`[diag] LAN cmd`) |
| 0009 | The CMD hold: a host's CMD held within its window and sent at once (`BANANADS_LAN_CMD_HOLD_US`) |
| 0010 | A missing MP reply: the CMD sent again within a grace, duplicates answered again, the ACK sent again (`BANANADS_LAN_GRACE_MS`); test-only drop and weak-radio delay (`BANANADS_LAN_DROP`, `BANANADS_LAN_DELAY_MS`) |
| 0011 | The ACK sent again goes in one datagram with the CMD (one sendto and one radio frame less per exchange) |
| 0012 | The lazy Wi-Fi timer: the idle 8 µs ticks folded into the next event (`BANANADS_WIFI_LAZY`, on in Pixelado Party; Mario Party DS in multiplayer ~46 → ~49 fps) |
| 0013 | The room's pause: a player in its menu holds the game for everyone (`LAN::SetHold`, the host's word) |
| 0014 | The room's pause: a client waiting for its host pauses there (`Platform::MP_HoldWait`) |

0004, 0005, 0008 and part of 0009 are diagnostics for the log, and 0007 an experiment that is off. They
stay while the freeze under a weak radio is being looked into; removing them is the next change (and
gives a new `binary_sha`).

## Changing a patch, or adding one

Don't edit the patch files by hand. `dev/tree.sh` makes the sources editable (two git repositories, the
pinned upstream with one commit per patch, in `.cache/dev` or `PIXELADO_CORE_DEV_TREE`); edit and commit there
(a fixup of a patch's commit changes that patch, a new commit is a new patch), then `dev/export.sh`
writes this folder back and checks that the patches, applied from scratch as the build does, give the
same tree.

## Updating BananaDS

Change `superdrastic` in `upstream.lock`, run `dev/tree.sh -f` (rebase what no longer applies) or
`build/build.sh` with `PIXELADO_CORE_DRAFT=1`. If a patch no longer
applies, rebase it on the new tree, test on the consoles, then set the new `binary_sha` in `manifest`.
