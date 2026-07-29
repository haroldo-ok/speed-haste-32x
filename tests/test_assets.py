#!/usr/bin/env python3
"""Validate conversion of both original Speed Haste shareware circuits/classes."""
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
assert constant("SHA_TRACK_COUNT") == 2
expected = (
    (0, 115, 88, 20, 86, 263, "Racer's Edge"),
    (1, 190, 186, 20, 123, 325, "The City"),
)
for track, tiles, path, starts, walls, objects, title in expected:
    assert constant(f"SHA_MAP{track}_TILE_COUNT") == tiles
    assert constant(f"SHA_MAP{track}_PATH_COUNT") == path
    assert constant(f"SHA_MAP{track}_START_COUNT") == starts
    assert constant(f"SHA_MAP{track}_WALL_COUNT") == walls
    assert constant(f"SHA_MAP{track}_OBSTACLE_COUNT") == objects
    assert title in manifest

assert constant("SHA_SPRITE_COUNT") >= 110
assert len(blob) > 2_400_000
for required in (
    "SHA_COCKPIT0_OFF", "SHA_COCKPIT1_OFF", "SHA_MENU_MAIN_OFF",
    "SHA_MENU_CIRCUIT_OFF", "SHA_MENU_CAR_OFF", "SHA_CAR_SPRITES_OFF",
):
    constant(required)

for track, expected_tiles, *_rest in expected:
    map_off = constant(f"SHA_MAP{track}_MAP128_OFF")
    tiles_off = constant(f"SHA_MAP{track}_TILES_OFF")
    map_data = blob[map_off:map_off + 128 * 128]
    assert len(set(map_data)) == expected_tiles
    colors = set()
    for tile in set(map_data):
        colors.update(blob[tiles_off + tile * 4096:tiles_off + (tile + 1) * 4096])
    assert any(160 <= c < 192 for c in colors)
    assert len(colors) > 60

# 2 classes × 6 original I3D models × 16 directions × 64×48 pixels.
cars_off = constant("SHA_CAR_SPRITES_OFF")
car_bytes = 2 * 6 * 16 * 64 * 48
cars = blob[cars_off:cars_off + car_bytes]
assert len(cars) == car_bytes and any(cars)
# Both classes must contain distinct visual data.
half = car_bytes // 2
assert cars[:half] != cars[half:]
print("PASS: MAP00/MAP01, both cockpits, HUD/menu assets, and 12 I3D cars")
