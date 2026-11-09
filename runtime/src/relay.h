/* The game's side of the relay (tools/relay/relay.py, decision 0057): one TCP connection that carries local play
 * over the internet. ldn.c makes and finds rooms through it, socket.c sends and receives the players' packets. */
#ifndef RELAY_H
#define RELAY_H

#include <stddef.h>
#include <stdint.h>

enum { RELAY_HELLO = 1, RELAY_CREATE, RELAY_UPDATE, RELAY_SCAN, RELAY_JOIN, RELAY_NETINFO, RELAY_LEAVE,
       RELAY_PACKET, RELAY_REJECT };

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

#endif
