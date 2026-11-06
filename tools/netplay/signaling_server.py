#!/usr/bin/env python3
"""Small, self-hostable room/signaling service for No Mercy P2P netplay.

This service only creates rooms and forwards connection-signaling messages. It
does not carry controller input, game state, ROM data, or video. Put it behind a
TLS reverse proxy before exposing it to the Internet.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hmac
import json
import os
import secrets
import socket
import socketserver
import struct
import threading
import time
from urllib.parse import parse_qs, urlsplit


MAX_PEERS = 20
MAX_PLAYERS = 4
MAX_BODY_BYTES = 16 * 1024
MAX_SIGNAL_BYTES = 4 * 1024
MAX_QUEUED_EVENTS = 512
MAX_LONG_POLL_MS = 15_000
PEER_TIMEOUT_SECONDS = 60
SIGNAL_TYPES = {"offer", "answer", "ice_candidate", "ready", "start", "p2p_hello"}
ROOM_CODE_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"

# STUN (RFC 5389) constants. Only Binding requests are handled. The service is
# used by the game to learn the public UDP endpoint of its netplay socket so
# peers can hole-punch to each other.
STUN_PORT = int(os.environ.get("NOMERCY_STUN_PORT", "8766"))
STUN_MAGIC_COOKIE = 0x2112A442
STUN_BINDING_REQUEST = 0x0001
STUN_BINDING_RESPONSE = 0x0101
STUN_ATTR_XOR_MAPPED_ADDRESS = 0x0020
STUN_ATTR_MAPPED_ADDRESS = 0x0001
STUN_FAMILY_IPV4 = 0x01

LOCK = threading.RLock()
ROOMS: dict[str, "Room"] = {}


@dataclass
class Peer:
    peer_id: str
    token: str
    name: str
    role: str
    player_slot: int | None
    queued_for_player: bool = False
    joined_at: float = field(default_factory=time.monotonic)
    last_seen: float = field(default_factory=time.monotonic)
    events: deque[dict] = field(default_factory=deque)
    next_cursor: int = 0
    signal_times: deque[float] = field(default_factory=deque)

    def public_info(self) -> dict:
        return {
            "peer_id": self.peer_id,
            "name": self.name,
            "role": self.role,
            "player_slot": self.player_slot,
            "queued_for_player": self.queued_for_player,
        }


@dataclass
class Room:
    code: str
    host_peer_id: str
    max_players: int = MAX_PLAYERS
    secret: str = field(default_factory=lambda: secrets.token_urlsafe(24))
    peers: dict[str, Peer] = field(default_factory=dict)
    seat_queue: deque[str] = field(default_factory=deque)
    condition: threading.Condition = field(default_factory=lambda: threading.Condition(LOCK))
    created_at: float = field(default_factory=time.monotonic)


def _new_code() -> str:
    return "".join(secrets.choice(ROOM_CODE_ALPHABET) for _ in range(8))


def _new_peer(name: str, role: str, player_slot: int | None) -> Peer:
    return Peer(
        peer_id=secrets.token_urlsafe(9),
        token=secrets.token_urlsafe(32),
        name=name,
        role=role,
        player_slot=player_slot,
    )


def _enqueue(room: Room, peer_id: str, event: dict) -> None:
    peer = room.peers.get(peer_id)
    if peer is None:
        return
    peer.next_cursor += 1
    queued = {"cursor": peer.next_cursor, **event}
    peer.events.append(queued)
    while len(peer.events) > MAX_QUEUED_EVENTS:
        peer.events.popleft()
    room.condition.notify_all()


def _broadcast(room: Room, event: dict, exclude: set[str] | None = None) -> None:
    excluded = exclude or set()
    for peer_id in tuple(room.peers):
        if peer_id not in excluded:
            _enqueue(room, peer_id, event)


def _remove_peer(room: Room, peer_id: str, reason: str) -> None:
    peer = room.peers.pop(peer_id, None)
    if peer is None:
        return
    room.seat_queue = deque(queued_id for queued_id in room.seat_queue if queued_id != peer_id)
    if peer_id == room.host_peer_id:
        _broadcast(room, {"kind": "room_closed", "reason": reason})
        ROOMS.pop(room.code, None)
        room.condition.notify_all()
        return
    _broadcast(room, {
        "kind": "peer_left",
        "peer_id": peer.peer_id,
        "role": peer.role,
        "player_slot": peer.player_slot,
        "reason": reason,
    })


def _prune_expired(now: float | None = None) -> None:
    now = time.monotonic() if now is None else now
    for room in tuple(ROOMS.values()):
        for peer in tuple(room.peers.values()):
            if now - peer.last_seen > PEER_TIMEOUT_SECONDS:
                _remove_peer(room, peer.peer_id, "timeout")


def _normalise_code(code: str) -> str:
    return code.strip().upper()


def _valid_name(value: object) -> str:
    if not isinstance(value, str):
        raise ValueError("name must be a string")
    name = " ".join(value.split())
    if not name or len(name) > 24:
        raise ValueError("name must be 1..24 characters")
    return name


class SignalingHandler(BaseHTTPRequestHandler):
    server_version = "NoMercySignaling/0.1"
    sys_version = ""

    def log_message(self, _format: str, *args: object) -> None:
        # Avoid logging invite codes, peer IDs, tokens, or signaling payloads.
        return

    def _json(self, status: int, body: dict) -> None:
        encoded = json.dumps(body, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        try:
            self.wfile.write(encoded)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _read_json(self) -> dict:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as exc:
            raise ValueError("invalid Content-Length") from exc
        if length <= 0 or length > MAX_BODY_BYTES:
            raise ValueError("request body must be 1..16384 bytes")
        try:
            value = json.loads(self.rfile.read(length))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("body must be valid JSON") from exc
        if not isinstance(value, dict):
            raise ValueError("body must be a JSON object")
        return value

    def _room_path(self) -> tuple[str, str] | None:
        parts = [part for part in urlsplit(self.path).path.split("/") if part]
        if len(parts) < 3 or parts[:2] != ["v1", "rooms"]:
            return None
        return _normalise_code(parts[2]), "/" + "/".join(parts[3:])

    def _get_room_and_peer(self, code: str) -> tuple[Room, Peer] | None:
        with LOCK:
            _prune_expired()
            room = ROOMS.get(code)
            if room is None:
                return None
            peer_id = self.headers.get("X-Peer-ID", "")
            peer = room.peers.get(peer_id)
            authorization = self.headers.get("Authorization", "")
            token = authorization[7:] if authorization.startswith("Bearer ") else ""
            if peer is None or not token or not hmac.compare_digest(peer.token, token):
                return None
            peer.last_seen = time.monotonic()
            return room, peer

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = urlsplit(self.path)
        if path.path == "/healthz":
            self._json(200, {"status": "ok", "service": "nomercy-signaling"})
            return

        route = self._room_path()
        if route is None or route[1] != "/events":
            self._json(404, {"error": "not found"})
            return

        code, _ = route
        try:
            query = parse_qs(path.query)
            after = int(query.get("after", ["0"])[0])
            wait_ms = min(max(int(query.get("wait_ms", ["0"])[0]), 0), MAX_LONG_POLL_MS)
            if after < 0:
                raise ValueError("after must be non-negative")
        except ValueError:
            self._json(400, {"error": "invalid event cursor or wait_ms"})
            return

        deadline = time.monotonic() + wait_ms / 1000.0
        with LOCK:
            while True:
                _prune_expired()
                room = ROOMS.get(code)
                if room is None:
                    self._json(404, {"error": "room not found"})
                    return
                peer_id = self.headers.get("X-Peer-ID", "")
                peer = room.peers.get(peer_id)
                authorization = self.headers.get("Authorization", "")
                token = authorization[7:] if authorization.startswith("Bearer ") else ""
                if peer is None or not token or not hmac.compare_digest(peer.token, token):
                    self._json(401, {"error": "invalid peer credentials"})
                    return
                peer.last_seen = time.monotonic()

                if peer.events and after < peer.events[0]["cursor"] - 1:
                    self._json(409, {"error": "event cursor expired", "cursor": peer.next_cursor})
                    return
                events = [event for event in peer.events if event["cursor"] > after]
                if events:
                    self._json(200, {"events": events, "cursor": events[-1]["cursor"]})
                    return

                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    self._json(200, {"events": [], "cursor": after})
                    return
                room.condition.wait(remaining)

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = urlsplit(self.path).path
        try:
            body = self._read_json()
        except ValueError as exc:
            self._json(400, {"error": str(exc)})
            return

        if path == "/v1/rooms":
            self._create_room(body)
            return

        route = self._room_path()
        if route is None:
            self._json(404, {"error": "not found"})
            return
        code, action = route
        if action == "/join":
            self._join_room(code, body)
        elif action == "/heartbeat":
            self._heartbeat(code)
        elif action == "/signal":
            self._signal(code, body)
        elif action == "/queue":
            self._queue_for_player(code)
        elif action == "/next-match":
            self._next_match(code, body)
        elif action == "/leave":
            self._leave(code)
        else:
            self._json(404, {"error": "not found"})

    def do_DELETE(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        route = self._room_path()
        if route is None or route[1] != "":
            self._json(404, {"error": "not found"})
            return
        authenticated = self._get_room_and_peer(route[0])
        if authenticated is None:
            self._json(401, {"error": "invalid peer credentials"})
            return
        room, peer = authenticated
        with LOCK:
            if peer.peer_id != room.host_peer_id:
                self._json(403, {"error": "only the host can close the room"})
                return
            _broadcast(room, {"kind": "room_closed", "reason": "host_closed"}, {peer.peer_id})
            room.condition.notify_all()
            ROOMS.pop(room.code, None)
        self._json(200, {"closed": True})

    def _create_room(self, body: dict) -> None:
        try:
            name = _valid_name(body.get("name"))
            max_players = int(body.get("max_players", MAX_PLAYERS))
            if max_players < 2 or max_players > MAX_PLAYERS:
                raise ValueError("max_players must be between 2 and 4")
        except (TypeError, ValueError) as exc:
            self._json(400, {"error": str(exc)})
            return

        with LOCK:
            _prune_expired()
            for _ in range(10):
                code = _new_code()
                if code not in ROOMS:
                    break
            else:
                self._json(503, {"error": "could not allocate a room code"})
                return
            host = _new_peer(name, "player", 0)
            room = Room(code=code, host_peer_id=host.peer_id, max_players=max_players)
            room.peers[host.peer_id] = host
            ROOMS[code] = room
        self._json(201, self._peer_response(room, host))

    def _join_room(self, code: str, body: dict) -> None:
        try:
            name = _valid_name(body.get("name"))
            requested_role = body.get("role", "player")
            if requested_role not in ("player", "spectator"):
                raise ValueError("role must be player or spectator")
        except ValueError as exc:
            self._json(400, {"error": str(exc)})
            return

        with LOCK:
            _prune_expired()
            room = ROOMS.get(code)
            if room is None:
                self._json(404, {"error": "room not found"})
                return
            if len(room.peers) >= MAX_PEERS:
                self._json(409, {"error": "room is full", "max_peers": MAX_PEERS})
                return

            used_slots = {peer.player_slot for peer in room.peers.values() if peer.player_slot is not None}
            role = requested_role
            slot = None
            if requested_role == "player":
                slot = next((i for i in range(room.max_players) if i not in used_slots), None)
                if slot is None:
                    self._json(409, {"error": f"all {room.max_players} player slots are occupied"})
                    return

            peer = _new_peer(name, role, slot)
            room.peers[peer.peer_id] = peer
            existing = [p.public_info() for p in room.peers.values() if p.peer_id != peer.peer_id]
            _broadcast(room, {"kind": "peer_joined", "peer": peer.public_info()}, {peer.peer_id})
        self._json(201, {**self._peer_response(room, peer), "peers": existing})

    @staticmethod
    def _peer_response(room: Room, peer: Peer) -> dict:
        return {
            "room_code": room.code,
            "peer_id": peer.peer_id,
            "peer_token": peer.token,
            "role": peer.role,
            "player_slot": peer.player_slot,
            "host_peer_id": room.host_peer_id,
            "max_peers": MAX_PEERS,
            "max_players": room.max_players,
            "heartbeat_seconds": PEER_TIMEOUT_SECONDS // 3,
            "stun_port": STUN_PORT,
            # Shared HMAC key for P2P packets. Delivered over the (TLS-protected)
            # signaling channel so invite-code games do not need a typed secret.
            "room_secret": room.secret,
        }

    def _heartbeat(self, code: str) -> None:
        authenticated = self._get_room_and_peer(code)
        if authenticated is None:
            self._json(401, {"error": "invalid peer credentials"})
            return
        room, _peer = authenticated
        with LOCK:
            self._json(200, {
                "ok": True,
                "peers": [peer.public_info() for peer in room.peers.values()],
            })

    def _signal(self, code: str, body: dict) -> None:
        authenticated = self._get_room_and_peer(code)
        if authenticated is None:
            self._json(401, {"error": "invalid peer credentials"})
            return
        room, sender = authenticated
        target_id = body.get("to_peer_id")
        message_type = body.get("type")
        data = body.get("data", {})
        if not isinstance(target_id, str) or target_id == sender.peer_id:
            self._json(400, {"error": "to_peer_id must identify another peer"})
            return
        if message_type not in SIGNAL_TYPES:
            self._json(400, {"error": "unsupported signaling type"})
            return
        if not isinstance(data, dict):
            self._json(400, {"error": "data must be a JSON object"})
            return
        if len(json.dumps(data, separators=(",", ":")).encode("utf-8")) > MAX_SIGNAL_BYTES:
            self._json(413, {"error": "signaling data too large"})
            return

        now = time.monotonic()
        with LOCK:
            recipient = room.peers.get(target_id)
            if recipient is None:
                self._json(404, {"error": "target peer not found"})
                return
            while sender.signal_times and now - sender.signal_times[0] > 10:
                sender.signal_times.popleft()
            if len(sender.signal_times) >= 120:
                self._json(429, {"error": "signaling rate limit exceeded"})
                return
            sender.signal_times.append(now)
            if message_type == "start" and sender.peer_id != room.host_peer_id:
                self._json(403, {"error": "only the host can announce game start"})
                return
            _enqueue(room, recipient.peer_id, {
                "kind": "signal",
                "from_peer_id": sender.peer_id,
                "type": message_type,
                "data": data,
            })
        self._json(202, {"queued": True})

    def _queue_for_player(self, code: str) -> None:
        authenticated = self._get_room_and_peer(code)
        if authenticated is None:
            self._json(401, {"error": "invalid peer credentials"})
            return
        room, peer = authenticated
        with LOCK:
            if peer.role != "spectator":
                self._json(409, {"error": "only spectators can queue for a player seat"})
                return
            if peer.queued_for_player:
                self._json(200, {
                    "queued": True,
                    "position": list(room.seat_queue).index(peer.peer_id) + 1,
                })
                return
            peer.queued_for_player = True
            room.seat_queue.append(peer.peer_id)
            position = len(room.seat_queue)
            _broadcast(room, {
                "kind": "spectator_queued",
                "peer_id": peer.peer_id,
                "position": position,
            })
        self._json(202, {"queued": True, "position": position})

    def _next_match(self, code: str, body: dict) -> None:
        authenticated = self._get_room_and_peer(code)
        if authenticated is None:
            self._json(401, {"error": "invalid peer credentials"})
            return
        room, sender = authenticated
        eliminated = body.get("eliminated_peer_ids", [])
        if (not isinstance(eliminated, list) or len(eliminated) > MAX_PLAYERS
                or any(not isinstance(peer_id, str) for peer_id in eliminated)
                or len(set(eliminated)) != len(eliminated)):
            self._json(400, {"error": "eliminated_peer_ids must be a unique list of up to four peer IDs"})
            return

        with LOCK:
            if sender.peer_id != room.host_peer_id:
                self._json(403, {"error": "only the host can advance the seat queue"})
                return
            freed_slots = []
            for peer_id in eliminated:
                peer = room.peers.get(peer_id)
                if peer is None or peer.role != "player" or peer.player_slot is None:
                    self._json(409, {"error": "eliminated peer is not an active player"})
                    return
                freed_slots.append((peer.player_slot, peer))

            transitions = []
            for slot, peer in sorted(freed_slots, key=lambda item: item[0]):
                peer.role = "spectator"
                peer.player_slot = None
                peer.queued_for_player = False
                transitions.append({"peer_id": peer.peer_id, "role": "spectator", "player_slot": None})
                _broadcast(room, {
                    "kind": "seat_changed",
                    "peer_id": peer.peer_id,
                    "role": "spectator",
                    "player_slot": None,
                })

                while room.seat_queue:
                    next_peer_id = room.seat_queue.popleft()
                    next_peer = room.peers.get(next_peer_id)
                    if next_peer is not None and next_peer.queued_for_player and next_peer.role == "spectator":
                        next_peer.role = "player"
                        next_peer.player_slot = slot
                        next_peer.queued_for_player = False
                        transition = {
                            "peer_id": next_peer.peer_id,
                            "role": "player",
                            "player_slot": slot,
                        }
                        transitions.append(transition)
                        _broadcast(room, {"kind": "seat_changed", **transition})
                        break

            _broadcast(room, {"kind": "next_match", "transitions": transitions})
        self._json(200, {"transitions": transitions, "queued_spectators": len(room.seat_queue)})

    def _leave(self, code: str) -> None:
        authenticated = self._get_room_and_peer(code)
        if authenticated is None:
            self._json(401, {"error": "invalid peer credentials"})
            return
        room, peer = authenticated
        with LOCK:
            _remove_peer(room, peer.peer_id, "left")
        self._json(200, {"left": True})


class StunHandler(socketserver.BaseRequestHandler):
    """Minimal RFC 5389 STUN Binding responder.

    The game sends a Binding request from its netplay UDP socket and gets back
    its server-reflexive address, which is then exchanged through the signaling
    API so both peers can hole-punch. Only IPv4 is supported.
    """

    def handle(self) -> None:
        data, sock = self.request
        if len(data) < 20:
            return
        message_type, message_length, magic_cookie, transaction_id = struct.unpack(
            "!HHI12s", data[:20]
        )
        if message_type != STUN_BINDING_REQUEST or magic_cookie != STUN_MAGIC_COOKIE:
            return
        if len(data) < 20 + message_length:
            return

        ip, port = self.client_address[0], self.client_address[1]
        try:
            packed_ip = socket.inet_aton(ip)
        except OSError:
            return

        xor_port = port ^ (STUN_MAGIC_COOKIE >> 16)
        xor_ip = struct.unpack("!I", packed_ip)[0] ^ STUN_MAGIC_COOKIE
        attribute = struct.pack(
            "!HHBBHI",
            STUN_ATTR_XOR_MAPPED_ADDRESS,
            8,
            0,
            STUN_FAMILY_IPV4,
            xor_port,
            xor_ip,
        )
        header = struct.pack(
            "!HHI12s",
            STUN_BINDING_RESPONSE,
            len(attribute),
            STUN_MAGIC_COOKIE,
            transaction_id,
        )
        sock.sendto(header + attribute, self.client_address)


class ThreadingUDPServer(socketserver.ThreadingMixIn, socketserver.UDPServer):
    allow_reuse_address = True
    daemon_threads = True


def make_stun_server(host: str = "0.0.0.0", port: int = STUN_PORT) -> ThreadingUDPServer:
    return ThreadingUDPServer((host, port), StunHandler)


def make_server(host: str = "127.0.0.1", port: int = 8765) -> ThreadingHTTPServer:
    return ThreadingHTTPServer((host, port), SignalingHandler)


def main() -> None:
    host = os.environ.get("NOMERCY_SIGNALING_BIND", "127.0.0.1")
    port = int(os.environ.get("NOMERCY_SIGNALING_PORT", "8765"))
    stun_host = os.environ.get("NOMERCY_STUN_BIND", "0.0.0.0")
    stun_port = int(os.environ.get("NOMERCY_STUN_PORT", str(STUN_PORT)))
    server = make_server(host, port)
    stun_server = make_stun_server(stun_host, stun_port)
    stun_thread = threading.Thread(target=stun_server.serve_forever, daemon=True)
    stun_thread.start()
    print(f"No Mercy signaling service listening on {host}:{port}")
    print(f"No Mercy STUN responder listening on {stun_host}:{stun_port}/udp")
    print("Expose it only through a TLS reverse proxy; game traffic is not relayed here.")
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        stun_server.shutdown()
        stun_server.server_close()
        server.server_close()


if __name__ == "__main__":
    main()
