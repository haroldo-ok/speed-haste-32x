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

`tools/import_speed_haste.py` reads the JCL directory exactly as `jclib.c` does. For MAP00 it imports 115 rotated tile combinations, 88 path points, 20 starts, 86 textured wall sides, 263 visible decorations and 106 IS2 graphics.

## Coordinate and fixed-point conventions

The port keeps the notable original conventions:

- world positions are wrapping unsigned 32-bit values;
- the map is 128×128 pointers to 64×64 tiles;
- floor coordinates use a 7.6.19 split;
- angles cover 0–65535;
- cosine/sine use 2.30 fixed point;
- the original `Sin(a) == Cos(a + 16384)` orientation is retained;
- road palette indices 160–191 are driveable ground;
- PATH speed and direction values come directly from `MAP00.PTH`.

## Rendering architecture

The displayed viewport is the original 320×200 image centered inside 320×224 NTSC output.

1. The master SH-2 submits the perspective floor job to the slave SH-2.
2. The master draws the scrolling `NUBES0.PIX`/`MOUNT0.PIX` panorama in parallel.
3. The processors synchronize.
4. The master draws sector walls, visible IS2 objects, cars, cockpit/HUD and overlays.
5. The hidden framebuffer is flipped at VBlank.

The job descriptor lives in cache-through SDRAM at `0x26030000`, avoiding SH-2 cache coherency failures.

## Car rendering

Rendering the B3D car polygons at runtime cost about two VBlanks per scene. The original `FS3` system already supports 9/17-angle bitmap vehicles. The importer therefore rotates and rasterizes each original low-detail I3D car into 16 palette-indexed views. Runtime selection uses relative car/camera angle and the same perspective size calculation as `FSP_AddObj()`.

This retains the original geometry and palette while substantially improving speed.

## Timing

The first faithful build advanced one 70 Hz simulation step per completed rendered frame, creating severe slow motion at 3–4 fps. The fixed loop reads the 68000 VBlank counter and performs all due 70 Hz world ticks before rendering, matching `race.c`'s timer catch-up design.

Measured with the pinned PicoDrive core:

- first source-based renderer: approximately 3–4 completed fps, with slow-motion physics;
- current optimized renderer: 11.6 completed fps;
- simulation: independent 70 Hz, so speed, countdown and race time are correct.

## Current scope

Implemented: Racer's Edge, Formula One, four computer rivals, three cameras, cockpit/HUD, minimap, automatic gearbox, reverse, countdown, laps, position, pause and finish.

Not yet ported: The City, Stock cars, the complete DOS menu graph, two-player split screen, persistent records, modem/IPX code, S3M music and VTAL effects.
