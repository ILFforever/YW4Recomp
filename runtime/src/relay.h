/* The game's side of the relay (tools/relay/relay.py, decision 0082): one TCP connection that carries local play
 * over the internet. ldn.c makes and finds rooms through it, socket.c sends and receives the players' packets. */
#ifndef RELAY_H
#define RELAY_H

#include <stddef.h>
#include <stdint.h>

#include "yw4cpu.h"

enum { RELAY_HELLO = 1, RELAY_CREATE, RELAY_UPDATE, RELAY_SCAN, RELAY_JOIN, RELAY_NETINFO, RELAY_LEAVE,
       RELAY_PACKET, RELAY_REJECT, RELAY_PING };

/* YW4_RELAY (host:port) is set: local play goes through the relay; else a Switch alone in a room */
int relay_enabled(void);
/* Sends a request and waits for its answer (same type) into out: the answer's size, or -1 (no relay, timeout) */
long relay_request(uint8_t type, const void *p, size_t n, uint8_t *out, size_t cap, int timeout_ms);
/* Sends without an answer: 0, or -1 */
int relay_send(uint8_t type, const void *p, size_t n);
/* A UDP packet to another player (addresses in host order) */
int relay_send_packet(uint32_t dst, uint16_t dport, uint32_t src, uint16_t sport, const void *data, size_t n);

/* what the relay tells, called on its reader thread */
void ldn_relay_netinfo(const uint8_t *info);           /* the room changed: its NetworkInfo (0x480) */
void ldn_relay_closed(uint8_t reason);                 /* the room is gone for us: a DisconnectReason */
void socket_relay_packet(uint32_t dst, uint16_t dport, uint32_t src, uint16_t sport, const uint8_t *data, size_t n);
uint32_t ldn_my_address(void);                         /* our address in the room, 0 when in none */

/* The room we host or joined, for Discord status (discord_game.c): 1 and its players now, its most players and
 * the game's scene number for it (a trade: scene 1, 2 players at most); 0 in none. Any thread. */
int ldn_room(int *players, int *max_players, int *scene);
/* The game is in local play (Ark Swap, Blasters): from its ldn Initialize to its Finalize. The overlay's Online tab
 * locks its server and room settings meanwhile. Any thread. */
int ldn_active(void);
/* Hosting (from OpenAccessPoint) or joined a room: the overlay's Online tab locks its server and room settings only
 * then (owner, 2026-11-20: the F4 hint pointed at a locked tab on the start screen and the room list). Any thread. */
int ldn_in_room(void);
/* A menu item was picked (OnDecideFocus, mousemenu.c): Blasters' top list's next row read may be Online (ldn.c) */
void ldn_menu_decided(void);
/* The Online tab's server or room settings changed: an open connection is closed, so the next request (the room
 * list's Update List) connects with them. Nothing while in a room (ldn_in_room): they reach the next connection. */
void relay_settings_changed(void);

/* The menus' Online / Local choice (yw4_hook_net_mode, ldn.c): 1 Online (our server), 0 Local (YW4_RELAY or alone),
 * for which activity (its public / private room settings). An open connection that no longer fits is closed; the
 * next request connects to the chosen server with the chosen room code. */
enum { RELAY_SWAP, RELAY_BLASTERS };
void relay_choose(int online, int activity);

/* The overlay's Online tab: each server's ping, measured on a thread (HELLO -> WELCOME, best of three); while it runs
 * a second start does nothing. relay_ping_ms: milliseconds, -1 still measuring, -2 no answer. */
void relay_ping_start(const char *const *addrs, int n);
int relay_ping_ms(int i);

/* Online-mode recon (ssl.c): YW4_NET_TRACE=1 prints the guest call stack at `what`; YW4_FAKE_INTERNET=1 lets the
 * game's internet request through (tests only) */
void yw4_net_trace(const CPUState *c, const char *what);
int yw4_fake_internet(void);

#endif
