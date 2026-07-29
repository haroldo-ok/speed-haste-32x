#!/usr/bin/env python3
"""Static invariants that catch malformed/non-bootable 32X images."""
from pathlib import Path
import struct
import sys

rom = Path(sys.argv[1] if len(sys.argv) > 1 else "release/SpeedHaste32X.32x")
data = rom.read_bytes()
assert 1024 * 1024 <= len(data) <= 4 * 1024 * 1024, f"unexpected ROM size: {len(data)}"
assert len(data) % (512 * 1024) == 0, "ROM must be padded in 512 KiB banks"
assert data[0x100:0x110] == b"SEGA 32X        ", "missing Genesis/32X signature"
assert data[0x120:0x12B] == b"SPEED HASTE", "bad domestic title"
assert data[0x3C0:0x3D0] == b"SPEED HASTE 32X ", "bad MARS header"
assert struct.unpack_from(">I", data, 0x3D4)[0] > 0, "empty SH-2 text image"
assert struct.unpack_from(">I", data, 0x3E0)[0] == 0x06000240, "bad master SH-2 entry"
assert struct.unpack_from(">I", data, 0x3E4)[0] == 0x06000244, "bad slave SH-2 entry"
assert struct.unpack_from(">I", data, 0x1A4)[0] == len(data) - 1, "bad ROM end address"
expected = 0
for i in range(0x200, len(data) - 1, 2):
    expected = (expected + ((data[i] << 8) | data[i + 1])) & 0xFFFF
actual = struct.unpack_from(">H", data, 0x18E)[0]
assert actual == expected and actual != 0, f"checksum mismatch {actual:04x}!={expected:04x}"
assert sum(byte != 0 for byte in data[:0x10000]) > 8000, "ROM payload is unexpectedly empty"
print(f"PASS: static 32X ROM checks ({rom}, checksum 0x{actual:04X})")
