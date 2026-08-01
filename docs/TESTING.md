# Testing

## Converted-content validation

```sh
python3 tests/test_assets.py
```

The test rejects placeholder content and verifies:

- MAP00: 115 tiles, 88 PATH points, 20 starts, 11 TV cameras, 107 sector vertices, 67 polygons/308 sides, 86 walls (76 collision boundaries), 263 objects;
- MAP01: 190 tiles, 186 PATH points, 20 starts, 36 TV cameras, 136 sector vertices, 91 polygons/394 sides, 123 collision walls, 325 objects;
- complete per-sector object ranges and 256 default-sector spatial-bin ranges, with every object index appearing exactly once (126/116 default objects across 17/23 non-empty bins);
- both original panoramas and cockpits;
- main, circuit and vehicle menu backgrounds;
- at least 130 decoded IS2 graphics, including sparks and three smoke families;
- two classes × six I3D models × 16 directional views;
- distinct Formula One and Stock vehicle data;
- driveable palette range and varied terrain on both circuits.

## Static cartridge validation

```sh
python3 tests/test_rom_static.py release/SpeedHaste32X.32x
```

Checks cartridge banking, `SEGA 32X` and MARS headers, master/slave entry points, payload, end address and Genesis checksum.

## PicoDrive point-to-point test

```sh
bash tools/test_emulator.sh
```

The test builds PicoDrive revision `78a662e3135871a6c657d5e61900f6704152e594` and drives the ROM with emulated controller input:

1. Boot original shareware title.
2. Enter main menu.
3. Enter circuit selection and visibly change Racer's Edge → The City.
4. Enter class selection and visibly change Formula One → Stock.
5. Enter car selection and visibly change between original Stock I3D cars.
6. Start The City countdown and race.
7. Cycle to an imported MAP01 trackside-TV camera and verify a distinct view.
8. Measure completed frames over 600 VBlanks.
9. Accelerate and require visible world travel.
10. Accelerate/steer into a source-derived wall and require another large frame delta.
11. Assert dedicated wall-collision, general-collision and chase-camera-lag probes.
12. Reverse steering while accelerating and require continued encoded X/Y position changes, proving the car can drive away instead of being rolled back into the wall.
13. Pause and resume.
14. Exercise three lap checkpoints and reach finish.
15. Return through the menus, start Racer's Edge/Formula One, traverse all 88 authored PATH points through real rectangular collision, and require a completed route with collision point `0xFFFF`.

### Black/frozen-screen gates

Every capture must provide 320×224 output, at least eight colors and at least 25% non-black pixels. Menu selections must alter the framebuffer, state probes must transition, and the heartbeat must continue changing during a race.

### Performance gate

The City is larger than Racer's Edge and is tested with Stock cars. The current report records:

- 150 completed frames in 600 VBlanks;
- **15.0 output fps**;
- independent 70 Hz world simulation;
- 20 visible sectors, 18 of 123 walls and 89 of 325 object candidates;
- master-critical profile: 3 visibility + 44 panorama + 9 floor wait + 25 walls + 35 objects + 7 cars/effects + 16 HUD = 139 ticks at Sclk/8192;
- overlapping slave-floor profile: 53 ticks;
- 104,257 changed RGB bytes after acceleration;
- 131,396 after steering/collision;
- 68 encoded position changes while driving away from the wall;
- 140,113 changed RGB bytes between impact and recovered travel;
- complete Racer's Edge route in 2,902 VBlanks with no collision point (`65535`/`0xFFFF`).

`tests/test_collision.py` decodes every compact sector/side/vertex record, validates indices, adjacency ranges, bounds, wall links and all 40 source start positions. It also proves MAP00's ten seam-crossing walls (and MAP01's sixteen) have short wrapped deltas rather than phantom 15–16K spans, and checks current/adjacent-sector lookup, asymmetric rectangular bounds, old/mid/new corner frames, contact/release hysteresis, outward-motion handling, `slidcounter` decay and recovery steering.

`tests/test_projection.py` also proves point-to-point that a world location sampled at each floor row projects back to the identical object-base row. It checks the IS2 hotspot transform, the original 180-degree car transform, stateful 1/16 camera convergence, and `GetAngle(dx, -dy)` at 16 headings. A forward-flow test proves fixed scenery gets closer and moves down-screen as the camera advances. The E2E test requires the camera-lag probe to remain active under sustained steering.

The regression floor is now 12 fps for this stress configuration.

## Full suite

```sh
make GENDEV=/path/to/toolchains/sega test
```

`tests/test_optimization.py` verifies the SH7604 divider and floor-row assembly kernel, dual-row packed output, aligned cache-through SDRAM, tile/topology/object/bin caches, wall DDA loops, bounded portal traversal, candidate masking, packed panorama wrapping, split master/slave profiling, imported topology counts and static cameras.

Machine-readable results and 17 PNG captures are written under `test-results/emulator/`.
