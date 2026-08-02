#!/usr/bin/env python3
"""Two-player split-screen point-to-point test.

Boots the ROM, toggles two-player mode from the main menu, starts a race and
verifies that two stacked viewports both render distinct world content and that
player 2 (AI-driven here, no second pad) makes the bottom viewport move.
"""
import argparse
import json
from pathlib import Path
from libretro_harness import LibretroHarness

# RetroPad IDs (libretro): B=0, START=3, UP=4, LEFT=6, RIGHT=7.
PAD_START = 3
PAD_UP = 4
PAD_LEFT = 6
PAD_RIGHT = 7


def probe(emu):
    return emu.pixel(318, 223)


def p2_camera_lag_probe(emu):
    return emu.pixel(310, 223)


def p2_human_probe(emu):
    return emu.pixel(305, 223)





def wait_probe_change(emu, old, limit, buttons=()):
    frames = 0
    while frames < limit:
        emu.run(1, buttons)
        frames += 1
        if probe(emu) != old:
            return frames
    raise AssertionError(f"state did not change in {limit}; probe={probe(emu)}")


def press(emu, button, limit=600):
    old = probe(emu)
    wait_probe_change(emu, old, limit, {button})
    emu.run(50)


def press_toggle(emu, button):
    # hold + release to trip one rising edge and clear the menu latch
    emu.run(50, {button})
    emu.run(60)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", required=True, type=Path)
    ap.add_argument("--rom", default=Path("release/SpeedHaste32X.32x"), type=Path)
    ap.add_argument("--out", default=Path("test-results/emulator"), type=Path)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report: dict[str, object] = {"core": str(args.core.resolve()),
                                 "rom": str(args.rom.resolve())}

    with LibretroHarness(args.core, args.rom) as emu:
        emu.run(120)
        press(emu, PAD_START)               # title -> main
        one = emu.rgb_frame()
        emu.save_png(args.out / "s1_main_one_player.png")
        press_toggle(emu, PAD_LEFT)         # one -> two player
        emu.run(10)
        two = emu.rgb_frame()
        emu.save_png(args.out / "s2_main_two_players.png")
        report["menu_toggle_changed"] = (one != two)
        if one == two:
            raise AssertionError("split toggle did not change the main menu")

        press(emu, PAD_START)               # main -> circuit (City)
        press(emu, PAD_START)               # circuit -> class (Stock)
        press(emu, PAD_START)               # class -> car
        press(emu, PAD_START)               # car -> countdown
        cd = probe(emu)
        wait_probe_change(emu, cd, 900)
        emu.run(50)
        emu.save_png(args.out / "s3_split_race.png")

        def band(top):
            pts = []
            for y in (top + 40, top + 70):
                for x in (80, 160, 240):
                    pts.append(emu.pixel(x, y))
            return pts

        top_band = band(12)
        bot_band = band(112)
        top_ok = any(max(p) > 0 for p in top_band)
        bot_ok = any(max(p) > 0 for p in bot_band)
        distinct = sum(1 for a, b in zip(top_band, bot_band) if a != b) >= 3
        report["top_viewport_nonblack"] = top_ok
        report["bottom_viewport_nonblack"] = bot_ok
        report["top_bottom_distinct"] = distinct
        if not top_ok or not bot_ok:
            raise AssertionError("a split viewport is black")
        if not distinct:
            raise AssertionError("split viewports do not show distinct views")

        report["p2_human_probe_rgb"] = list(p2_human_probe(emu))
        report["p2_human_in_emulator"] = max(p2_human_probe(emu)) > 200
        # Player 2 is human (a pad is present on port 2 in PicoDrive). Drive it
        # by accelerating on libretro port 1 and require its viewport to move.
        p2_p0 = emu.pixel(160, 150)
        moved = False
        for _ in range(600):
            emu.run(1, buttons2={PAD_UP})
            if emu.pixel(160, 150) != p2_p0:
                moved = True
                break
        report["player2_moved"] = moved
        (args.out / "split_report.json").write_text(json.dumps(report, indent=2) + "\n")
        if not moved:
            raise AssertionError("player 2 position never changed (pad2 accel)")

        # Player 2 is human. While player 1 steers, keep player 2 accelerating
        # (pad2) and steer it so its chase camera must turn to follow. The
        # probe fires while camera_angle2 lags toward player2.movangle.
        emu.run(120, {PAD_RIGHT, PAD_UP}, buttons2={PAD_UP, PAD_RIGHT})
        emu.run(120, {PAD_LEFT, PAD_UP}, buttons2={PAD_UP, PAD_LEFT})
        p2_before = emu.pixel(150, 140)
        p2_moved_after = False
        for _ in range(300):
            emu.run(1, buttons2={PAD_UP, PAD_RIGHT})
            if emu.pixel(150, 140) != p2_before:
                p2_moved_after = True
                break
        report["p2_camera_tracks"] = p2_moved_after
        if not p2_moved_after:
            raise AssertionError("player 2 camera stopped tracking while driving")

        p2_turned = False
        for _ in range(900):
            steer = PAD_RIGHT if (_ // 120) % 2 == 0 else PAD_LEFT
            emu.run(1, buttons2={PAD_UP, steer})
            if max(p2_camera_lag_probe(emu)) > 200:
                p2_turned = True
                break
        report["p2_camera_turns"] = p2_turned
        if not p2_turned:
            raise AssertionError("player 2 chase camera never turned to follow")

        # Split-screen frame rate via the heartbeat probe (must be playable).
        last = emu.pixel(319, 223)
        changes = 0
        for _ in range(600):
            emu.run(1)
            cur = emu.pixel(319, 223)
            if cur != last:
                changes += 1
                last = cur
        fps = changes / 10.0
        report["split_fps"] = fps
        if fps < 8.0:
            raise AssertionError(f"split screen too slow: {fps:.1f} fps")

    report["result"] = "PASS"
    (args.out / "split_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    print(f"PASS: split screen ({args.out})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
