# Changelog

## 0.1.1 — Linux update

### Added

- **Online play (experimental)** — opt-in via `-DNOMERCY_ONLINE=ON`. The
  release binary includes it.
  - Invite-code lobby: **Host & Wait**, automatic start when the room fills,
    **Start Now** to begin early, **Cancel Lobby**, **Join Game**, and
    **Spectate**.
  - Server-issued per-room secret, so invite-code players do not type a shared
    password.
  - STUN address discovery and direct UDP hole punching; the signaling service
    never relays gameplay.
  - 2–4 players plus spectators, adjustable input delay, per-seat input lead,
    packet age, and substitution diagnostics.
- **CRT filter** in the Graphics tab: scanlines, aperture grille, shadow mask,
  and slot mask, with scanline, mask, curvature, and vignette strengths.
- `tools/netplay/`: self-hosted signaling service, STUN responder, tests, and
  protocol notes.
- Online build instructions and the `NOMERCY_ONLINE` CMake option.

### Fixed

- Offline input now reports only the controller ports that have an assigned
  player. Previously all four ports looked occupied, which made the game fill a
  singles match with CPU partners/opponents (the 2-vs-1 match) and made the CPU
  feel unfairly strong.
- Release packaging and documentation were brought back in sync with the
  current game and online build.

### Notes

- Online play needs `libcurl`, the same USA Rev 1 ROM on every machine, and a
  reachable signaling server (`python3 tools/netplay/signaling_server.py`).
  Direct UDP means strict/symmetric NATs may still need a forwarded port.
- Ray tracing is **not** included. The upstream ray tracing path is D3D12-only
  and experimental; the Vulkan RT64 port remains future work.
- Build the offline game with the default CMake configuration. Build online with
  `-DNOMERCY_ONLINE=ON` in a separate build directory.
