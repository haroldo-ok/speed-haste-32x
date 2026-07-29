#!/usr/bin/env python3
"""Update Genesis ROM end address and checksum after padding."""
from pathlib import Path
import struct
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: fix_rom_header.py ROM.32x")
path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
if len(data) < 0x200:
    raise SystemExit("ROM is too small")
# ROM range in the standard header.
data[0x1A4:0x1A8] = struct.pack(">I", len(data) - 1)
# Standard 16-bit big-endian sum from 0x200 to end.
checksum = 0
for i in range(0x200, len(data) - 1, 2):
    checksum = (checksum + ((data[i] << 8) | data[i + 1])) & 0xFFFF
data[0x18E:0x190] = struct.pack(">H", checksum)
path.write_bytes(data)
print(f"header: end=0x{len(data)-1:08X} checksum=0x{checksum:04X}")
