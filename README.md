# Speed Haste 32X

A source-based Sega 32X port of Javier Arévalo's 1995 DOS racer **Speed Haste**.

## Current status

This revision ports both circuits and vehicle classes from the original shareware data:

- **Racer's Edge** (`MAP00`) and **The City** (`MAP01`);
- Formula One and Stock classes, six original I3D cars per class;
- original class-specific cockpits, tachometers and maximum speeds;
- Stock powersliding behavior derived from `userctl.c`;
- original 64×64 floor tiles, rotations, PATH points, walls and track objects;
- original floor-casting equations from `3dfloor.c`/`fla.asm`;
- one shared `hsk+y` projection origin for ground, walls, objects and cars;
- original chase-camera 1/16 angular lag and correct rear/front model orientation;
- corrected inverted-world-Y heading reconstruction (`GetAngle(dx, -dy)`), so forward travel makes scenery approach the viewer;
- original panorama, mountains, wall textures, trackside sprites and palette shading;
- original title and race-menu backgrounds;
- multi-step setup flow for circuit, class and individual car selection;
- countdown, lap/position HUD, chase/cockpit/high/trackside-TV cameras, pause and finish states;
- PATH-driven computer racers based on `cars.c` and `racemap.c`;
- source-backed guardrail and car-to-car collision response, including wall-release hysteresis and the original crash-recovery phase;
- original spark, ground-smoke and persistent skid graphics.

The world runs at the original **70 Hz simulation rate**, independently of video. The pinned PicoDrive City/Stock stress test measures **12.0 fps** without slow-motion physics.

## Play

Load this ROM with a current PicoDrive/RetroArch core configured for Sega 32X:

**[`release/SpeedHaste32X.32x`](release/SpeedHaste32X.32x)**

The cartridge is 2.5 MiB and has a valid MARS header and Genesis checksum.

### Menu controls

- **Start/B/C:** confirm
- **Left/Right:** change circuit, class or car
- **A:** go back

### Race controls

| Genesis/32X pad | Action |
|---|---|
| Left / Right | Steer |
| B or C | Accelerate |
| A or Down | Brake; continue holding for reverse |
| Start | Pause/resume |
| X (six-button pad) | Cycle chase, cockpit, high and trackside-TV cameras |
| Y (six-button pad) | Toggle HUD |

Stock cars can break traction under hard high-speed steering/braking and then progressively regain it, following the separate `cartype == 1` path in the DOS source.

## Performance implementation

The current renderer uses:

- master/slave SH-2 parallel floor and panorama rendering;
- 80×65 floor samples expanded in the DOS low-detail style;
- 2×2 trackside sprite and polygon sampling;
- low-detail textured wall columns;
- an eight-frame cached textured minimap;
- all 12 I3D cars pre-rendered into the original engine's 16-direction sprite path;
- 32-bit framebuffer fills and packed writes;
- 70 Hz timer catch-up from `race.c`, so rendering never determines game speed;
- SH7604 hardware-divider helpers adapted from D32XR's fixed-point routines;
- frequency-sorted 28-tile SDRAM cache covering 95% of Racer's Edge and 92% of The City map cells;
- cached map indices, panoramas, mountain layer, and 8 KiB shade table in aligned SDRAM;
- build-time `SEC_TOMAP` wall conversion and DDA wall interpolation with no divide/modulo in horizontal loops;
- broad-phase collision rejection before closest-point projection;
- no-divide integer normalisation only after a real wall contact, never in the normal 70 Hz scan.

PWM audio remains disabled by default because continuous slave-SH-2 FIFO polling stalls some ARM PicoDrive builds. The slave is used for floor rendering instead.

## Build

Prerequisites: POSIX shell, Python 3, `curl`, `make`, and approximately 600 MiB of temporary cache space.

```sh
bash tools/build.sh
```

The script installs the pinned 32XDK `20220418` toolchain in the user cache and creates `release/SpeedHaste32X.32x`.

With an existing toolchain:

```sh
make GENDEV=/opt/toolchains/sega
```

### Reimporting the original data

```sh
python3 tools/import_speed_haste.py /path/to/SPEEDH.JCL --out assets/generated
```

The importer follows `jclib.c`, `racemap.c`, `sectors.c`, `is2code.c`, `object3d.c` and `flsprs.c`. It now imports both shareware maps, both panoramas/cockpits and all 12 vehicle models.

## Automated tests

```sh
make GENDEV=/path/to/toolchains/sega test
```

The PicoDrive E2E selects the newly added content through emulated pad input:

```text
title → main menu → The City → Stock → car selection → countdown
      → race → accelerate → wall impact → drive away → pause/resume → finish
```

The suite captures 16 points, rejects black/frozen output, verifies selection changes, visible travel, source-wall collision and recovery, trackside cameras, shared floor/object projection, directional-car orientation and active chase-camera lag, and enforces a 9 fps minimum on The City. The anti-stick point-to-point gate steers into a wall, reverses steering, and requires the encoded world position to keep changing. Current result: **12.0 fps**, 103,292 RGB bytes changed by acceleration, 143,632 by steering/collision, and 53 position-probe changes while driving away.

See [`test-results/emulator/report.json`](test-results/emulator/report.json).

## Layout

- `src/sh_game.*` — fixed-point gearbox, Stock sliding, AI, race timing and menus
- `src/sh_render.*` — original-style floor, walls, objects, cars, cockpit and HUD
- `src/sh_assets.*` — per-circuit compact data accessors
- `src/platform/` — 68000 pad polling, VDP and dual-SH-2 work
- `assets/generated/` — converted MAP00/MAP01 and Formula/Stock runtime data
- `tools/import_speed_haste.py` — JCL/IS2/I3D/map/sector converter
- `tests/` — content, ROM, black-screen, control, completion and performance tests

## Scope still in progress

Not yet ported: two-player split screen, persistent record tables, IPX/serial/modem play, complete DOS menu decoration/animation, S3M music and VTAL audio. The registered-only six circuits are not present in the supplied shareware data and are not claimed as implemented.
