#!/usr/bin/env python3
"""Regression checks for wall release and source-faithful crash recovery."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
game = (root / "src/sh_game.c").read_text()
render = (root / "src/sh_render.c").read_text()
e2e = (root / "tests/test_emulator.py").read_text()


def define(name: str) -> int:
    match = re.search(rf"^#define\s+{name}\s+(\d+)$", game, re.MULTILINE)
    assert match, name
    return int(match.group(1))


radius = define("WALL_COLLISION_RADIUS")
release = define("WALL_RELEASE_RADIUS")
assert release > radius, "wall release needs hysteresis beyond contact radius"

# Contacts are resolved toward the previous (road) side. Outward/tangent
# motion depenetrates without another reflection, which is the core anti-stick
# rule. Expensive normalisation remains behind broad/narrow-phase contact.
assert "nearest_wall_point(old_cx, old_cy" in game
assert "normal_motion >= 0" in game
assert "release_from_wall(car, qx, qy, nx, ny);" in game
collision = game[game.index("static int collide_with_wall"):game.index("static void player_tick")]
assert collision.index("dx * dx + dy * dy < WALL_COLLISION_RADIUS") < collision.index("release_from_wall")

# userctl.c's omitted slidcounter path caused reflected movement to recover
# immediately toward a body still aimed into the wall. It must rotate/decay
# first, while the normal steering turn remains unconditional.
for source_fragment in (
    "if (p->slidcounter > 0)",
    "p->angle = (uint16_t)(p->angle + p->slidva);",
    "p->slidspeed -= p->slidspeed >> 7;",
    "muldiv(reflection >> 4, car->slidspeed,",
    "p->angle = (uint16_t)(p->angle + muldiv(p->va, p->v, 1 << 22));",
):
    assert source_fragment in game, source_fragment
assert "if (p->v && !p->sliding)" not in game

# Point-to-point model for a vertical guardrail (road is x > 0). An inward
# impact bounces and releases at x=release; the next outward step is clear. If
# a saved state begins embedded, an outward command depenetrates without a
# second bounce back into the wall.
def contact(old_x: int, new_x: int) -> tuple[int, bool]:
    if abs(new_x) >= radius:
        return new_x, False
    road_normal = 1 if old_x > 0 else (-1 if old_x < 0 else -1 if new_x > 0 else 1)
    inward = (new_x - old_x) * road_normal < 0
    return road_normal * release, inward


position, bounced = contact(40, 30)
assert bounced and position == release
position, bounced = contact(position, position + 4)
assert not bounced and position > radius
position, bounced = contact(30, 31)
assert not bounced and position == release
position, bounced = contact(-30, -31)
assert not bounced and position == -release

# E2E exposes source-wall and position probes, then requires continued movement
# after steering away from the wall.
for probe in (313, 314, 315):
    assert f"223 * 320 + {probe}" in render
assert "wall_recovery_position_changes" in e2e
assert "car remained pinned to wall" in e2e

print("PASS: wall depenetration, release hysteresis, crash recovery and E2E escape probe")
