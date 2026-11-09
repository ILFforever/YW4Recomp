/* Tests runtime/src/relay.c, the game's side of the relay, against a running relay.py: run once as "host" and
 * once as "station" (YW4_RELAY=127.0.0.1:<port>, the same YW4_ROOM). Build: clang -o test_client.exe
 * tools/relay/test_client.c runtime/src/relay.c runtime/src/host.c -lws2_32 -lwinmm -lpsapi */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/src/host.h"
#include "../../runtime/src/relay.h"

static volatile int netinfos, packets, closed = -1;
static uint8_t last_packet[64];
void ldn_relay_netinfo(const uint8_t *info) { (void)info; netinfos++; }
void ldn_relay_closed(uint8_t reason) { closed = reason; }
void socket_relay_packet(uint32_t dst, uint16_t dport, uint32_t src, uint16_t sport, const uint8_t *data, size_t n) {
    printf("packet %08x:%u -> %08x:%u: %.*s\n", src, sport, dst, dport, (int)n, (const char *)data);
    memcpy(last_packet, data, n < 63 ? n : 63);
    packets++;
}
uint32_t ldn_my_address(void) { return 0; }

static int wait_for(volatile int *v, int want, int ms) {
    for (int i = 0; i < ms / 10 && *v < want; i++) host_sleep_ns(10000000);
    return *v >= want;
}

int main(int argc, char **argv) {
    if (argc < 2 || !relay_enabled()) { fprintf(stderr, "usage: YW4_RELAY=host:port test_client host|station\n"); return 2; }
    uint8_t info[0x480] = {0}, ans[4 + 24 * 0x480];
    uint64_t lcid = 0x010086C00AF7C000ull;
    memcpy(info, &lcid, 8);
    for (int i = 0; i < 16; i++) info[0x10 + i] = (uint8_t)(0xA0 + i);
    info[0x50 + 0x16] = 4;
    if (!strcmp(argv[1], "host")) {
        long n = relay_request(RELAY_CREATE, info, sizeof info, ans, 4 + 0x480, 3000);
        uint32_t ip;
        memcpy(&ip, ans, 4);
        printf("host: create -> %ld bytes, ip %08x\n", n, ip);
        if (n != 4 + 0x480 || ip != 0xA9FE0101) return 1;
        if (!wait_for(&netinfos, 1, 5000)) { printf("host: nobody joined\n"); return 1; }
        printf("host: someone joined\n");
        if (!wait_for(&packets, 1, 5000)) { printf("host: no packet\n"); return 1; }
        relay_send_packet(0xA9FE0102, 49152, 0xA9FE0101, 49152, "pong", 4);
        host_sleep_ns(500000000);
        relay_send(RELAY_LEAVE, NULL, 0);
        host_sleep_ns(300000000);
        printf("host: ok\n");
        return 0;
    }
    long n = -1;
    uint32_t count = 0;
    for (int tries = 0; tries < 30 && count == 0; tries++) {
        n = relay_request(RELAY_SCAN, &lcid, 8, ans, sizeof ans, 3000);
        if (n >= 4) memcpy(&count, ans, 4);
        if (!count) host_sleep_ns(200000000);
    }
    printf("station: scan -> %u room(s)\n", count);
    if (!count) return 1;
    uint8_t req[16 + 0x40] = {0};
    memcpy(req, ans + 4 + 0x10, 16);
    memcpy(req + 16 + 0xC, "Friend", 6);
    uint8_t j[5 + 0x480];
    n = relay_request(RELAY_JOIN, req, sizeof req, j, sizeof j, 3000);
    uint32_t ip;
    memcpy(&ip, j + 1, 4);
    printf("station: join -> ok %u, ip %08x, %u players\n", j[0], ip, j[5 + 0x50 + 0x17]);
    if (n != sizeof j || !j[0] || ip != 0xA9FE0102 || j[5 + 0x50 + 0x17] != 2) return 1;
    relay_send_packet(0xA9FE0101, 49152, ip, 49152, "ping", 4);
    if (!wait_for(&packets, 1, 5000) || memcmp(last_packet, "pong", 4)) { printf("station: no pong\n"); return 1; }
    if (!wait_for(&closed, 0, 5000) || closed != 3) { printf("station: not told the room closed (%d)\n", closed); return 1; }
    printf("station: told the room closed (reason %d)\nstation: ok\n", closed);
    return 0;
}
