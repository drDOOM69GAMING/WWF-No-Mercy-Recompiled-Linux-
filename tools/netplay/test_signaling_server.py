import json
from http.server import ThreadingHTTPServer
import socket
import struct
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import signaling_server


class SignalingServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), signaling_server.SignalingHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = "http://127.0.0.1:%d" % cls.server.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        with signaling_server.LOCK:
            signaling_server.ROOMS.clear()

    def request(self, method, path, body=None, peer=None):
        data = None if body is None else json.dumps(body).encode("utf-8")
        headers = {}
        if data is not None:
            headers["Content-Type"] = "application/json"
        if peer is not None:
            headers["X-Peer-ID"] = peer["peer_id"]
            headers["Authorization"] = "Bearer " + peer["peer_token"]
        req = Request(self.base + path, data=data, headers=headers, method=method)
        with urlopen(req, timeout=3) as response:
            return response.status, json.loads(response.read())

    def request_error(self, method, path, body=None, peer=None):
        data = None if body is None else json.dumps(body).encode("utf-8")
        headers = {}
        if data is not None:
            headers["Content-Type"] = "application/json"
        if peer is not None:
            headers["X-Peer-ID"] = peer["peer_id"]
            headers["Authorization"] = "Bearer " + peer["peer_token"]
        req = Request(self.base + path, data=data, headers=headers, method=method)
        with self.assertRaises(HTTPError) as error:
            urlopen(req, timeout=3)
        with error.exception as response:
            return response.code, json.loads(response.read())

    def create_room(self):
        status, host = self.request("POST", "/v1/rooms", {"name": "Host"})
        self.assertEqual(status, 201)
        return host

    def test_player_slots_and_spectators(self):
        host = self.create_room()
        code = host["room_code"]
        self.assertEqual(host["player_slot"], 0)
        players = [host]
        for name, slot in (("Player 2", 1), ("Player 3", 2), ("Player 4", 3)):
            status, peer = self.request("POST", f"/v1/rooms/{code}/join", {"name": name})
            self.assertEqual(status, 201)
            self.assertEqual(peer["player_slot"], slot)
            players.append(peer)

        status, error = self.request_error(
            "POST", f"/v1/rooms/{code}/join", {"name": "Player 5", "role": "player"}
        )
        self.assertEqual(status, 409)
        self.assertIn("player slots", error["error"])

        status, spectator = self.request(
            "POST", f"/v1/rooms/{code}/join", {"name": "Observer", "role": "spectator"}
        )
        self.assertEqual(status, 201)
        self.assertEqual(spectator["role"], "spectator")
        self.assertIsNone(spectator["player_slot"])

    def test_room_has_twenty_connection_cap(self):
        host = self.create_room()
        code = host["room_code"]
        for i in range(19):
            status, _peer = self.request("POST", f"/v1/rooms/{code}/join", {
                "name": "Spectator %02d" % i,
                "role": "spectator",
            })
            self.assertEqual(status, 201)
        status, error = self.request_error("POST", f"/v1/rooms/{code}/join", {
            "name": "One too many",
            "role": "spectator",
        })
        self.assertEqual(status, 409)
        self.assertEqual(error["max_peers"], 20)

    def test_signaling_is_targeted_and_does_not_accept_gameplay_packets(self):
        host = self.create_room()
        code = host["room_code"]
        _, guest = self.request("POST", f"/v1/rooms/{code}/join", {"name": "Guest"})

        status, result = self.request("POST", f"/v1/rooms/{code}/signal", {
            "to_peer_id": guest["peer_id"],
            "type": "offer",
            "data": {"sdp": "test-offer"},
        }, peer=host)
        self.assertEqual(status, 202)
        self.assertTrue(result["queued"])

        status, events = self.request(
            "GET", f"/v1/rooms/{code}/events?after=0", peer=guest
        )
        self.assertEqual(status, 200)
        self.assertEqual(len(events["events"]), 1)
        self.assertEqual(events["events"][0]["type"], "offer")
        self.assertEqual(events["events"][0]["data"]["sdp"], "test-offer")

        status, error = self.request_error("POST", f"/v1/rooms/{code}/signal", {
            "to_peer_id": guest["peer_id"],
            "type": "game_input",
            "data": {"buttons": 1},
        }, peer=host)
        self.assertEqual(status, 400)
        self.assertIn("unsupported", error["error"])

    def test_credentials_and_host_authority(self):
        host = self.create_room()
        code = host["room_code"]
        _, guest = self.request("POST", f"/v1/rooms/{code}/join", {"name": "Guest"})

        status, error = self.request_error("POST", f"/v1/rooms/{code}/signal", {
            "to_peer_id": host["peer_id"],
            "type": "start",
            "data": {},
        }, peer=guest)
        self.assertEqual(status, 403)
        self.assertIn("host", error["error"])

        status, error = self.request_error("POST", f"/v1/rooms/{code}/heartbeat", {})
        self.assertEqual(status, 401)
        self.assertIn("credentials", error["error"])

    def test_queued_spectator_fills_eliminated_seat_for_next_match(self):
        host = self.create_room()
        code = host["room_code"]
        _, player = self.request("POST", f"/v1/rooms/{code}/join", {"name": "Player 2"})
        _, spectator = self.request("POST", f"/v1/rooms/{code}/join", {
            "name": "Next Up",
            "role": "spectator",
        })

        status, queued = self.request("POST", f"/v1/rooms/{code}/queue", {}, peer=spectator)
        self.assertEqual(status, 202)
        self.assertEqual(queued["position"], 1)

        status, result = self.request("POST", f"/v1/rooms/{code}/next-match", {
            "eliminated_peer_ids": [player["peer_id"]],
        }, peer=host)
        self.assertEqual(status, 200)
        self.assertEqual(result["transitions"], [
            {"peer_id": player["peer_id"], "role": "spectator", "player_slot": None},
            {"peer_id": spectator["peer_id"], "role": "player", "player_slot": 1},
        ])
        self.assertEqual(result["queued_spectators"], 0)

        status, room = self.request("POST", f"/v1/rooms/{code}/heartbeat", {}, peer=spectator)
        self.assertEqual(status, 200)
        promoted = next(peer for peer in room["peers"] if peer["peer_id"] == spectator["peer_id"])
        self.assertEqual(promoted["role"], "player")
        self.assertEqual(promoted["player_slot"], 1)

    def test_health_check(self):
        status, result = self.request("GET", "/healthz")
        self.assertEqual(status, 200)
        self.assertEqual(result["status"], "ok")

    def test_room_player_limit(self):
        status, host = self.request("POST", "/v1/rooms", {"name": "Host", "max_players": 2})
        self.assertEqual(status, 201)
        self.assertEqual(host["max_players"], 2)
        code = host["room_code"]

        status, _player = self.request("POST", f"/v1/rooms/{code}/join", {
            "name": "Player 2",
            "role": "player",
        })
        self.assertEqual(status, 201)

        status, error = self.request_error("POST", f"/v1/rooms/{code}/join", {
            "name": "Player 3",
            "role": "player",
        })
        self.assertEqual(status, 409)

    def test_room_secret_is_shared(self):
        host = self.create_room()
        self.assertTrue(host.get("room_secret"))
        status, player = self.request("POST", f"/v1/rooms/{host['room_code']}/join", {
            "name": "Player 2",
            "role": "player",
        })
        self.assertEqual(status, 201)
        self.assertEqual(player["room_secret"], host["room_secret"])

    def test_stun_binding(self):
        server = signaling_server.make_stun_server("127.0.0.1", 0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(2)
        try:
            sock.bind(("127.0.0.1", 0))
            transaction_id = b"nomercy-test"
            request = struct.pack(
                "!HHI12s",
                signaling_server.STUN_BINDING_REQUEST,
                0,
                signaling_server.STUN_MAGIC_COOKIE,
                transaction_id,
            )
            sock.sendto(request, server.server_address)
            response, _ = sock.recvfrom(512)

            message_type, message_length, magic_cookie, returned_id = struct.unpack(
                "!HHI12s", response[:20]
            )
            self.assertEqual(message_type, signaling_server.STUN_BINDING_RESPONSE)
            self.assertEqual(magic_cookie, signaling_server.STUN_MAGIC_COOKIE)
            self.assertEqual(returned_id, transaction_id)
            self.assertGreaterEqual(message_length, 12)

            attr_type, attr_length = struct.unpack("!HH", response[20:24])
            self.assertEqual(attr_type, signaling_server.STUN_ATTR_XOR_MAPPED_ADDRESS)
            self.assertEqual(attr_length, 8)
            _reserved, _family, xor_port, xor_ip = struct.unpack("!BBHI", response[24:36])
            port = xor_port ^ (signaling_server.STUN_MAGIC_COOKIE >> 16)
            address = xor_ip ^ signaling_server.STUN_MAGIC_COOKIE
            self.assertEqual(port, sock.getsockname()[1])
            self.assertEqual(socket.inet_ntoa(struct.pack("!I", address)), "127.0.0.1")
        finally:
            sock.close()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
