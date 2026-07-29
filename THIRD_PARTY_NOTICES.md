# Third-party notices and provenance

## Speed Haste source and data

- Source: <https://github.com/TheJare/SpeedHasteSrc>
- Source revision: `62456580c1b96a6a3d7de4c7faa1d57f4876a646`
- Shareware package supplied in the task: <https://www.dosgamesarchive.com/file/speed-haste/speedsha>
- Copyright: NoriaWorks Entertainment, Javier Arévalo Baeza and Friendware, 1995

The source README states: “As far as I'm concerned, you can do whatever you want with this code, use it as far as applicable law permits.”

The ported formulas and structures are identified in `docs/PORTING.md`. Existing Javier Arévalo copyright headers are retained where source was directly adapted.

`assets/generated/speed_haste_assets.bin` is a platform conversion of both shareware circuits (MAP00/MAP01), both vehicle classes, palettes, floor tiles, panoramas, sector walls, sprites, cockpits/HUDs and I3D vehicle geometry from the supplied `SPEEDH.JCL`. It contains no DOS executable, setup program, DOS4GW runtime, documentation, original S3M music or raw sound effects. Speed Haste names, graphics and converted game data remain property of their respective owners; this project does not relicense them.

The conversion can be reproduced with `tools/import_speed_haste.py` from a legally obtained `SPEEDH.JCL`.

## 32X boot/support code

- Upstream: <https://github.com/gameblabla/32x-playground>
- Revision: `8eb67a8048572df58796705d700c2500f8f20dc1`
- Original 32X support author credited in source: Chilly Willy

The platform startup assembly and linker layout were adapted from this 32X support code. Existing source notices are retained.

## Doom 32X: Resurrection reference

- Repository: <https://github.com/viciious/d32xr>
- Revision reviewed: `95f5e05ca4f5f50f9f158440d05317db425dd2c4`

D32XR was used as a hardware/toolchain reference for current GCC flags, framebuffer access, master/slave synchronization and PicoDrive-compatible startup. No Doom game or content code is included.

## 32XDK

- Releases: <https://github.com/viciious/32XDK/releases>
- Release: `20220418`
- GCC: 12.1.0

The toolchain is downloaded separately by `tools/setup_toolchain.sh` and is not redistributed.

## PicoDrive

- Repository: <https://github.com/libretro/picodrive>
- Test revision: `78a662e3135871a6c657d5e61900f6704152e594`

PicoDrive is built in the user cache by `tools/test_emulator.sh` and is not redistributed.
