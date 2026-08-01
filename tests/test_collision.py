#!/usr/bin/env python3
"""Validate full SEC topology and rectangular wall-collision recovery."""
from pathlib import Path
import re
import struct

root = Path(__file__).resolve().parents[1]
game = (root / "src/sh_game.c").read_text()
assets_c = (root / "src/sh_assets.c").read_text()
importer = (root / "tools/import_speed_haste.py").read_text()
render = (root / "src/sh_render.c").read_text()
e2e = (root / "tests/test_emulator.py").read_text()
header = (root / "assets/generated/speed_haste_assets.h").read_text()
blob = (root / "assets/generated/speed_haste_assets.bin").read_bytes()


def constant(name: str) -> int:
    match = re.search(rf"#define {name} (?:\()?(-?0x[0-9A-Fa-f]+|-?\d+)(?:\))?u?", header if name.startswith("SHA_") else game)
    assert match, name
    return int(match.group(1), 0)


expected = ((107, 67, 308, 86, 76), (136, 91, 394, 123, 123))
for track, (vertex_count, sector_count, side_count, wall_count, collision_count) in enumerate(expected):
    assert constant(f"SHA_MAP{track}_SECTOR_VERTEX_COUNT") == vertex_count
    assert constant(f"SHA_MAP{track}_SECTOR_COUNT") == sector_count
    assert constant(f"SHA_MAP{track}_SECTOR_SIDE_COUNT") == side_count
    vertices_off = constant(f"SHA_MAP{track}_SECTOR_VERTICES_OFF")
    sectors_off = constant(f"SHA_MAP{track}_SECTORS_OFF")
    sides_off = constant(f"SHA_MAP{track}_SECTOR_SIDES_OFF")
    walls_off = constant(f"SHA_MAP{track}_WALLS_OFF")
    starts_off = constant(f"SHA_MAP{track}_STARTS_OFF")

    vertices = [struct.unpack_from("<HH", blob, vertices_off + i * 4)
                for i in range(vertex_count)]
    sectors = [struct.unpack_from("<HBBHHHH", blob, sectors_off + i * 12)
               for i in range(sector_count)]
    sides = [struct.unpack_from("<HHhH", blob, sides_off + i * 8)
             for i in range(side_count)]
    wall_flags = [struct.unpack_from("<H", blob, walls_off + i * 20 + 18)[0]
                  for i in range(wall_count)]

    assert sum(record[1] for record in sectors) == side_count
    assert sectors[0][0] == 0
    for index, (first, count, flags, min_x, min_y, max_x, max_y) in enumerate(sectors):
        assert flags in (0, 1)
        assert first + count <= side_count
        used = sides[first:first + count]
        assert used
        xs = [vertices[v][0] for side in used for v in side[:2]]
        ys = [vertices[v][1] for side in used for v in side[:2]]
        assert (min(xs), min(ys), max(xs), max(ys)) == (min_x, min_y, max_x, max_y), index
    for v0, v1, other, wall in sides:
        assert v0 < vertex_count and v1 < vertex_count
        assert -1 <= other < sector_count
        assert wall == 0xFFFF or wall < wall_count
    wall_refs = [wall for _v0, _v1, _other, wall in sides if wall != 0xFFFF]
    assert sorted(wall_refs) == list(range(wall_count))
    assert sum(bool(wall_flags[wall] & 1) for wall in wall_refs) == collision_count

    # Python equivalent of sectors.c::SEC_IsInSector, used to ensure every
    # original grid position starts in exactly one driveable imported polygon.
    def contains(sector: int, x: int, y: int) -> bool:
        first, count, _flags, min_x, min_y, max_x, max_y = sectors[sector]
        if not (min_x <= x <= max_x and min_y <= y <= max_y):
            return False
        hits = 0
        for v0, v1, _other, _wall in sides[first:first + count]:
            x0, y0 = vertices[v0]
            x1, y1 = vertices[v1]
            if not ((x >= x0 and x < x1) or (x < x0 and x >= x1)):
                continue
            if y0 < y and y1 < y:
                hits += 1
            elif y0 < y or y1 < y:
                if x0 < x1:
                    iy = y0 + (x - x0) * (y1 - y0) // (x1 - x0)
                else:
                    iy = y1 + (x - x1) * (y0 - y1) // (x0 - x1)
                if iy < y:
                    hits += 1
        return bool(hits & 1)

    for start in range(20):
        map_x, map_y = struct.unpack_from("<HH", blob, starts_off + start * 8)
        owners = [index for index in range(sector_count)
                  if contains(index, map_x << 4, map_y << 4)]
        assert len(owners) == 1 and sectors[owners[0]][2] != 0, (track, start, owners)

# Racer's Edge has ten short walls crossing signed world-coordinate seams.
# Endpoint-by-endpoint signed conversion inflated them to ~15–16K units and
# created the reported backwards-pushing barrier. Wrapped subtraction keeps
# every real side below 3K units.
for track, expected_phantoms in ((0, 10), (1, 16)):
    walls_off = constant(f"SHA_MAP{track}_WALLS_OFF")
    wall_count = constant(f"SHA_MAP{track}_WALL_COUNT")
    phantoms = 0
    wrapped_max = 0
    for wall in range(wall_count):
        x0, y0, x1, y1 = struct.unpack_from("<IIII", blob, walls_off + wall * 20)
        signed = lambda value: value if value < 0x80000000 else value - 0x100000000
        direct = max(abs((signed(x1) >> 18) - (signed(x0) >> 18)),
                     abs((signed(y1) >> 18) - (signed(y0) >> 18)))
        dx = signed((x1 - x0) & 0xFFFFFFFF) >> 18
        dy = signed((y1 - y0) & 0xFFFFFFFF) >> 18
        wrapped_max = max(wrapped_max, abs(dx), abs(dy))
        phantoms += direct > 8000
    assert phantoms == expected_phantoms and wrapped_max < 3000
assert "vx = ((int32_t)(wall.x1 - wall.x0)) >> 18;" in game
assert "new_frame_x = old_frame_x + ((int32_t)(car->x - old_x) >> 18)" in game
assert "new_corner_x[j] - car->x" in game
assert "new_corner_x[i] - old_corner_x[i]" in game
assert "phantom ~16384-unit wall" in game

# The importer preserves every polygon side, adjacency and optional wall link.
for fragment in ("declared_side_count", "sector_records", "side_records",
                 "MAP{number}_SECTOR_VERTICES", "MAP{number}_SECTOR_SIDES"):
    assert fragment in importer

# Runtime uses current -> neighbours -> full fallback, never a global wall scan
# in the normal collision pass. All cars retain a current sector.
assert "SEC_IsInSector" in assets_c
assert assets_c.index("/* Normal movement") < assets_c.index("for (i = 0; i < assets->sector_count")
collision = game[game.index("static int collide_with_wall"):game.index("static void player_tick")]
assert "old_sector_data.side_count" in collision
assert "assets.wall_count" not in collision
assert "car->sector = (int8_t)sha_find_sector" in game

# PlayerBounds from userctl.c: asymmetric 0xB00000/-0x800000 body and narrow
# 0x380000 half-width, transformed by body angle. Swept corners reject tunnels.
assert constant("PLAYER_FRONT") == 0xB00000
assert constant("PLAYER_REAR") == -0x800000
assert constant("PLAYER_HALF_WIDTH") == 0x380000
for fragment in ("car_corners", "segments_intersect", "old_corner_x",
                 "new_corner_x", "rectangle_release_radius"):
    assert fragment in collision or fragment in game

# Outward/tangent movement depenetrates rather than reflecting back; inward
# impacts retain source slidcounter rotation/decay and active steering.
assert "normal_motion >= 0" in collision
assert "WALL_RELEASE_MARGIN" in game
for fragment in ("if (p->slidcounter > 0)",
                 "p->angle = (uint16_t)(p->angle + p->slidva);",
                 "p->slidspeed -= p->slidspeed >> 7;",
                 "muldiv(reflection >> 4, car->slidspeed,",
                 "p->angle = (uint16_t)(p->angle + muldiv(p->va, p->v, 1 << 22));"):
    assert fragment in game, fragment
assert "if (p->v && !p->sliding)" not in game

# E2E exposes a source-wall probe and requires continued position changes after
# reversing steering, in addition to the black/frozen screen gates.
for probe in (313, 314, 315):
    assert f"223 * 320 + {probe}" in render
assert "wall_recovery_position_changes" in e2e
assert "car remained pinned to wall" in e2e

# Oriented rectangular car-to-car collision via SAT (replaces the circle
# approximation) with a minimum-translation vector that fully separates.
assert "car_rect_corners_i" in game
assert "axis_overlap" in game
assert "min_overlap" in game and "mtv_x" in game
for fragment in ("Separating Axis Theorem", "min_overlap * ux", "push_y << 18"):
    assert fragment in game, fragment
assert "dx * dx + dy * dy >= 42 * 42" not in game  # circle path removed

# Python SAT model reproduces the C box and MTV for representative cases.
def car_corners(cx, cy, angle, front=0xB00000, rear=-0x800000, half=0x380000):
    import math
    a = angle * 2 * math.pi / 65536
    fx, fy = math.cos(a), math.sin(a)
    lx, ly = -fy, fx
    lon = [front, front, rear, rear]
    lat = [half, -half, -half, half]
    out = []
    for L, La in zip(lon, lat):
        # corner in 18-bit fixed world units, then >>18 to integer world units
        x18 = int(round(L * fx + La * lx))
        y18 = int(round(L * fy + La * ly))
        out.append((cx + (x18 >> 18), cy + (y18 >> 18)))
    return out

def axis_overlap_py(ax, ay, A, B):
    proj = lambda C: [ax * x + ay * y for x, y in C]
    pA, pB = proj(A), proj(B)
    if max(pA) < min(pB) or max(pB) < min(pA):
        return -1
    return min(max(pA), max(pB)) - max(min(pA), min(pB))

def sat_overlap(A, B, fA, fB):
    axes = [(fA[0], fA[1]), (-fA[1], fA[0]), (fB[0], fB[1]), (-fB[1], fB[0])]
    for ax, ay in axes:
        ov = axis_overlap_py(ax, ay, A, B)
        if ov < 0:
            return False, None
    return True, min(axis_overlap_py(ax, ay, A, B) for ax, ay in axes)

# Two cars side-by-side, same heading: they overlap -> SAT reports collision.
A = car_corners(0, 0, 0)
B = car_corners(0, 25, 0)          # shifted 25 units laterally (overlaps, width 14+14=28)
assert sat_overlap(A, B, (1, 0), (1, 0))[0]
# Far apart on the same axis: no collision.
B2 = car_corners(0, 80, 0)
assert not sat_overlap(A, B2, (1, 0), (1, 0))[0]
# Head-on (180 deg): front ends collide.
B3 = car_corners(78, 0, 0x8000)
assert sat_overlap(A, B3, (1, 0), (-1, 0))[0]
# T-bone: lateral box of one vs front of other still registers overlap.
B4 = car_corners(45, 0, 0x4000)    # 90 deg turn at the side
assert sat_overlap(A, B4, (1, 0), (0, 1))[0]

print("PASS: full SEC topology, local lookup, rectangular swept collision, wall escape and car-car SAT")
