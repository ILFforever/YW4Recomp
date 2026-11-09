# 0057 — Playing together over the internet: the Switch's local play, carried by our own server
<!-- decision
state: plan
date: 2026-11-09
replaces: none
code: recomp/overrides.toml, runtime/src/ldn.c, runtime/src/socket.c, tools/relay/ (to come)
-->

Status: **plan** (2026-11-09, the owner asked for it: trading and the Blasters mode with friends).

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

### In the launcher

Settings → Online: the relay address (`host:port`) and the room code, and the player's name. Written to
`settings.ini` (`YW4_RELAY`, `YW4_ROOM`) as the other settings are.

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
