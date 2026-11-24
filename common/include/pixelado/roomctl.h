/* Pixelado Party's room, its local socket (ROOM-PROTOCOL §12.2, §12.7): a core started by the room says what its
 * multiplayer session is doing (listening as the host, connected as a guest) and when it ends, and quits when the room
 * ends the game (END, exit code 10). --room-ctl SOCK (or --set room.ctl=SOCK, PIXELADO_ROOM_CTL) turns it on; without
 * it, or if the socket isn't there, nothing changes.
 *
 * No thread: px_room_poll is called from the core's loop (each frame, and while it waits). */
#ifndef PIXELADO_ROOMCTL_H
#define PIXELADO_ROOMCTL_H

#ifdef __cplusplus
extern "C" {
#endif

/* connects (tried for up to ~2 s: party-room is up before the core) and says HELLO. 0: no room */
int px_room_open(const char *path, const char *engine, const char *gid, const char *caps);
int px_room_up(void);
/* SESSION state=listening|connected|failed|lost [reason=] */
void px_room_session(const char *state, const char *reason);
/* reads what the room said; 1 once the room has ended the game (END) */
int px_room_poll(void);
int px_room_ended(void);
/* GAME state=ended reason=quit|end|error, then closes */
void px_room_close(const char *reason);

#ifdef __cplusplus
}
#endif
#endif
