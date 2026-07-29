# Testing

## Converted-content validation

```sh
python3 tests/test_assets.py
```

The test rejects placeholder content and verifies:

- MAP00: 115 tiles, 88 PATH points, 20 starts, 86 walls, 263 objects;
- MAP01: 190 tiles, 186 PATH points, 20 starts, 123 walls, 325 objects;
- both original panoramas and cockpits;
- main, circuit and vehicle menu backgrounds;
- at least 110 decoded IS2 graphics;
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
7. Measure completed frames over 600 VBlanks.
8. Accelerate and require visible world travel.
9. Accelerate/steer and require another large frame delta.
10. Pause and resume.
11. Exercise three lap checkpoints and reach finish.

### Black/frozen-screen gates

Every capture must provide 320×224 output, at least eight colors and at least 25% non-black pixels. Menu selections must alter the framebuffer, state probes must transition, and the heartbeat must continue changing during a race.

### Performance gate

The City is larger than Racer's Edge and is tested with Stock cars. The current report records:

- 100 completed frames in 600 VBlanks;
- **10.0 output fps**;
- independent 70 Hz world simulation;
- 98,874 changed RGB bytes after acceleration;
- 156,532 after steering.

`tests/test_projection.py` also proves point-to-point that a world location sampled at each floor row projects back to the identical object-base row. It checks the IS2 hotspot transform, the original 180-degree car transform, stateful 1/16 camera convergence, and `GetAngle(dx, -dy)` at 16 headings. A forward-flow test proves fixed scenery gets closer and moves down-screen as the camera advances. The E2E test requires the camera-lag probe to remain active under sustained steering.

The regression floor is 9 fps for this stress configuration.

## Full suite

```sh
make GENDEV=/path/to/toolchains/sega test
```

Machine-readable results and 14 PNG captures are written under `test-results/emulator/`.
