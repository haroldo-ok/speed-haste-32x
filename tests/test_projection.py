#!/usr/bin/env python3
"""Regression checks for Speed Haste's shared floor/object perspective."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
render = (root / "src/sh_render.c").read_text()
game = (root / "src/sh_game.c").read_text()


def macro(name: str) -> str:
    m = re.search(rf"^#define\s+{name}\s+(.+)$", render, re.MULTILINE)
    assert m, f"missing {name}"
    return m.group(1).strip()


# The original RenderView calls both floor and SEC/FSP rendering at hsk+y.
# Single-player and split viewports share this projection base via cam->vp.
assert "full_viewport" in render and "split_viewport" in render
assert "v.proj_y = 12 + 70" in render          # full-screen floor base
assert "v.proj_y = v.y + 35" in render         # split half-height floor base
assert "const int floor_y = cam->vp.proj_y;" in render
for expected in (
    "sy[i] = (int16_t)(cam->vp.proj_y",
    "y = cam->vp.proj_y + muldiv",
    "bottom0 = cam->vp.proj_y - cam->horizon",
):
    assert expected in render, f"object projection diverged from floor: {expected}"

# Algebraic point-to-point check. A point sampled by floor row R must project
# as a zero-height object back to that same row. These are F3D/FSP integer ops.
height, focus, horizon, projection_y = 1848, 3136, 1, 12 + 70
for row in (0, 1, 5, 10, 30, 65, 100, 129):
    Y = horizon + row
    radius = height * focus // Y
    # F3D uses >>22 with 2.30 cosine, then FSP converts world to >>6 depth.
    world_forward = (radius * (1 << 30)) >> 22
    depth = world_forward >> 6
    projected = projection_y + (height << 2) * focus // depth - horizon
    assert projected == projection_y + row, (row, projected)

# Directional car selection must include the same 180-degree model rotation as
# flsprs.c: a = 0x8000 - camera.angle + object.angle.
assert "0x8000u - cam->angle + car->angle" in render
# IS2 world sprites share the projected anchor and apply their authored hotspot.
assert "center_x - muldiv(sp->dx, width, sp->width)" in render
assert "base_y - muldiv(sp->dy, height, sp->height)" in render
# World Y is inverted. The original source always reconstructs headings with
# GetAngle(dx, -dy); using +dy mirrors the camera/AI direction after steering.
assert game.count("vector_angle(dx, -dy)") == 4
assert "vector_angle(dx, dy)" not in game

# Point-to-point direction check for the engine convention:
# movement=(Cos(a), Sin(a))=(cos(a), -sin(a)); GetAngle consumes (dx, -dy).
def approximate_angle(dx: int, dy: int) -> int:
    ax, ay = abs(dx), abs(dy)
    if ax | ay == 0:
        return 0
    base = ay * 8192 // ax if ax >= ay else 16384 - ax * 8192 // ay
    if dx >= 0 and dy >= 0: return base
    if dx < 0 <= dy: return 32768 - base
    if dx < 0: return 32768 + base
    return (-base) & 0xFFFF

import math
for angle in range(0, 65536, 4096):
    dx = round(math.cos(angle * 2 * math.pi / 65536) * (1 << 20))
    world_dy = round(-math.sin(angle * 2 * math.pi / 65536) * (1 << 20))
    rebuilt = approximate_angle(dx, -world_dy)
    assert abs(((rebuilt - angle + 32768) & 0xFFFF) - 32768) < 800

# A fixed point in front of a forward-moving chase camera must get closer and
# project downward/toward the viewer, never recede toward the horizon.
camera_radius = 20 << 20
object_ahead = 30 << 20
forward_move = 3 << 20
depth_before = camera_radius + object_ahead
depth_after = depth_before - forward_move
assert depth_after < depth_before
screen_before = projection_y + (height << 2) * focus // (depth_before >> 6) - horizon
screen_after = projection_y + (height << 2) * focus // (depth_after >> 6) - horizon
assert screen_after > screen_before

# Chase/high cameras must retain state and converge by 1/16, not snap to player.
assert "delta / 16" in game
assert "*angle + step" in game          # per-player camera converges by 1/16
assert "camera_angle2" in game          # split player 2 keeps its own camera
print("PASS: shared perspective, forward scene flow, camera lag and rear-view orientation")
