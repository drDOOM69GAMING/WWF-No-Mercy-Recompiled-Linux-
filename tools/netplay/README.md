# No Mercy P2P netplay

> **Status:** the online client is built with `-DNOMERCY_ONLINE=ON`; the default
> source build stays offline. The release `dist/NoMercyRecompiled` includes the
> online client. The invite-code lobby/join flow and the UDP input transport are
> tested; ray tracing is not part of this build.

This folder contains the self-hostable invite/signaling service and the STUN
responder used by the game's online mode. The service assigns invite codes and
forwards connection-setup messages; it **does not** relay gameplay, ROMs, game
assets, or video.

Two transports are available in the game:

- **Invite code** — the host creates a room through the signaling service. Peers
  exchange their public UDP endpoints through the service, learn their own
  endpoint with STUN, and hole-punch directly to each other.
- **Direct IP / LAN** — the host exposes a UDP port and players connect to its
  address. Useful when both sides are on the same LAN or UDP port forwarding is
  already set up.

## Current limits

- A room has up to 20 connections: 4 controller players and 16 spectators.
- Sync is **host-authoritative input lockstep**. Clients sample their controller
  and send it tagged a few frames ahead (`Input delay`); the host finalizes each
  frame and broadcasts the four pads. The host does not block on a slow peer, and
  every packet carries a short input history for loss/reordering recovery.
- Clients can request a frame resync if they fall behind; the host bursts the
  missing frames from a 128-frame buffer.
- Invite-code mode needs a cone NAT (or a LAN path) for hole punching. Symmetric
  NATs and strict firewalls can still fail; there is no TURN relay yet.
- Spectators receive the host's broadcast and do not send controller input.
- Each participant must provide a compatible USA Rev 1 ROM locally. Neither the
  service nor the game uploads or distributes ROM data.
- Invite-code rooms get a per-room HMAC secret from the signaling service.
  Direct mode uses the typed shared secret. In both cases it authenticates
  packets; it is **not** encryption.
- This is still a prototype. Put the HTTP service behind HTTPS/TLS and add
  operational rate limits before opening it to the Internet.

## Run the service

Requires Python 3.10+ and only the standard library:

```sh
python3 tools/netplay/signaling_server.py
```

Defaults:

- HTTP signaling: `127.0.0.1:8765/tcp`
- STUN: `0.0.0.0:8766/udp`

Environment overrides:

- `NOMERCY_SIGNALING_BIND`, `NOMERCY_SIGNALING_PORT`
- `NOMERCY_STUN_BIND`, `NOMERCY_STUN_PORT`

For public hosting, bind the HTTP service to localhost and put a TLS reverse
proxy such as Caddy in front of it. The STUN responder is UDP, so expose UDP
8766 directly (or change the port and set it in the game's server URL flow; the
room response tells the game which STUN port to use).

## Playing from the launcher

1. Launch the game and choose **Play Online**.
2. Keep **Connection** on **Invite code**.
3. Enter the signaling server URL (for example `https://nm.example.com`).
4. **Host:** press **Host & Wait**. The room code appears in the tab and stays
   alive while the lobby heartbeats. The match starts automatically when the
   configured players have joined; **Start Now** begins early, and **Cancel
   Lobby** backs out.
5. **Joiner:** enter the code and press **Join Game** (or **Spectate**). The
   server assigns your player seat, and the game starts when the host starts.
6. There is no shared password to type for invite-code rooms; the server issues
   the room secret. For direct mode, the typed secret must match on both sides.
7. The **Esc / Menu** Netplay tab shows the room code, host addresses, per-seat
   input lead, time since the last packet, and substitution counts.

For LAN or port-forwarded play, switch **Connection** to **Direct IP / LAN** and
use **Host Game** / **Join Game**. Open UDP port 27025 on the host.

### Environment variables

The direct transport can still be configured without the UI. Environment
variables take precedence over the menu and are useful for scripted launches:

```sh
WCW_NETPLAY_MODE=host WCW_NETPLAY_SECRET='replace-with-a-long-random-key' \
WCW_NETPLAY_PLAYERS=4 WCW_NETPLAY_PORT=27025 WCW_NETPLAY_DELAY=4 \
WCW_AUTOBOOT=1 ./dist/NoMercyRecompiled
```

```sh
WCW_NETPLAY_MODE=join WCW_NETPLAY_SECRET='replace-with-a-long-random-key' \
WCW_NETPLAY_HOST=192.0.2.10 WCW_NETPLAY_SEAT=1 WCW_NETPLAY_PORT=27025 \
WCW_AUTOBOOT=1 ./dist/NoMercyRecompiled
```

Use `WCW_NETPLAY_MODE=spectate` for a spectator. `WCW_NETPLAY_DELAY` is 0–8
frames (default 4). `WCW_NETPLAY_SPECTATORS` asks the host to wait for that many
spectators during the direct handshake.

## API sketch

- `GET /healthz`
- `POST /v1/rooms` — create a room (`{"name", "max_players": 2..4}`); returns an
  invite code, host credentials, `max_players`, and `stun_port`.
- `POST /v1/rooms/{code}/join` — join as a player or spectator. Player slots are
  0 to `max_players - 1`; spectators have no controller slot.
- `POST /v1/rooms/{code}/queue` — a spectator joins the FIFO queue for a player
  seat.
- `POST /v1/rooms/{code}/next-match` — host announces eliminated players; their
  seats are released and queued spectators are promoted for the next bout.
- `GET /v1/rooms/{code}/events?after={cursor}&wait_ms={n}` — long-poll targeted
  signaling and room-presence events.
- `POST /v1/rooms/{code}/signal` — forward an offer, answer, ICE candidate,
  readiness event, host start announcement, or `p2p_hello` endpoint exchange.
- `POST /v1/rooms/{code}/heartbeat` and `/leave`; `DELETE /v1/rooms/{code}`
  closes a room (host only).

Authenticated requests use `X-Peer-ID` and `Authorization: Bearer <peer_token>`.
Rooms are in-memory and expire after peer heartbeats stop; restarting the
service closes all rooms. The signaling allowlist deliberately rejects input
and state packets so this endpoint cannot turn into a gameplay relay.

The STUN responder implements RFC 5389 Binding requests for IPv4 only and
returns `XOR-MAPPED-ADDRESS`.

## Traffic topology

Active players get the direct host path. The host keeps the last 128 finalized
frames and includes the last 12 in every frame packet, so a single lost packet
does not stall the match. Clients tag their current controller sample for
`frame + input_delay`; the host uses a short bounded wait (12 ms) for a late
sample and then falls back to the player's most recent input. This keeps the
host running at frame cadence even when a peer is late, and the input-delay
setting absorbs steady-state latency. Spectator fan-out is still direct from the
host; a relay tree is a later optimization only if measurements justify it.

Every peer must use the same USA Rev 1 ROM and compatible save data.

## Tests

```sh
python3 -m unittest discover -s tools/netplay -p 'test_*.py'
```
