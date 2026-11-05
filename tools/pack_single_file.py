#!/usr/bin/env python3
"""Append the Linux runtime payload to NoMercyRecompiled.

The payload holds only files the launcher needs to start (UI fonts/icons and the
controller database). It never contains the ROM or game textures: those are read
from the ROM the user selects on first run and are stored outside the executable.

Layout appended to the ELF image:

    payload:
        b"NMPAYL01" | u32 format(=1) | u32 entry_count
        per entry: u16 name_len | name | u64 data_len | data
    trailer (last 24 bytes):
        b"NMZPKG01" | u64 payload_size | u64 version (blake2b-64 of payload)

The executable itself is untouched, so it stays a valid ELF.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
from pathlib import Path

PAYLOAD_MAGIC = b"NMPAYL01"
TRAILER_MAGIC = b"NMZPKG01"
FORMAT_VERSION = 1


def collect(assets: Path, extra: list[Path]) -> list[tuple[str, bytes]]:
    entries: list[tuple[str, bytes]] = []
    for path in sorted(assets.rglob("*")):
        if path.is_file():
            entries.append((path.relative_to(assets.parent).as_posix(), path.read_bytes()))
    for path in extra:
        entries.append((path.name, path.read_bytes()))
    return entries


def build_payload(entries: list[tuple[str, bytes]]) -> bytes:
    out = bytearray()
    out += PAYLOAD_MAGIC
    out += struct.pack("<II", FORMAT_VERSION, len(entries))
    for name, data in entries:
        encoded = name.encode("utf-8")
        if len(encoded) > 0xFFFF:
            raise ValueError(f"path too long: {name}")
        out += struct.pack("<H", len(encoded))
        out += encoded
        out += struct.pack("<Q", len(data))
        out += data
    return bytes(out)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--extra", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    entries = collect(args.assets, [Path(p) for p in args.extra])
    payload = build_payload(entries)
    version = int.from_bytes(hashlib.blake2b(payload, digest_size=8).digest(), "little")

    image = args.binary.read_bytes()
    packed = image + payload + TRAILER_MAGIC + struct.pack("<QQ", len(payload), version)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(packed)
    os.chmod(args.output, 0o755)

    print(f"packed {len(entries)} files ({len(payload)} bytes) into {args.output}")
    print(f"payload version: {version:016x}")


if __name__ == "__main__":
    main()
