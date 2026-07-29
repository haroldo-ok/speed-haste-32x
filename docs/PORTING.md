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

- MAP00 Racer's Edge: 115 tile/rotation combinations, 88 PATH points, 20 starts, 86 wall sides and 263 objects;
- MAP01 The City: 190 combinations, 186 PATH points, 20 starts, 123 wall sides and 325 objects;
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

## Car rendering

Rendering the B3D car polygons at runtime cost about two VBlanks per scene. The original `FS3` system already supports 9/17-angle bitmap vehicles. The importer therefore rotates and rasterizes each original low-detail I3D car into 16 palette-indexed views. Runtime selection uses relative car/camera angle and the same perspective size calculation as `FSP_AddObj()`.

This retains the original geometry and palette while substantially improving speed. Directional selection includes the original `0x8000 - camera_angle + car_angle` transform; omitting the 180-degree term showed a car's front from a trailing camera.

## Camera behavior and forward flow

Chase and high cameras retain their own angle and converge on the player's movement angle by `delta / 16`, matching `HandleView()`. This allows the camera to lag during steering and Stock slides instead of snapping to the player's forward axis every rendered frame. The radius-zero cockpit camera intentionally uses the body angle directly.

A second issue was the world-Y sign. Speed Haste movement uses `dy = Sin(angle)`, where `Sin(a) = Cos(a + 16384)` and therefore has inverted mathematical Y. Every original heading reconstruction calls `GetAngle(dx, -dy)`. Calling the port's equivalent with `+dy` mirrored the movement heading after turns; the chase camera then converged toward the mirrored direction and stationary scenery appeared to move toward the horizon. Player movement, PATH/lap checks and AI targets now all preserve the original `-dy` call.

The regression test also proves that a fixed point ahead of a forward-moving chase camera decreases in depth and projects downward—toward the viewer.

## Timing

The first faithful build advanced one 70 Hz simulation step per completed rendered frame, creating severe slow motion at 3–4 fps. The fixed loop reads the 68000 VBlank counter and performs all due 70 Hz world ticks before rendering, matching `race.c`'s timer catch-up design.

Measured with the pinned PicoDrive core:

- first source-based renderer: approximately 3–4 completed fps, with slow-motion physics;
- Racer's Edge / Formula One optimized renderer: 11.6 completed fps;
- The City / Stock stress configuration: 10.0 completed fps;
- simulation: independent 70 Hz, so speed, countdown and race time are correct.

## Stock handling

`CARS.LST` supplies separate 240–260 km/h Stock ratings. The player uses that class reference speed in the same gearbox equations. Unlike Formula One, `userctl.c` permits `cartype == 1` to enter the sliding path under hard high-speed steering/braking; the port retains separate movement angle, body angle, slide speed and progressive traction recovery.

## Current scope

Implemented: both shareware circuits, Formula One and Stock classes, all 12 cars, four computer rivals, multi-step setup menus, three cameras, both cockpits/HUDs, minimap, automatic gearbox, Stock sliding, reverse, countdown, laps, position, pause and finish.

Not yet ported: two-player split screen, persistent records, modem/IPX code, S3M music, VTAL effects, and the six registered-only circuits absent from the supplied shareware archive.
