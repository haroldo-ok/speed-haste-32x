#!/usr/bin/env python3
"""PicoDrive E2E for both-track/Stock-car Speed Haste 32X milestone."""
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
PAD_C = 8  # RetroPad A maps to Genesis C in PicoDrive
PAD_X = 10 # RetroPad L maps to Genesis X


def wall_collision_probe(emu: LibretroHarness) -> tuple[int, int, int]:
    return emu.pixel(313, 223)


def position_probe(emu: LibretroHarness) -> tuple[tuple[int, int, int], tuple[int, int, int]]:
    return emu.pixel(314, 223), emu.pixel(315, 223)


def collision_probe(emu: LibretroHarness) -> tuple[int, int, int]:
    return emu.pixel(316, 223)


def probe(emu: LibretroHarness) -> tuple[int, int, int]:
    return emu.pixel(318, 223)


def camera_probe(emu: LibretroHarness) -> tuple[int, int, int]:
    return emu.pixel(317, 223)


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
        "camera_lag_probe_rgb": list(camera_probe(emu)),
        "state_probe_rgb": list(probe(emu)),
    }


def capture(emu: LibretroHarness, out: Path, name: str,
            report: dict[str, object]) -> bytes:
    metrics = frame_metrics(emu)
    assert metrics["width"] == 320 and metrics["height"] == 224
    assert int(metrics["unique_colors"]) >= 8, f"{name}: possible blank screen"
    assert float(metrics["non_black_ratio"]) >= 0.25, f"{name}: mostly black output"
    emu.save_png(out / f"{name}.png")
    report[name] = metrics
    return emu.rgb_frame()


def press_transition(emu: LibretroHarness, button: int, limit: int = 600) -> int:
    old = probe(emu)
    frames = wait_probe_change(emu, old, limit, {button})
    emu.run(50)  # release long enough to clear the menu input latch
    return frames


def press_selection(emu: LibretroHarness, button: int) -> None:
    emu.run(50, {button})
    emu.run(60)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", required=True, type=Path)
    ap.add_argument("--rom", default=Path("release/SpeedHaste32X.32x"), type=Path)
    ap.add_argument("--out", default=Path("test-results/emulator"), type=Path)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report: dict[str, object] = {
        "core": str(args.core.resolve()), "rom": str(args.rom.resolve()),
        "sequence": "title -> main -> The City -> Stock -> car -> countdown -> race -> wall impact -> recovery -> pause -> finish",
    }

    with LibretroHarness(args.core, args.rom) as emu:
        emu.run(120)
        capture(emu, args.out, "01_original_title", report)

        report["vblanks_title_to_main"] = press_transition(emu, PAD_START)
        capture(emu, args.out, "02_main_menu", report)

        report["vblanks_main_to_circuit"] = press_transition(emu, PAD_START)
        circuit0 = capture(emu, args.out, "03_racers_edge_selection", report)
        press_selection(emu, PAD_RIGHT)
        circuit1 = capture(emu, args.out, "04_the_city_selection", report)
        assert circuit0 != circuit1, "circuit selection did not visibly change"

        report["vblanks_circuit_to_class"] = press_transition(emu, PAD_START)
        class0 = capture(emu, args.out, "05_formula_one_selection", report)
        press_selection(emu, PAD_RIGHT)
        class1 = capture(emu, args.out, "06_stock_selection", report)
        assert class0 != class1, "car class selection did not visibly change"

        report["vblanks_class_to_car"] = press_transition(emu, PAD_START)
        car0 = capture(emu, args.out, "07_stock_car_one", report)
        press_selection(emu, PAD_RIGHT)
        car1 = capture(emu, args.out, "08_stock_car_two", report)
        assert car0 != car1, "original Stock I3D car selection did not change"

        report["vblanks_car_to_countdown"] = press_transition(emu, PAD_START)
        countdown_probe = probe(emu)
        capture(emu, args.out, "09_city_countdown", report)
        report["vblanks_countdown_to_race"] = wait_probe_change(
            emu, countdown_probe, 900)
        emu.run(50)
        race_probe = probe(emu)
        race = capture(emu, args.out, "10_city_stock_race", report)

        # Cycle chase -> cockpit -> high -> original MAP01 trackside TV camera.
        for _ in range(3):
            press_selection(emu, PAD_X)
        television = capture(emu, args.out, "10b_city_tv_camera", report)
        assert television != race, "static MAP camera did not change the view"
        press_selection(emu, PAD_X)  # wrap back to chase

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
        report["measured_output_fps_city_stock"] = measured_fps
        assert measured_fps >= 9.0, f"rendering regression: {measured_fps:.1f} fps"

        emu.run(420, {PAD_C})
        accelerated = capture(emu, args.out, "11_stock_accelerated", report)
        accel_delta = sum(a != b for a, b in zip(race, accelerated))
        assert accel_delta > 10000, "Stock acceleration did not produce travel"
        emu.run(360, {PAD_C, PAD_RIGHT})
        steered = capture(emu, args.out, "12_stock_steered", report)
        steer_delta = sum(a != b for a, b in zip(accelerated, steered))
        assert steer_delta > 5000, "Stock steering did not alter the view"
        assert max(camera_probe(emu)) > 200, "chase camera snapped to player instead of lagging"
        assert max(collision_probe(emu)) > 200, "hard steering never reached collision response"
        assert max(wall_collision_probe(emu)) > 200, "hard steering never reached a source wall"
        report["collision_probe_rgb"] = list(collision_probe(emu))
        report["wall_collision_probe_rgb"] = list(wall_collision_probe(emu))
        report["acceleration_changed_rgb_bytes"] = accel_delta
        report["steering_changed_rgb_bytes"] = steer_delta

        # Reverse the steering used to hit the guardrail and keep accelerating.
        # The QA position nibbles must continue changing: repeated rollback to
        # the same embedded point was the wall-sticking regression.
        recovery_changes = 0
        last_position = position_probe(emu)
        for _ in range(240):
            emu.run(1, {PAD_C, PAD_LEFT})
            current_position = position_probe(emu)
            if current_position != last_position:
                recovery_changes += 1
                last_position = current_position
        recovered = capture(emu, args.out, "12b_wall_collision_recovered", report)
        recovery_delta = sum(a != b for a, b in zip(steered, recovered))
        assert recovery_changes >= 6, \
            f"car remained pinned to wall; only {recovery_changes} position changes"
        assert recovery_delta > 5000, "driving away from wall did not resume world movement"
        report["wall_recovery_position_changes"] = recovery_changes
        report["wall_recovery_changed_rgb_bytes"] = recovery_delta
        emu.run(60)

        report["vblanks_to_pause"] = press_transition(emu, PAD_START)
        capture(emu, args.out, "13_paused", report)
        report["vblanks_to_resume"] = press_transition(emu, PAD_START)
        assert probe(emu) == race_probe

        chord = {PAD_LEFT, PAD_RIGHT, PAD_C}
        for _lap in range(3):
            emu.run(80, chord)
            emu.run(80)
        for _ in range(3):
            if probe(emu) != race_probe:
                break
            emu.run(100, chord)
            emu.run(80)
        assert probe(emu) != race_probe, "finish state was not reached"
        capture(emu, args.out, "14_finished", report)

    report["result"] = "PASS"
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    print(f"PASS: The City + Stock E2E, content, controls and performance ({args.out})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
