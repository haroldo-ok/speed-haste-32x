# Speed Haste 32X

A source-based Sega 32X port of Javier Arévalo's 1995 DOS racer **Speed Haste**.

## Current status

This revision ports the actual Speed Haste systems and data rather than presenting a generic pseudo-3D racer:

- original `MAP00` circuit, **Racer's Edge**;
- original 64×64 floor tiles and map rotations;
- original floor-casting equations from `3dfloor.c`/`fla.asm`;
- original panorama, mountains, wall textures, trackside sprites and palette shading;
- original Formula One cockpit and HUD graphics;
- directional car sprites pre-rendered from the original I3D models;
- fixed-point gearbox, RPM, steering, reverse and off-road drag based on `userctl.c`;
- path-driven computer racers based on `cars.c` and `racemap.c`;
- original title graphic, countdown, lap/position display, camera modes, pause and finish states.

The race runs at the original **70 Hz simulation rate**, independently of rendering. The optimized renderer completes approximately **11.6 frames/second** in the pinned PicoDrive core—up from roughly 3–4 fps in the first source-based build—with game time and vehicle speed remaining correct.

## Play

Load this ROM with a current PicoDrive/RetroArch core configured for Sega 32X:

**[`release/SpeedHaste32X.32x`](release/SpeedHaste32X.32x)**

The image is 1.5 MiB, has a valid MARS header, and has been exercised from boot through a complete race in PicoDrive.

### Controls

| Genesis/32X pad | Action |
|---|---|
| Left / Right | Steer |
| B or C | Accelerate |
| A or Down | Brake; continue holding for reverse |
| Start | Enter menu, start race, pause/resume |
| X (six-button pad) | Cycle chase, cockpit and high cameras |
| Y (six-button pad) | Toggle HUD |

## Performance work

The port is constrained by the 32X's shared cartridge/framebuffer buses. The current renderer uses:

- master/slave SH-2 parallel floor and panorama rendering;
- 80×65 floor samples expanded in the DOS low-detail style;
- 2×2 trackside sprite sampling;
- low-detail textured walls;
- an eight-frame cached textured minimap;
- I3D cars pre-rendered into the original engine's directional-sprite path;
- 32-bit framebuffer fills and packed writes;
- 70 Hz timer catch-up, as in the original `race.c`, so rendering never causes slow motion.

PWM audio is disabled by default because continuous slave-SH-2 FIFO polling stalls some ARM PicoDrive builds. The slave processor is instead used to accelerate rendering.

## Build

Prerequisites: a POSIX shell, Python 3, `curl`, `make`, and approximately 600 MiB of temporary toolchain/cache space.

```sh
bash tools/build.sh
```

The script installs the pinned 32XDK `20220418` toolchain in the user cache and creates:

```text
release/SpeedHaste32X.32x
```

If 32XDK is already installed:

```sh
make GENDEV=/opt/toolchains/sega
```

### Reimporting original data

The compact runtime asset file was produced from the shareware `SPEEDH.JCL` supplied in the task:

```sh
python3 tools/import_speed_haste.py /path/to/SPEEDH.JCL --out assets/generated
```

The importer follows the structures in `jclib.c`, `racemap.c`, `sectors.c`, `is2code.c`, `object3d.c` and `flsprs.c`. It imports only the assets needed for Racer's Edge and converts the I3D cars to 16 directional frames for 32X performance.

## Automated tests

```sh
make GENDEV=/path/to/toolchains/sega test
```

The suite validates the imported original data, ROM/MARS headers and checksum, then drives the real ROM through PicoDrive:

```text
title → original menu → countdown → race → accelerate → steer
      → pause → resume → lap checkpoints → finish
```

It captures eight screenshots, rejects black/frozen output, verifies visible motion, and requires at least 10 completed frames/second. Latest measured result: **11.6 fps**, with 159,025 RGB bytes changed by acceleration and 148,490 by steering.

See [`test-results/emulator/report.json`](test-results/emulator/report.json).

## Layout

- `src/sh_game.*` — fixed-point gearbox, player, AI, race timing and states
- `src/sh_render.*` — original-style floor, walls, objects, cars, cockpit and HUD
- `src/sh_assets.*` — compact original-data accessors
- `src/platform/` — 68000 startup/pad polling, SH-2 startup, VDP and dual-SH-2 work
- `assets/generated/` — converted Racer's Edge runtime data
- `tools/import_speed_haste.py` — JCL/IS2/I3D/map/sector converter
- `tests/` — data, ROM, black-screen, control, completion and performance tests

## Scope still in progress

The port currently concentrates on the first shareware circuit and Formula One mode. The City, Stock cars, original menu depth, split-screen, records, network play and audio remain future porting work. These are not represented as complete in this revision.
