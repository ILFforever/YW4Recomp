# 0082 — Playing together over the internet: the Switch's local play, carried by our own server
<!-- decision
state: plan
date: 2026-11-19
replaces: none
code: recomp/overrides.toml, runtime/src/ldn.c, runtime/src/socket.c, runtime/src/relay.c, runtime/src/relay.h, runtime/src/ssl.c, runtime/src/svc_csrng.c, runtime/src/svc_misc.c, runtime/src/svc_acc.c, runtime/src/settings.c, runtime/src/gamemenu.c, tools/relay/relay.py, tools/relay/setup_server.sh, tools/relay/keepalive.py
-->

Status: **plan**. Written and built by **Quandiy** in their fork (github.com/Quandiy/YW4Recomp, branch `amd-fixes`,
2026-11-09/10, as their decision 0057: their owner asked for trading and the Blasters mode with friends). Brought into
this repo on 2026-11-19 at the owner's request, with their commits cherry-picked so they stay the author (each says
`cherry picked from commit ...`), renumbered 0082 (our 0057 is the mouse menu lists).

Changes made bringing it over:

- nn::ngc: this repo already had `ProfanityFilter::Initialize`, `GetContentVersion` and `MaskProfanityWordsInText`
  (swkbd.c, the name keyboard); the fork's copies of those three are left out, its constructor and destructor kept.
- Settings: `YW4_RELAY`, `YW4_ROOM` and `YW4_NICKNAME` are lines in `settings.ini` (added by hand for now, like
  `YW4_PERF`), not the fork's separate `online.ini`.
- The fork's launcher Online page is not brought over yet: the launcher has changed a lot since; it gets its own page
  in the current launcher's style (a mockup first).
- Not yet known: whether a trade or a Blasters match goes through end to end. The fork got as far as one game seeing
  the other's room (its commit ba5fdf8).

## Context

Yo-kai Watch 4++ plays with others in two ways (docs/recon.md): over the internet through Nintendo's NEX servers,
which are gone for good and can't be rebuilt, and **locally** (several Switches in one room) through `nn::ldn`, the
Switch's local wireless. Both run through Pia 5.17, Nintendo's game networking library, linked into `main`.

Local play needs no server of Nintendo's: the consoles find each other over Wi-Fi and then talk UDP. If the game
believes it is playing locally while the packets really travel over the internet, friends can trade and play
Blasters together. Ryujinx did exactly this for its emulator ("LDN" builds and later its own LDN server), for many
games; it is the proven way.

What `main` uses (symbols in the image, 2026-11-09):

- `nn::ldn`: Initialize/Finalize, GetState, AttachStateChangeEvent, OpenAccessPoint/CloseAccessPoint,
  CreateNetwork(Private)/DestroyNetwork, SetAdvertiseData, SetStationAcceptPolicy, Reject, OpenStation/CloseStation,
  Scan(Private), Connect(Private)/Disconnect, GetNetworkInfo, GetNetworkConfig, GetIpv4Address,
  GetSecurityParameter, GetDisconnectReason, MakeIpv4Address, MakeBroadcastAddress.
- `nn::socket`: Initialize/Finalize, Socket, Bind, Connect, Listen, Accept, Send/SendTo, Recv/RecvFrom, Poll,
  SetSockOpt/GetSockOpt, GetSockName, Fcntl, Shutdown, Close, the Inet* helpers, GetAddrInfo.
- `nn::nifm`: Initialize, SubmitNetworkRequest, IsNetworkAvailable, SetLocalNetworkMode, GetCurrentPrimaryIpAddress.

Today the runtime has none of `ldn:u`, `bsd:u`: the sdk's versions of these functions would send IPC to services
that don't exist and the run stops ("unimplemented IPC command").

## Proposal

### Native `nn::ldn` and `nn::socket`, not the system services

Like NVN, vi and swkbd (decision 0016), the sdk's `nn::ldn::*` and `nn::socket::*` become native functions
(`recomp/overrides.toml`), so there is no IPC to emulate: the arguments are the documented structs (Switchbrew
"LDN services": NetworkInfo 0x480 bytes, NetworkConfig 0x20, SecurityConfig 0x44, UserConfig 0x30, ...) and plain
BSD socket calls.

- **ldn.c** keeps the state machine (None, Initialized, AccessPoint, AccessPointCreated, Station,
  StationConnected), the state-change event (an os SystemEvent, as vi's vsync event), the network info with its
  nodes and advertise data, and talks to the relay.
- **socket.c** gives the game real host sockets for everything except the local network's addresses: a socket bound
  to the ldn address (169.254.x.y, as on a Switch) sends and receives through the relay instead.

### The relay: a small server of our own

One small program (`tools/relay/`, Python, no dependencies) that one friend runs, or a cheap server runs:

- **Rooms**: the host's CreateNetwork opens a room on the relay with the game's network info and advertise data;
  the others' Scan lists the rooms of the same game (LocalCommunicationId) on the same relay.
- **Join**: Connect adds a node; the relay gives every node an address (169.254.1.1 for the host, .2, .3, ...), and
  sends everyone the new network info (that is what the state-change event reports).
- **Packets**: UDP from the game to another node's address (or the broadcast address) goes to the relay over one
  TCP/UDP connection per player, which passes it on. Pia does its own reliability, so passing packets on is enough.
- **Room code**: an optional word the players agree on, so strangers on the same relay don't see each other.

### The game's Online button goes to our server (2026-11-19)

The owner's model: **Online** means our server, **Local** means the home network. The game's own Online mode logs in
to Nintendo's servers (an account check, then NEX), which are gone, so Online is turned into local play at the
moment the player chooses it:

- How it was found: the call stacks of `ssl`'s start (Online) and `ldn`'s start (Local), `YW4_NET_TRACE=1`, both
  lead into the Lua interpreter (`main+0xd2…–0xd5…`: `lua_resume` at `0xd2e090`, called by `0x8a9760`); the menu
  script `trade_menu_top_5.00.01.lua.bin` (script.cpk) calls commands by the CRC32 of their name, kept as numbers:
  `funcLuaMenuTradeCommand(0xbfba3718)` asks "Online?" and the script then runs either the login (network command
  `0xe7eb7a7d`, handler `f_7163f0`) or local play. The answer is one byte of the trade menu object (+0x239c), which
  the trade command `0x1c105a8d` (`f_78a260`) stores when the player picks Online or Local. The same hash appears in
  `busters_top_*.lua.bin`, but Blasters' script doesn't ask it: see below.
- **Blasters (2026-11-20)** takes its path from the row picked in its top list (control `0x4e6f00fb`: 0 alone,
  1 Online, 2 Local), not from that byte. The row reaches the script through `funcLuaMenuCommand(0x9b2aaf08)`
  (`main+0x2ef9b0`, the control's `+0x13c` handed over at `0x2efa60`): there, once after a pick (`OnDecideFocus`),
  Online becomes Local and the relay takes Online to our server with Blasters' room settings
  (`yw4_hook_menu_row`). Its matching's own Online flag (`0x4a3223c4`, `main+0x66aa80`, the same byte store as the
  trade's) is kept Local too. Owner-tested: host and join through both servers, a mission played together.
- The hook (`recomp/overrides.toml`, `yw4_hook_net_mode` in `ldn.c`, at `main+0x78a2d0`, the store): the choice
  goes to the relay (`relay_choose`) and the game stores Local. Online then connects to `YW4_ONLINE_SERVER` with the
  room settings of that activity; Local to `YW4_RELAY` (or alone, until a home-network discovery exists).
- With the internet request granted (`YW4_FAKE_INTERNET=1`) the Online path stops at the Nintendo account check
  (`acc` IManagerForApplication command 0): rebuilding it would mean faking Nintendo accounts and NEX, not done.
- `runtime/src/ssl.c`: nn::ssl starts without a service, so before the hook (and in any menu not hooked yet) the
  Online button shows the game's own "connection failed" instead of stopping the run.

### Servers

Two relays on Oracle Cloud's free tier (US East `129.153.179.140`, Singapore `161.118.234.155`, port 41230), each
set up with `tools/relay/setup_server.sh` (a systemd service; `--keepalive N` a lowest-priority CPU load so the free
machine isn't reclaimed as idle). The relay counts each room's traffic (a line a minute and a total), for sizing.

Hardening (2026-11-20, owner: "what else should our server connection do to match industry standard"; the easy ones
first; `python tools/relay/test_relay.py` tests each):

- **Room ids:** a room's SessionId was `rand()`, never seeded: every game's first room had the same one, so a second
  host took the first's place and an old connection leaving removed a newer room (a private Blasters room nobody
  could find). Now unique per room (`session_id` in `ldn.c`), and a closing room removes only itself.
- **Dead connections:** TCP keepalive from 10 s (every 5 s, 3 missed) and `TCP_USER_TIMEOUT` 20 s: about 25 s, where
  one held a room for 2 1/2 hours. **Heartbeat** (protocol 2): the game sends PING every 3 s and the relay answers;
  the relay drops a heartbeat client silent for 15 s (a frozen game), the game drops a relay silent for 12 s. Each
  side only once it knows the other speaks protocol 2.
- **Limits:** 16 connections and 4 rooms per address, 1000 and 300 in all, 400 game packets a second per client (a
  Blasters mission: about 60; more are dropped, not the connection), the hello within 10 s.
- **Status:** a summary line every 5 minutes; `python tools/relay/status.py` shows both servers.
- **Privacy** (owner: "very important"): the logs name clients by a hash of their address, salted anew at each
  start, never the address; the machine's logs (journal, and rsyslog's syslog / auth.log) are kept 7 days
  (`setup_server.sh`); the logs from before were wiped on both servers. The README says so.
- **Not yet:** game packets over UDP (TCP holds back every packet behind a lost one: stutters on long routes),
  encryption, rejoining a room after a short drop, deploys that keep rooms.

### In the game: the overlay's Online tab

A tab of its own (owner, 2026-11-19, the "Cards" layout; `draw_online_page` in `gamemenu.c`): the server (each
with its live ping, `relay_ping_start`), Ark Swap and Blasters each **Public** (the server's shared lobby) or
**Private** with a code, and the profile: the player's name (`YW4_NICKNAME`, which acc's GetProfile hands the game)
and Discord status. Saved in `settings.ini` like the other settings. Home-network play keeps `YW4_RELAY` /
`YW4_ROOM` lines for now; a launcher page can come later.

### Phases

0. **Diagnostics** (first): native ldn and sockets that log every call with its arguments and act as a Switch
   alone in a room (Scan finds nobody; a host's network has only itself). The game no longer stops in its local
   play menus, and one try of trading and of Blasters shows which calls Pia makes in which order.
1. **Two games on one PC**: the relay on localhost, two instances of the game (two save folders). Trading between
   them is the first goal: it is the smallest exchange.
2. **Over the internet**: the relay on a friend's PC (one forwarded port) or a small server; the launcher fields.
3. **Blasters**: more players, more traffic, timing; measured once trading works.

## Risks

- Pia may check things a relay doesn't give it naturally (link level, timing of the state-change event, node
  updates in GetNetworkInfo's history). Ryujinx's experience says it's doable; we'll see which in phase 0/1.
- Latency: local play expects a few milliseconds; over the internet it's 20-100 ms. Trading doesn't mind;
  Blasters may lag or drop players if Pia's timeouts are tight.
- Every player needs the same game version (2.2.0) and the same mods that change game data.
- The relay sees only game packets, never accounts or files. It should still only be run by someone the group trusts.
