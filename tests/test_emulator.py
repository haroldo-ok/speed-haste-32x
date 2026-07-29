#!/usr/bin/env python3
"""Headless PicoDrive point-to-point and performance test for the source port."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from libretro_harness import LibretroHarness

PAD_START = 3
PAD_LEFT = 6
PAD_RIGHT = 7
PAD_C = 8          # RetroPad A maps to Genesis C in PicoDrive
PAD_Z = 11         # RetroPad R maps to Genesis Z


def probe(emu: LibretroHarness) -> tuple[int, int, int]:
    return emu.pixel(318, 223)


def heartbeat(emu: LibretroHarness) -> tuple[int, int, int]:
    return emu.pixel(319, 223)


def wait_probe_change(emu: LibretroHarness, old: tuple[int, int, int], limit: int,
                      buttons: set[int] | tuple[int, ...] = ()) -> int:
    for frame in range(1, limit + 1):
        emu.run(1, buttons)
        if emu.width >= 320 and probe(emu) != old:
            return frame
    raise AssertionError(f"state did not change in {limit} VBlanks; probe={probe(emu)}")


def frame_metrics(emu: LibretroHarness) -> dict[str, object]:
    rgb = emu.rgb_frame()
    pixels = [rgb[i:i + 3] for i in range(0, len(rgb), 3)]
    black = sum(max(p) <= 8 for p in pixels)
    return {
        "width": emu.width,
        "height": emu.height,
        "crc32": f"{zlib.crc32(rgb):08x}",
        "unique_colors": len(set(pixels)),
        "non_black_ratio": round(1.0 - black / len(pixels), 6),
        "state_probe_rgb": list(probe(emu)),
    }


def capture(emu: LibretroHarness, out: Path, name: str, report: dict[str, object]) -> bytes:
    metrics = frame_metrics(emu)
    assert metrics["width"] == 320 and metrics["height"] == 224
    assert int(metrics["unique_colors"]) >= 8, f"{name}: possible blank screen"
    assert float(metrics["non_black_ratio"]) >= 0.25, f"{name}: mostly black output"
    emu.save_png(out / f"{name}.png")
    report[name] = metrics
    return emu.rgb_frame()


def press_for_transition(emu: LibretroHarness, button: int, limit: int = 600) -> int:
    old = probe(emu)
    frames = wait_probe_change(emu, old, limit, {button})
    emu.run(40)  # release through several 68000 controller polls
    return frames


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", required=True, type=Path)
    ap.add_argument("--rom", default=Path("release/SpeedHaste32X.32x"), type=Path)
    ap.add_argument("--out", default=Path("test-results/emulator"), type=Path)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report: dict[str, object] = {
        "core": str(args.core.resolve()), "rom": str(args.rom.resolve()),
        "sequence": "title -> original menu -> countdown -> race -> accelerate -> steer -> pause/resume -> finish",
    }

    with LibretroHarness(args.core, args.rom) as emu:
        emu.run(120)
        title_probe = probe(emu)
        title = capture(emu, args.out, "01_original_title", report)

        report["vblanks_title_to_menu"] = press_for_transition(emu, PAD_START)
        menu_probe = probe(emu)
        assert menu_probe != title_probe
        menu = capture(emu, args.out, "02_original_menu", report)
        assert menu != title

        report["vblanks_menu_to_countdown"] = press_for_transition(emu, PAD_START)
        countdown_probe = probe(emu)
        capture(emu, args.out, "03_countdown", report)

        report["vblanks_countdown_to_race"] = wait_probe_change(emu, countdown_probe, 900)
        emu.run(40)
        race_probe = probe(emu)
        race = capture(emu, args.out, "04_race_start", report)

        # Count completed source-port frames over ten emulated seconds.
        changes = 0
        last = heartbeat(emu)
        for _ in range(600):
            emu.run(1)
            current = heartbeat(emu)
            if current != last:
                changes += 1
                last = current
        measured_fps = changes / 10.0
        report["completed_frames_in_600_vblanks"] = changes
        report["measured_output_fps"] = measured_fps
        assert measured_fps >= 10.0, f"rendering regression: {measured_fps:.1f} fps"

        emu.run(360, {PAD_C})
        accelerated = capture(emu, args.out, "05_accelerated", report)
        accel_delta = sum(a != b for a, b in zip(race, accelerated))
        assert accel_delta > 10000, "acceleration did not produce visible travel"
        emu.run(300, {PAD_C, PAD_RIGHT})
        steered = capture(emu, args.out, "06_steered", report)
        steer_delta = sum(a != b for a, b in zip(accelerated, steered))
        assert steer_delta > 5000, "steering did not alter the view"
        report["acceleration_changed_rgb_bytes"] = accel_delta
        report["steering_changed_rgb_bytes"] = steer_delta
        emu.run(60)

        report["vblanks_to_pause"] = press_for_transition(emu, PAD_START)
        paused_probe = probe(emu)
        assert paused_probe != race_probe
        capture(emu, args.out, "07_paused", report)
        report["vblanks_to_resume"] = press_for_transition(emu, PAD_START)
        assert probe(emu) == race_probe

        # Impossible physical chord Left+Right+C advances a QA lap result.
        chord = {PAD_LEFT, PAD_RIGHT, PAD_C}
        for _lap in range(3):
            emu.run(80, chord)
            emu.run(80)
        if probe(emu) == race_probe:
            # Controller polling can miss a chord near a render boundary.
            for _ in range(3):
                emu.run(100, chord); emu.run(80)
                if probe(emu) != race_probe:
                    break
        assert probe(emu) != race_probe, "finish state was not reached"
        capture(emu, args.out, "08_finished", report)

    report["result"] = "PASS"
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    print(f"PASS: source-port E2E, black-screen, controls and >=10 fps checks ({args.out})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
