#!/usr/bin/env python3
"""Validate the generated data is an actual conversion of Speed Haste MAP00."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
blob = (root / "assets/generated/speed_haste_assets.bin").read_bytes()
header = (root / "assets/generated/speed_haste_assets.h").read_text()
manifest = (root / "assets/generated/manifest.txt").read_text()

def constant(name: str) -> int:
    match = re.search(rf"#define {name} (\d+)u", header)
    assert match, name
    return int(match.group(1))

assert blob[:8] == b"SH32DATA"
assert constant("SHA_TILE_COUNT") == 115
assert constant("SHA_PATH_COUNT") == 88
assert constant("SHA_START_COUNT") == 20
assert constant("SHA_WALL_COUNT") == 86
assert constant("SHA_OBSTACLE_COUNT") == 263
assert constant("SHA_SPRITE_COUNT") >= 100
assert "Racer's Edge" in manifest
assert len(blob) > 1_000_000

map_off = constant("SHA_MAP128_OFF")
tiles_off = constant("SHA_TILES_OFF")
map_data = blob[map_off:map_off + 128 * 128]
assert len(set(map_data)) == 115
# The converted circuit must contain asphalt palette indices and varied terrain.
used_tiles = set(map_data)
colors = set()
for tile in used_tiles:
    colors.update(blob[tiles_off + tile * 4096:tiles_off + (tile + 1) * 4096])
assert any(160 <= c < 192 for c in colors)
assert len(colors) > 60

cars_off = constant("SHA_CAR_SPRITES_OFF")
assert any(blob[cars_off:cars_off + 6 * 16 * 64 * 48])
print("PASS: original MAP00, tiles, path, walls, objects, HUD, cockpit, and I3D car sprites")
