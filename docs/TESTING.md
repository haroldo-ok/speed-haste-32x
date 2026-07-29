# Testing

## Data conversion test

```sh
python3 tests/test_assets.py
```

Validates that the committed runtime blob contains the original Racer's Edge data rather than placeholder art:

- 115 MAP00 tile/rotation combinations;
- 88 PATH points;
- 20 grid starts;
- 86 textured sector wall sides;
- 263 trackside objects;
- at least 100 decoded IS2 graphics;
- road palette range and varied terrain;
- non-empty directional frames rasterized from all six I3D cars.

## Static cartridge test

```sh
python3 tests/test_rom_static.py release/SpeedHaste32X.32x
```

Checks the `SEGA 32X` signature, MARS module header, master/slave entry points, ROM bank sizing, end address, payload and Genesis checksum.

## PicoDrive point-to-point test

```sh
bash tools/test_emulator.sh
```

The script builds PicoDrive revision `78a662e3135871a6c657d5e61900f6704152e594` and loads the ROM through a minimal headless libretro frontend.

The automated pad sequence is:

1. Boot the original Speed Haste shareware title.
2. Press Start and reach the original Formula One menu background.
3. Start Racer's Edge and verify the countdown.
4. Reach the race.
5. Measure completed frames over 600 emulated VBlanks.
6. Accelerate and require a large framebuffer delta.
7. Accelerate/steer and require another delta.
8. Pause and resume.
9. Exercise three lap checkpoints.
10. Reach and capture the finish state.

### Black/frozen-screen checks

Every point must provide:

- 320×224 video;
- at least eight distinct colors;
- at least 25% non-black pixels;
- a state-probe transition where appropriate;
- a changing frame heartbeat during the race.

### Performance gate

The test counts actual completed source-port frames, not calls to `retro_run()`. It requires at least 10 fps. The current report records:

- 116 completed frames in 600 VBlanks;
- **11.6 fps**;
- 159,025 changed RGB bytes after acceleration;
- 148,490 changed RGB bytes after steering.

Simulation remains timer-driven at 70 Hz even when a rendered frame spans multiple VBlanks.

## Full suite

```sh
make GENDEV=/path/to/toolchains/sega test
```

Artifacts and machine-readable results are in `test-results/emulator/`.
