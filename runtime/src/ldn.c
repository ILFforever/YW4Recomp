/* nn::ldn, the Switch's local wireless play, native (decision 0057).
 *
 * The sdk's nn::ldn functions send IPC to ldn:u; here they are native overrides (recomp/overrides.toml) on the
 * structs Switchbrew documents ("LDN services"): NetworkInfo 0x480, NetworkConfig 0x20, SecurityConfig 0x44,
 * SecurityParameter 0x20, UserConfig 0x30, ScanFilter 0x60. Addresses (Ipv4Address) are u32 in host order, as
 * nn::ldn::MakeIpv4Address makes them.
 *
 * Phase 0 (now): a Switch alone in a room. Scan finds nobody, Connect fails as with no network in reach, a host's
 * network holds only itself (169.254.1.1). Every call is logged with its arguments ("yw4: ldn: ..."), so one try
 * of the game's local play shows what Pia asks for. The relay (phase 1) fills in the other players.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host.h"
#include "kernel.h"
#include "yw4load.h"

/* nn::ldn result codes: module 203 */
#define LDN_RESULT(desc) (203u | ((uint32_t)(desc) << 9))
#define R_OK 0u
#define R_INVALID_STATE LDN_RESULT(32)
#define R_CONNECTION_FAILED LDN_RESULT(64)
#define R_NOT_FOUND LDN_RESULT(48)

enum { ST_NONE, ST_INITIALIZED, ST_ACCESS_POINT, ST_AP_CREATED, ST_STATION, ST_STATION_CONNECTED, ST_ERROR };
static const char *const state_names[] = {"None", "Initialized", "AccessPoint", "AccessPointCreated", "Station",
                                          "StationConnected", "Error"};

#define HOST_ADDR 0xA9FE0101u  /* 169.254.1.1: the host's address in its own network */
#define SUBNET 0xFFFFFF00u

static HostLock lock = HOST_LOCK_INIT;
static uint32_t state;
static int16_t disconnect_reason;
static uint8_t network[0x480];  /* NetworkInfo of the network we host or joined */
static uint8_t net_config[0x20];
static uint8_t security_param[0x20];
static uint8_t accept_policy;
static KEvent *state_event;

static int verbose(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("YW4_LDN_LOG");
        v = !(e && *e == '0');  /* on by default while this is being built */
    }
    return v;
}
#define LOG(...) do { if (verbose()) fprintf(stderr, "yw4: ldn: " __VA_ARGS__); } while (0)

static void set_state(uint32_t s) {
    LOG("state %s -> %s\n", state_names[state < 7 ? state : 6], state_names[s < 7 ? s : 6]);
    state = s;
    if (state_event) yw4_event_signal_from_host(state_event);
}

static void hex_line(const char *what, const uint8_t *p, size_t n) {
    if (!verbose()) return;
    fprintf(stderr, "yw4: ldn:   %s (%zu bytes):", what, n);
    for (size_t i = 0; i < n && i < 0x180; i++) fprintf(stderr, "%s%02x", i % 32 ? "" : "\n      ", p[i]);
    fprintf(stderr, "\n");
}

static void log_network_config(const uint8_t *nc) {
    uint64_t lcid;
    uint16_t scene, ch, ver;
    memcpy(&lcid, nc, 8);
    memcpy(&scene, nc + 0xA, 2);
    memcpy(&ch, nc + 0x10, 2);
    memcpy(&ver, nc + 0x14, 2);
    LOG("  network config: local communication id %016" PRIx64 ", scene %u, channel %u, max nodes %u, version %u\n",
        lcid, scene, ch, nc[0x12], ver);
}
static void log_security(const uint8_t *sc) {
    uint16_t mode, len;
    memcpy(&mode, sc, 2);
    memcpy(&len, sc + 2, 2);
    LOG("  security: mode %u, passphrase %u bytes\n", mode, len);
}
static void log_user(const uint8_t *uc) {
    char name[0x22] = {0};
    memcpy(name, uc, 0x21);
    LOG("  user: \"%s\"\n", name);
}

/* our own node in the network info: 0x50 Ldn + 0x18 nodes, 0x40 each */
static void put_node(int i, uint32_t addr, const uint8_t *user, uint16_t version) {
    uint8_t *n = network + 0x50 + 0x18 + 0x40 * i;
    memset(n, 0, 0x40);
    memcpy(n, &addr, 4);
    uint8_t mac[6] = {0x02, 0x00, 0x59, 0x57, 0x34, (uint8_t)(i + 1)};  /* locally administered, "YW4" */
    memcpy(n + 4, mac, 6);
    n[0xA] = (uint8_t)i;
    n[0xB] = 1;
    memcpy(n + 0xC, user, 0x20);
    memcpy(n + 0x2E, &version, 2);
}

static void make_network(const uint8_t *nc, const uint8_t *sc, const uint8_t *uc) {
    memset(network, 0, sizeof network);
    memcpy(net_config, nc, 0x20);
    memcpy(network, nc, 0x10);  /* IntentId */
    for (int i = 0; i < 16; i++) network[0x10 + i] = (uint8_t)rand();  /* SessionId */
    uint8_t *common = network + 0x20;
    uint8_t bssid[6] = {0x02, 0x00, 0x59, 0x57, 0x34, 0x01};
    memcpy(common, bssid, 6);
    static const char ssid[] = "yw4recomp";
    common[6] = (uint8_t)(sizeof ssid - 1);
    memcpy(common + 7, ssid, sizeof ssid - 1);
    memcpy(common + 0x28, nc + 0x10, 2);  /* channel */
    common[0x2A] = 3;                     /* link level: strong */
    common[0x2B] = 2;                     /* network type: ldn */
    uint8_t *ldn = network + 0x50;
    for (int i = 0; i < 16; i++) ldn[i] = (uint8_t)rand();  /* ServerRandom */
    memcpy(ldn + 0x10, sc, 2);                              /* security mode */
    ldn[0x12] = accept_policy;
    ldn[0x13] = 3;
    ldn[0x16] = nc[0x12] ? nc[0x12] : 8;  /* node count max */
    ldn[0x17] = 1;
    uint16_t ver;
    memcpy(&ver, nc + 0x14, 2);
    put_node(0, HOST_ADDR, uc, ver);
    memcpy(security_param, ldn, 16);
    memcpy(security_param + 16, network + 0x10, 16);
}

/* ---- the overrides: arguments in x0.., Result in x0 */

void yw4_ldn_Initialize(CPUState *c) {
    host_lock(&lock);
    LOG("Initialize\n");
    if (state == ST_NONE) set_state(ST_INITIALIZED);
    host_unlock(&lock);
    c->x[0] = R_OK;
}

void yw4_ldn_Finalize(CPUState *c) {
    host_lock(&lock);
    LOG("Finalize\n");
    set_state(ST_NONE);
    host_unlock(&lock);
    (void)c;
}

void yw4_ldn_GetState(CPUState *c) {
    c->x[0] = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
}

/* AttachStateChangeEvent(os::SystemEventType *): as vi's vsync event, a host-signalled event attached to the
 * game's SystemEventType through nn::os::AttachReadableHandleToSystemEvent (auto clear) */
void yw4_ldn_AttachStateChangeEvent(CPUState *c) {
    uint32_t handle;
    LOG("AttachStateChangeEvent\n");
    yw4_kernel_lock();
    if (!state_event) {
        state_event = yw4_event_new(&handle);
        state_event->host_signalled = 1;
    } else {
        handle = yw4_handle_new(&state_event->obj);
    }
    yw4_kernel_unlock();
    uint64_t lr = c->x[30];
    c->x[1] = handle, c->x[2] = 0, c->x[3] = 1;  /* x0 is already the event */
    yw4_resolve(yw4_guest_symbol("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"))(c);
    c->x[30] = lr;
}

static void transition(CPUState *c, const char *name, uint32_t from_a, uint32_t from_b, uint32_t to) {
    host_lock(&lock);
    LOG("%s (in %s)\n", name, state_names[state < 7 ? state : 6]);
    if (state == from_a || state == from_b) {
        set_state(to);
        c->x[0] = R_OK;
    } else {
        c->x[0] = R_INVALID_STATE;
    }
    host_unlock(&lock);
}

void yw4_ldn_OpenAccessPoint(CPUState *c) { transition(c, "OpenAccessPoint", ST_INITIALIZED, ST_INITIALIZED, ST_ACCESS_POINT); }
void yw4_ldn_CloseAccessPoint(CPUState *c) { transition(c, "CloseAccessPoint", ST_ACCESS_POINT, ST_AP_CREATED, ST_INITIALIZED); }
void yw4_ldn_DestroyNetwork(CPUState *c) { transition(c, "DestroyNetwork", ST_AP_CREATED, ST_AP_CREATED, ST_ACCESS_POINT); }
void yw4_ldn_OpenStation(CPUState *c) { transition(c, "OpenStation", ST_INITIALIZED, ST_INITIALIZED, ST_STATION); }
void yw4_ldn_CloseStation(CPUState *c) { transition(c, "CloseStation", ST_STATION, ST_STATION_CONNECTED, ST_INITIALIZED); }
void yw4_ldn_Disconnect(CPUState *c) { transition(c, "Disconnect", ST_STATION_CONNECTED, ST_STATION_CONNECTED, ST_STATION); }

/* CreateNetwork(const NetworkConfig&, const SecurityConfig&, const UserConfig&) */
void yw4_ldn_CreateNetwork(CPUState *c) {
    const uint8_t *nc = GPTR(c->x[0]), *sc = GPTR(c->x[1]), *uc = GPTR(c->x[2]);
    host_lock(&lock);
    LOG("CreateNetwork\n");
    log_network_config(nc);
    log_security(sc);
    log_user(uc);
    if (state != ST_ACCESS_POINT) {
        c->x[0] = R_INVALID_STATE;
    } else {
        make_network(nc, sc, uc);
        set_state(ST_AP_CREATED);
        c->x[0] = R_OK;
    }
    host_unlock(&lock);
}

/* CreateNetworkPrivate(NetworkConfig&, SecurityConfig&, SecurityParameter&, UserConfig&, int count, AddressEntry*) */
void yw4_ldn_CreateNetworkPrivate(CPUState *c) {
    const uint8_t *nc = GPTR(c->x[0]), *sc = GPTR(c->x[1]), *sp = GPTR(c->x[2]), *uc = GPTR(c->x[3]);
    host_lock(&lock);
    LOG("CreateNetworkPrivate (%d address entries)\n", (int)c->x[4]);
    log_network_config(nc);
    log_security(sc);
    log_user(uc);
    if (state != ST_ACCESS_POINT) {
        c->x[0] = R_INVALID_STATE;
    } else {
        make_network(nc, sc, uc);
        memcpy(network + 0x50, sp, 16);       /* the given ServerRandom */
        memcpy(network + 0x10, sp + 16, 16);  /* and SessionId */
        memcpy(security_param, sp, 0x20);
        set_state(ST_AP_CREATED);
        c->x[0] = R_OK;
    }
    host_unlock(&lock);
}

/* SetAdvertiseData(const void *, size_t) */
void yw4_ldn_SetAdvertiseData(CPUState *c) {
    size_t n = c->x[1];
    host_lock(&lock);
    LOG("SetAdvertiseData (%zu bytes)\n", n);
    if (n > 0x180) {
        c->x[0] = LDN_RESULT(33);
    } else {
        uint16_t n16 = (uint16_t)n;
        memcpy(network + 0x50 + 0x21A, &n16, 2);
        memset(network + 0x50 + 0x21C, 0, 0x180);
        if (n) memcpy(network + 0x50 + 0x21C, GPTR(c->x[0]), n);
        hex_line("advertise data", network + 0x50 + 0x21C, n);
        c->x[0] = R_OK;
    }
    host_unlock(&lock);
}

void yw4_ldn_SetStationAcceptPolicy(CPUState *c) {
    accept_policy = (uint8_t)c->x[0];
    network[0x50 + 0x12] = accept_policy;
    LOG("SetStationAcceptPolicy %u\n", accept_policy);
    c->x[0] = R_OK;
}

void yw4_ldn_Reject(CPUState *c) {
    uint32_t a = (uint32_t)c->x[0];
    LOG("Reject %u.%u.%u.%u\n", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    c->x[0] = R_OK;
}

/* Scan(NetworkInfo *out, int *count, int capacity, const ScanFilter&, int channel): nobody around (phase 0) */
static void scan(CPUState *c, const char *name) {
    const uint8_t *f = GPTR(c->x[3]);
    uint64_t lcid;
    uint32_t flag;
    memcpy(&lcid, f, 8);
    memcpy(&flag, f + 0x5C, 4);
    LOG("%s (room for %d, channel %d, filter flags %#x, local communication id %016" PRIx64 ")\n", name,
        (int)c->x[2], (int)c->x[4], flag, lcid);
    int32_t zero = 0;
    memcpy(GPTR(c->x[1]), &zero, 4);
    c->x[0] = state == ST_STATION || state == ST_STATION_CONNECTED || state == ST_AP_CREATED ? R_OK : R_INVALID_STATE;
}
void yw4_ldn_Scan(CPUState *c) { scan(c, "Scan"); }
void yw4_ldn_ScanPrivate(CPUState *c) { scan(c, "ScanPrivate"); }

/* Connect(const NetworkInfo&, SecurityConfig&, UserConfig&, int version, ConnectOption): nobody to join yet */
void yw4_ldn_Connect(CPUState *c) {
    LOG("Connect (version %d, option %#x)\n", (int)c->x[3], (unsigned)c->x[4]);
    log_security(GPTR(c->x[1]));
    log_user(GPTR(c->x[2]));
    c->x[0] = R_CONNECTION_FAILED;
}
/* ConnectPrivate(NetworkConfig&, SecurityConfig&, SecurityParameter&, UserConfig&, int version, ConnectOption) */
void yw4_ldn_ConnectPrivate(CPUState *c) {
    LOG("ConnectPrivate (version %d, option %#x)\n", (int)c->x[4], (unsigned)c->x[5]);
    log_network_config(GPTR(c->x[0]));
    log_security(GPTR(c->x[1]));
    log_user(GPTR(c->x[3]));
    c->x[0] = R_CONNECTION_FAILED;
}

void yw4_ldn_GetNetworkInfo(CPUState *c) {
    host_lock(&lock);
    if (state == ST_AP_CREATED || state == ST_STATION_CONNECTED) {
        memcpy(GPTR(c->x[0]), network, sizeof network);
        c->x[0] = R_OK;
    } else {
        c->x[0] = R_INVALID_STATE;
    }
    host_unlock(&lock);
}

void yw4_ldn_GetNetworkConfig(CPUState *c) {
    memcpy(GPTR(c->x[0]), net_config, sizeof net_config);
    c->x[0] = state == ST_AP_CREATED || state == ST_STATION_CONNECTED ? R_OK : R_INVALID_STATE;
}

void yw4_ldn_GetSecurityParameter(CPUState *c) {
    memcpy(GPTR(c->x[0]), security_param, sizeof security_param);
    c->x[0] = state == ST_AP_CREATED || state == ST_STATION_CONNECTED ? R_OK : R_INVALID_STATE;
}

/* GetIpv4Address(Ipv4Address *, SubnetMask *) */
void yw4_ldn_GetIpv4Address(CPUState *c) {
    uint32_t a = HOST_ADDR, m = SUBNET;
    memcpy(GPTR(c->x[0]), &a, 4);
    memcpy(GPTR(c->x[1]), &m, 4);
    LOG("GetIpv4Address -> 169.254.1.1/24\n");
    c->x[0] = state == ST_AP_CREATED || state == ST_STATION_CONNECTED ? R_OK : R_INVALID_STATE;
}

void yw4_ldn_GetDisconnectReason(CPUState *c) {
    LOG("GetDisconnectReason -> %d\n", disconnect_reason);
    c->x[0] = (uint64_t)(int64_t)disconnect_reason;
}

/* ---- nn::nifm: the game asks for a network before local play too. A local network request (SetLocalNetworkMode
 * true) is always granted, as on a Switch with its wireless on; an internet one never is (NEX is gone). */

static int local_mode;

void yw4_nifm_Initialize(CPUState *c) { LOG("nifm Initialize\n"); c->x[0] = R_OK; }
void yw4_nifm_SetLocalNetworkMode(CPUState *c) {
    local_mode = (int)(c->x[0] & 1);
    LOG("nifm SetLocalNetworkMode %d\n", local_mode);
}
void yw4_nifm_SubmitNetworkRequest(CPUState *c) { LOG("nifm SubmitNetworkRequest (%s)\n", local_mode ? "local" : "internet"); (void)c; }
void yw4_nifm_CancelNetworkRequest(CPUState *c) { LOG("nifm CancelNetworkRequest\n"); (void)c; }
void yw4_nifm_IsNetworkRequestOnHold(CPUState *c) { c->x[0] = 0; }
void yw4_nifm_IsNetworkAvailable(CPUState *c) {
    static int last = -1;
    if (last != local_mode) LOG("nifm IsNetworkAvailable -> %d\n", local_mode);
    last = local_mode;
    c->x[0] = (uint64_t)local_mode;
}
/* HandleNetworkRequestErrorResult(): shows the system's error applet on a Switch; here: the request "failed" */
void yw4_nifm_HandleNetworkRequestErrorResult(CPUState *c) {
    LOG("nifm HandleNetworkRequestErrorResult\n");
    c->x[0] = 0x6E | (3400u << 9);  /* nifm module 110: a failure the game reports as "no connection" */
}
void yw4_nifm_GetCurrentProxySetting(CPUState *c) {
    memset(GPTR(c->x[0]), 0, 0xAA);
    c->x[0] = R_OK;
}
void yw4_nifm_GetCurrentPrimaryIpAddress(CPUState *c) {
    uint32_t a = 0;
    memcpy(GPTR(c->x[0]), &a, 4);
    LOG("nifm GetCurrentPrimaryIpAddress\n");
    c->x[0] = local_mode ? R_OK : (0x6E | (300u << 9));
}
