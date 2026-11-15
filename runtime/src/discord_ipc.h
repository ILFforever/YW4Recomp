/* Discord's local IPC (decision 0077): the few messages Rich Presence needs, over the socket the Discord app
 * opens on this PC. No library: a frame is an 8-byte header (op, length, both little-endian u32) and that many
 * bytes of JSON. Windows: the named pipe \\.\pipe\discord-ipc-N; Linux: the socket discord-ipc-N in
 * $XDG_RUNTIME_DIR, $TMPDIR, $TMP, $TEMP or /tmp (also their Flatpak and Snap subfolders); N from 0 to 9.
 * Nothing here knows about the game (discord.c does), so tests can drive it against a stand-in server. */
#ifndef YW4_DISCORD_IPC_H
#define YW4_DISCORD_IPC_H

#include <stddef.h>
#include <stdint.h>

enum { DISCORD_OP_HANDSHAKE = 0, DISCORD_OP_FRAME = 1, DISCORD_OP_CLOSE = 2, DISCORD_OP_PING = 3, DISCORD_OP_PONG = 4 };

typedef struct {
    intptr_t h;  /* Windows: the pipe's HANDLE; Linux: the socket's descriptor */
    int open;
} DiscordIpc;

/* What the profile shows; NULL or "" leaves a field out. start: when the game started (Unix seconds), 0 none. */
typedef struct {
    const char *details, *state, *large_image, *large_text;
    int64_t start;
} DiscordActivity;

/* Connects to the first Discord socket that answers, or to `path` alone when it isn't NULL (tests). 0: connected. */
int discord_ipc_connect(DiscordIpc *c, const char *path);
void discord_ipc_close(DiscordIpc *c);
/* One frame out. 0: sent; -1: the connection is gone (closed). */
int discord_ipc_send(DiscordIpc *c, uint32_t op, const char *json);
/* One frame in, waiting at most wait_ms for it to start: 1 a frame (its JSON in buf, cut to n - 1 bytes and ended
 * with 0), 0 none yet, -1 the connection is gone (closed). */
int discord_ipc_recv(DiscordIpc *c, uint32_t *op, char *buf, size_t n, int wait_ms);

/* The frame for op and json in out (header + JSON): its length, or 0 if it doesn't fit in cap. */
size_t discord_frame(uint8_t *out, size_t cap, uint32_t op, const char *json);
/* The handshake's JSON: {"v":1,"client_id":"<id>"}. Its length, or 0 if it doesn't fit. */
size_t discord_handshake_json(char *out, size_t n, const char *client_id);
/* SET_ACTIVITY's JSON for this process (pid) with the activity, or clearing it (a NULL). Its length, or 0. */
size_t discord_activity_json(char *out, size_t n, long pid, const DiscordActivity *a, unsigned nonce);

#endif
