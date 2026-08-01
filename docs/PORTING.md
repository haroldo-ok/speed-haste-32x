# Speed Haste → 32X porting notes

## Source-first implementation

The current ROM is based on the behavior and formats in the original source release, not on the discarded prototype renderer.

| Original source | 32X port |
|---|---|
| `game/3dfloor.c` | `sh_render.c::draw_floor()` camera height/focus/radius calculations |
| `game/fla.asm` | packed map/tile lookup and low-detail expansion |
| `game/racemap.c` | converted 128×128 map, rotated 64×64 tiles, PATH points and starts |
| `game/sectors.c` | converted sector walls and projected wall columns |
| `game/flsprs.c` | projected trackside IS2 sprites and directional cars |
| `wcgsl/is2code.c` | build-time IS2 decompression |
| `wcgsl/object3d.c` | build-time B3D/I3D pointer-image parser |
| `game/userctl.c` | gear ratios, RPM, automatic shifts, braking/reverse, steering and ground drag |
| `game/cars.c` | PATH target selection, AI steering and target speeds |
| `game/race.c` | 70 Hz catch-up simulation, countdown, ranking, cameras, pause and finish flow |
| `game/hud.c` | original cockpit/HUD/countdown IS2 assets and local textured map |

`tools/import_speed_haste.py` reads the JCL directory exactly as `jclib.c` does. It imports:

- MAP00 Racer's Edge: 115 tile/rotation combinations, 88 PATH points, 20 starts, 67 sectors/308 topology sides, 86 rendered wall sides and 263 objects;
- MAP01 The City: 190 combinations, 186 PATH points, 20 starts, 91 sectors/394 topology sides, 123 rendered wall sides and 325 objects;
- 112 shared IS2 graphics, both panoramas/cockpits, and 12 original cars.

## Coordinate and fixed-point conventions

The port keeps the notable original conventions:

- world positions are wrapping unsigned 32-bit values;
- the map is 128×128 pointers to 64×64 tiles;
- floor coordinates use a 7.6.19 split;
- angles cover 0–65535;
- cosine/sine use 2.30 fixed point;
- the original `Sin(a) == Cos(a + 16384)` orientation is retained;
- road palette indices 160–191 are driveable ground;
- PATH speed and direction values come directly from `MAP00.PTH` or `MAP01.PTH`;
- each track retains a separate byte-indexed tile atlas, avoiding slower 16-bit map indices.

## Rendering architecture

The displayed viewport is the original 320×200 image centered inside 320×224 NTSC output.

`RenderView()` starts the floor at `hsk + y`, and passes that exact scanline as `cy` to `SEC_Render()`/`FSP`. The port now does the same: `PROJ_Y = VIEW_Y + SKY_H` is used by the floor, wall bottoms, IS2 ground hotspots and I3D-derived cars. Using screen center here was a 30-pixel error that made scenery appear detached from the ground plane.

1. The master SH-2 submits the perspective floor job to the slave SH-2.
2. The master draws the selected track's scrolling `NUBES*.PIX`/`MOUNT*.PIX` panorama in parallel.
3. The processors synchronize.
4. The master draws sector walls, visible IS2 objects, cars, cockpit/HUD and overlays.
5. The hidden framebuffer is flipped at VBlank.

The job descriptor lives in cache-through SDRAM at `0x26030000`, avoiding SH-2 cache coherency failures.

`SEC_Render()` originally builds a bounded breadth-first sector list. The port mirrors that architecture with a 20-sector low-detail limit and a conservative 120-degree portal cone. Wall IDs are deduplicated and restored to source order. Explicit scenery uses per-sector ranges; objects in the source engine's implicit default sector are additionally grouped into 16×16 wrapping world bins. A source-order candidate mask is built before projection. In the pinned City view this reduces wall work from 123 to 18 records and object transforms from 325 to 89 candidates.

The SH7604 watchdog runs independently on both SH-2s at `/8192`. Calibrated probes separate master panorama time, slave floor time and master synchronization wait. The current frame is 139 master-critical ticks: visibility 3, panorama 44, floor wait 9, walls 25, objects 35, cars/effects 7 and HUD 16. The overlapping slave floor takes 53 ticks, proving that floor—not panorama—is now the longer parallel task.

## Car rendering

Rendering the B3D car polygons at runtime cost about two VBlanks per scene. The original `FS3` system already supports 9/17-angle bitmap vehicles. The importer therefore rotates and rasterizes each original low-detail I3D car into 16 palette-indexed views. Runtime selection uses relative car/camera angle and the same perspective size calculation as `FSP_AddObj()`.

This retains the original geometry and palette while substantially improving speed. Directional selection includes the original `0x8000 - camera_angle + car_angle` transform; omitting the 180-degree term showed a car's front from a trailing camera.

## Camera behavior and forward flow

Chase and high cameras retain their own angle and converge on the player's movement angle by `delta / 16`, matching `HandleView()`. This allows the camera to lag during steering and Stock slides instead of snapping to the player's forward axis every rendered frame. The radius-zero cockpit camera intentionally uses the body angle directly.

A second issue was the world-Y sign. Speed Haste movement uses `dy = Sin(angle)`, where `Sin(a) = Cos(a + 16384)` and therefore has inverted mathematical Y. Every original heading reconstruction calls `GetAngle(dx, -dy)`. Calling the port's equivalent with `+dy` mirrored the movement heading after turns; the chase camera then converged toward the mirrored direction and stationary scenery appeared to move toward the horizon. Player movement, PATH/lap checks and AI targets now all preserve the original `-dy` call.

The regression test also proves that a fixed point ahead of a forward-moving chase camera decreases in depth and projects downward—toward the viewer.

## Collision and effects

The importer now retains the complete `MAPxx.SEC` topology instead of reducing it to drawable walls: 107/136 vertices, 67/91 polygons and 308/394 sides for Racer's Edge/The City. Each 12-byte sector record stores its first side, side count, source flags and bounding box. Each 8-byte side stores two vertex indices, the adjacent sector and an optional rendered-wall index. `sha_find_sector()` reproduces `SEC_IsInSector()`'s half-open ray test and tries the car's current polygon and direct neighbours before the rare full-map fallback. All cars retain their current sector; AI remains PATH-constrained and skips the player's boundary response.

Wall contact uses the asymmetric `PlayerBounds` dimensions from `userctl.c`: `0xB00000` front, `-0x800000` rear and `±0x380000` width, transformed by body angle. The four endpoint rectangle edges and old/mid/new corner positions are tested against only the current sector's collidable sides. Midpoint classification prevents a high-speed 70 Hz step from tunnelling through a thin outside region, while road-polygon overlaps remain passable as in `userctl.c`. A corner's destination sector provides the road/default classification.

Racer's Edge crosses the signed representation seam at world `0x80000000`. Converting each endpoint or rectangle corner separately produced ten phantom 15–16K-unit MAP00 walls even though every real wrapped wall delta is below 1.4K units. Collision now subtracts unsigned wrapping coordinates first and expresses walls, old corners and new corners in one car-relative frame. The automated MAP00 route covers all 88 PATH points and reports no collision (`0xFFFF`).

Collision response still mirrors movement around the source wall angle, halves slide speed, reduces RPM and enters the timed `slidcounter` recovery. A rollback can itself remain embedded, so the previous centre determines the road-facing wall normal and the rectangle's projected support radius determines depenetration distance. A four-unit release margin prevents immediate recontact. Outward/tangential movement is released without another reflection, and steering stays active during `slidcounter`, matching the unconditional body-angle turn in the DOS source. Integer square-root normalisation occurs only after actual contact, not in the ordinary local-sector scan.

Player/AI overlap uses the original 7/8 momentum-loss rule and separation impulse. Collisions spawn the original `SPRK01AA..06AA` sequence; off-road movement uses the `GND*` smoke families, and hard Stock braking/sliding leaves persistent ground skid marks.

AI remains PATH-constrained as in `cars.c`; it updates its sector hint but deliberately does not run the player's rectangular boundary-response pass.

## Trackside cameras

MAP thing type `0xF2xx` is imported as an original static camera with height `900 + xx*500`. Camera mode four selects the nearest camera, aims with `GetAngle(dx, -dy)`, and derives horizon/focus from distance following `HandleView()`.

## D32XR-inspired optimization

- SH7604 hardware divider wrappers replace software integer division in fixed-point hot paths.
- Wall top/bottom/texture interpolation is precomputed as DDA steps outside the column loop.
- Constant scales use shifts (`<< 12`, `<< 7`) in map/tile addressing.
- MAP wall coordinates are converted at build time.
- Physics traverses the current sector's compact side range rather than every track wall; adjacency is tested before a full polygon search.
- The importer frequency-sorts tile/rotation combinations. A 28-tile aligned SDRAM cache covers 95% of Racer's Edge and 92% of The City cells.
- Current map indices, panorama, mountains, shade table, sector topology, object ranges and default-bin indices are copied through cache-through SDRAM addresses, following D32XR's hot-lump/cache strategy.
- A source-style 20-sector traversal builds compact visible wall/object work lists.
- A hand-written SH-2 floor kernel holds map/tile/shade pointers and coordinates in registers for 80 samples, selects cached versus ROM tiles, and emits both scanlines with aligned packed writes.
- Panorama rows process four pixels per aligned write. Only one group can cross the horizontal seam; mountain transparency falls back to byte writes only for mixed groups.
- Floor shade-row selection and all perspective-row calculations remain outside the assembly horizontal loop.
- Obstacle/wall packed records are traversed directly instead of rebuilding track descriptors in each iteration.

## Timing

The first faithful build advanced one 70 Hz simulation step per completed rendered frame, creating severe slow motion at 3–4 fps. The fixed loop reads the 68000 VBlank counter and performs all due 70 Hz world ticks before rendering, matching `race.c`'s timer catch-up design.

Measured with the pinned PicoDrive core:

- first source-based renderer: approximately 3–4 completed fps, with slow-motion physics;
- Racer's Edge / Formula One optimized renderer: 11.6 completed fps;
- The City / Stock after collision/effect/cache pass: 12.0 completed fps;
- The City / Stock after visible-sector/object filtering: 12.7 completed fps;
- The City / Stock after SH-2 floor assembly, default-object bins and packed panorama writes: **15.0 completed fps**;
- simulation: independent 70 Hz, so speed, countdown and race time are correct.

## Stock handling

`CARS.LST` supplies separate 240–260 km/h Stock ratings. The player uses that class reference speed in the same gearbox equations. Unlike Formula One, `userctl.c` permits `cartype == 1` to enter the sliding path under hard high-speed steering/braking; the port retains separate movement angle, body angle, slide speed and progressive traction recovery.

## Current scope

Implemented: both shareware circuits, Formula One and Stock classes, all 12 cars, four computer rivals, setup menus, four camera modes including original TV positions, both cockpits/HUDs, minimap, automatic gearbox, Stock sliding, guardrail/car collisions, sparks/smoke/skids, reverse, countdown, laps, position, pause and finish.

Not yet ported: two-player split screen, persistent records, modem/IPX code, S3M music, VTAL effects, and the six registered-only circuits absent from the supplied shareware archive.
