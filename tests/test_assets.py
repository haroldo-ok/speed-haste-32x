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
    (0, 115, 88, 20, 11, 86, 76, 107, 67, 308, 263, 126, 17, "Racer's Edge"),
    (1, 190, 186, 20, 36, 123, 123, 136, 91, 394, 325, 116, 23, "The City"),
)
for (track, tiles, path, starts, cameras, walls, collision_walls,
     vertices, sectors, sector_sides, objects, default_objects,
     nonempty_default_bins, title) in expected:
    assert constant(f"SHA_MAP{track}_TILE_COUNT") == tiles
    assert constant(f"SHA_MAP{track}_PATH_COUNT") == path
    assert constant(f"SHA_MAP{track}_START_COUNT") == starts
    assert constant(f"SHA_MAP{track}_CAMERA_COUNT") == cameras
    assert constant(f"SHA_MAP{track}_WALL_COUNT") == walls
    assert constant(f"SHA_MAP{track}_COLLISION_WALL_COUNT") == collision_walls
    assert constant(f"SHA_MAP{track}_SECTOR_VERTEX_COUNT") == vertices
    assert constant(f"SHA_MAP{track}_SECTOR_COUNT") == sectors
    assert constant(f"SHA_MAP{track}_SECTOR_SIDE_COUNT") == sector_sides
    assert constant(f"SHA_MAP{track}_SECTOR_OBJECT_BUCKET_COUNT") == sectors + 1
    assert constant(f"SHA_MAP{track}_DEFAULT_OBJECT_COUNT") == default_objects
    assert constant(f"SHA_MAP{track}_OBSTACLE_COUNT") == objects
    meta_off = constant(f"SHA_MAP{track}_SECTOR_OBJECT_META_OFF")
    index_off = constant(f"SHA_MAP{track}_SECTOR_OBJECT_INDICES_OFF")
    ranges = [tuple(int.from_bytes(blob[meta_off + bucket * 4 + part:
                                        meta_off + bucket * 4 + part + 2], "little")
                    for part in (0, 2)) for bucket in range(sectors + 1)]
    assert ranges[0][0] == 0
    assert sum(count for _first, count in ranges) == objects
    assert ranges[-1][1] == default_objects
    indices = [int.from_bytes(blob[index_off + i * 2:index_off + i * 2 + 2], "little")
               for i in range(objects)]
    assert sorted(indices) == list(range(objects))
    bin_meta_off = constant(f"SHA_MAP{track}_DEFAULT_OBJECT_BIN_META_OFF")
    bin_index_off = constant(f"SHA_MAP{track}_DEFAULT_OBJECT_BIN_INDICES_OFF")
    assert constant(f"SHA_MAP{track}_DEFAULT_OBJECT_BIN_COUNT") == 256
    assert constant(f"SHA_MAP{track}_DEFAULT_OBJECT_NONEMPTY_BINS") == nonempty_default_bins
    bin_ranges = [tuple(int.from_bytes(blob[bin_meta_off + bucket * 4 + part:
                                            bin_meta_off + bucket * 4 + part + 2], "little")
                        for part in (0, 2)) for bucket in range(256)]
    assert sum(count for _first, count in bin_ranges) == default_objects
    assert sum(bool(count) for _first, count in bin_ranges) == nonempty_default_bins
    default_indices = [int.from_bytes(blob[bin_index_off + i * 2:
                                          bin_index_off + i * 2 + 2], "little")
                       for i in range(default_objects)]
    sector_default = indices[ranges[-1][0]:ranges[-1][0] + ranges[-1][1]]
    assert sorted(default_indices) == sorted(sector_default)
    assert title in manifest

assert constant("SHA_SPRITE_COUNT") >= 130
assert constant("SHA_TILE_CACHE_COUNT") == 28
assert len(blob) > 2_400_000
for effect in ("SHSPR_SPRK01AA_IS2", "SHSPR_SPRK06AA_IS2",
               "SHSPR_GND001AA_IS2", "SHSPR_GND106AA_IS2",
               "SHSPR_GND206AA_IS2"):
    assert effect in header
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
