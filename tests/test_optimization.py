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
assert "_sh2_floor_row" in math_s
assert "mov     #40,r14" in math_s          # 80 samples / 2x unroll
assert ".Lf0_rom" in math_s and ".Lf1_rom" in math_s  # two unrolled sample blocks
assert "mov.l   @(36,r4),r4" in math_s      # step_y hoisted out of the loop
assert "_sh2_floor_row_far" in math_s and ".Lffar_loop" in math_s  # far-row kernel
assert "FLOOR_FAR_ROWS" in render and "sh2_floor_row_far(&floor_job)" in render
# SDRAM caches for the render hot paths (records, sprite meta, sprite pixels).
assert "wall_cache" in assets and "obstacle_cache" in assets
assert "sprite_meta_cache" in assets and "sprite_cache" in assets
assert "out->walls = CACHE_THROUGH(wall_cache)" in assets
assert "out->obstacles = CACHE_THROUGH(obstacle_cache)" in assets
assert "sprite_cache_map" in assets and "build_sprite_cache" in assets
assert "packed_shade_cache" in assets and "sha_packed_shade_row" in assets
assert "sha_packed_shade_row(level)" in render
assert "mov.l   @(r0,r10),r0" in math_s  # packed color load in floor kernel
# HUD fast path: on-screen sprites skip per-pixel bounds checks.
assert "x + sp.width <= 320" in render and "y + sp.height <= 224" in render
assert "car_collision_count" in (root / "src/sh_game.c").read_text()
assert "223 * 320 + 309" in render  # car-collision QA probe
# Split screen: two stacked viewports, a second player car, and pad 2 input.
assert "split_viewport" in render and "render_split" in render
assert "player2" in (root / "src/sh_game.h").read_text()
assert "drive_player2" in (root / "src/sh_game.c").read_text()
assert "platform_read_pad2" in (root / "src/platform/platform.h").read_text()
assert "MARS_SYS_COMM10" in (root / "src/platform/platform_32x.c").read_text()
# The slave floor must always receive a viewport or the single-player floor
# reads an uninitialized cam.vp and glitches.
assert "cam.vp = full_viewport();" in render
# Split-screen player 2 has its own chase/high camera angle so its viewport
# follows player 2, not player 1's shared camera.
assert "camera_angle2" in (root / "src/sh_game.h").read_text()
assert "camera_angle2" in (root / "src/sh_game.c").read_text()
assert "p == &game->player2 ? game->camera_angle2" in render
# AI-driven player 2 exposes a movement heading so its chase camera turns.
assert "car->movangle = car->angle;" in (root / "src/sh_game.c").read_text()
assert "player2.movangle - game->camera_angle2" in render  # p2 camera-turn probe
# Player 2 is human-controlled when a second pad is present; AI only when no
# pad is connected (presence, not "any button held", stops the AI from grabbing
# the car whenever the human releases input).
assert "platform_pad2_present" in (root / "src/platform/platform.h").read_text()
assert "game->p2_human" in (root / "src/sh_game.c").read_text()
assert "game->p2_human" in (root / "src/sh_render.c").read_text()
assert "ori.w   #0x8000,d2" in (root / "src/platform/m68k_crt1.s").read_text()
assert "p2_human" in (root / "src/sh_game.h").read_text()
assert "mov.l   r0,@r5" in math_s and "mov.l   r0,@r6" in math_s
assert "sh2_floor_row(&floor_job)" in render
assert "CACHE_THROUGH" in assets
assert "aligned(16)" in assets
assert "SHA_TILE_CACHE_COUNT" in assets
assert "sector_vertex_cache" in assets and "sector_side_cache" in assets
assert "sector_object_meta_cache" in assets and "sector_object_index_cache" in assets
assert "default_object_bin_meta_cache" in assets and "default_object_bin_index_cache" in assets
assert "out->sector_sides = CACHE_THROUGH(sector_side_cache)" in assets
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

# Build-time SEC_TO_MAP plus complete topology/collision/static-camera imports.
assert constant("SHA_MAP0_CAMERA_COUNT") == 11
assert constant("SHA_MAP1_CAMERA_COUNT") == 36
assert constant("SHA_MAP0_COLLISION_WALL_COUNT") == 76
assert constant("SHA_MAP1_COLLISION_WALL_COUNT") == 123
assert constant("SHA_MAP0_SECTOR_COUNT") == 67
assert constant("SHA_MAP1_SECTOR_COUNT") == 91
assert constant("SHA_MAP0_SECTOR_SIDE_COUNT") == 308
assert constant("SHA_MAP1_SECTOR_SIDE_COUNT") == 394
assert "old_sector_data.side_count" in (root / "src/sh_game.c").read_text()
assert "MAX_VISIBLE_SECTORS 20" in render
assert "candidate_depth" in render and "candidate_index" in render
assert "MAX_CANDIDATES" in render
assert "build_visible_sectors" in render and "portal_may_be_visible" in render
assert "cam->visible_wall_count" in render
obstacle_loop = render[render.index("static uint16_t draw_obstacles"):
                       render.index("static void draw_effects")]
assert "sector_object_bucket_count" in assets
assert "sha_get_sector_object_range" in obstacle_loop
assert "sha_get_default_object_bin_range" in obstacle_loop
assert "bucket_index < 256" in obstacle_loop
assert "candidate_mask" in obstacle_loop
assert obstacle_loop.index("if (!(candidate_mask") < obstacle_loop.index("ob.x = sha_rd32")
assert "% 320" not in render[render.index("static void draw_background"):
                                  render.index("static void draw_floor")]
platform = (root / "src/platform/platform_32x.c").read_text()
assert "0xFFFFFE80" in platform and "0xA527" in platform
assert "platform_profile_timer_init" in (root / "src/platform/slave.c").read_text()
assert "floor_slave" in render and "floor_wait" in render
assert "render_profile" in render and "write_profile_probes" in render
for track in range(2):
    wall_off = constant(f"SHA_MAP{track}_WALLS_OFF")
    wall_count = constant(f"SHA_MAP{track}_WALL_COUNT")
    flags = [int.from_bytes(blob[wall_off + i * 20 + 18:wall_off + i * 20 + 20], "little")
             for i in range(wall_count)]
    assert sum(bool(flag & 1) for flag in flags) == constant(f"SHA_MAP{track}_COLLISION_WALL_COUNT")

print("PASS: divider/DDA, SDRAM caches, portal work lists, phase probes and cameras")
