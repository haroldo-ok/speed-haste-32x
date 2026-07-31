#!/usr/bin/env python3
"""Static/data regressions for the D32XR-inspired optimization pass."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
render = (root / "src/sh_render.c").read_text()
assets = (root / "src/sh_assets.c").read_text()
math_s = (root / "src/platform/sh2_math.s").read_text()
header = (root / "assets/generated/speed_haste_assets.h").read_text()
manifest = (root / "assets/generated/manifest.txt").read_text()
blob = (root / "assets/generated/speed_haste_assets.bin").read_bytes()


def constant(name: str) -> int:
    m = re.search(rf"#define {name} (\d+)u", header)
    assert m, name
    return int(m.group(1))


# D32XR hardware divider pattern and cache-through SDRAM working set.
assert "0xFFFFFF00" in math_s
assert "dmuls.l" in math_s
assert "CACHE_THROUGH" in assets
assert "aligned(16)" in assets
assert "SHA_TILE_CACHE_COUNT" in assets
assert constant("SHA_TILE_CACHE_COUNT") == 28
assert "cached-map-coverage=95%" in manifest
assert "cached-map-coverage=92%" in manifest

# Map indices remain bytes; frequently used entries were sorted to the prefix.
for track in range(2):
    map_off = constant(f"SHA_MAP{track}_MAP128_OFF")
    map_data = blob[map_off:map_off + 128 * 128]
    cached = sum(index < 28 for index in map_data)
    assert cached * 100 // len(map_data) >= 97  # padded area uses cached tile zero

# No divide/modulo in the wall horizontal inner loop: interpolation is DDA.
dda = render[render.index("/* DDA all horizontal interpolation"):
             render.index("static void draw_minimap")]
assert "top_step" in dda and "bottom_step" in dda and "tx_step" in dda
assert "/ den" not in dda and "% tex.width" not in dda
assert "x += 2" in dda and "y += 2" in dda

# Build-time SEC_TO_MAP and collision/static-camera imports.
assert constant("SHA_MAP0_CAMERA_COUNT") == 11
assert constant("SHA_MAP1_CAMERA_COUNT") == 36
assert constant("SHA_MAP0_COLLISION_WALL_COUNT") == 76
assert constant("SHA_MAP1_COLLISION_WALL_COUNT") == 123
for track in range(2):
    wall_off = constant(f"SHA_MAP{track}_WALLS_OFF")
    wall_count = constant(f"SHA_MAP{track}_WALL_COUNT")
    flags = [int.from_bytes(blob[wall_off + i * 20 + 18:wall_off + i * 20 + 20], "little")
             for i in range(wall_count)]
    assert sum(bool(flag & 1) for flag in flags) == constant(f"SHA_MAP{track}_COLLISION_WALL_COUNT")

print("PASS: hardware divider, DDA loops, SDRAM cache, collision walls and TV cameras")
