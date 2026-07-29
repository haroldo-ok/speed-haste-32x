#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/speed-haste-32x"
PICO_COMMIT="78a662e3135871a6c657d5e61900f6704152e594"
PICO_DIR="$CACHE_ROOT/picodrive-$PICO_COMMIT"
CORE="${PICODRIVE_CORE:-$PICO_DIR/picodrive_libretro.so}"

if [[ ! -f "$CORE" ]]; then
  command -v git >/dev/null || { echo "git is required to build PicoDrive" >&2; exit 1; }
  command -v make >/dev/null || { echo "make is required to build PicoDrive" >&2; exit 1; }
  if [[ ! -d "$PICO_DIR/.git" ]]; then
    mkdir -p "$CACHE_ROOT"
    git clone --recurse-submodules https://github.com/libretro/picodrive.git "$PICO_DIR"
  fi
  git -C "$PICO_DIR" checkout "$PICO_COMMIT"
  git -C "$PICO_DIR" submodule update --init --recursive
  make -C "$PICO_DIR" -f Makefile.libretro -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" platform=unix
fi

python3 "$ROOT/tests/test_emulator.py" \
  --core "$CORE" \
  --rom "$ROOT/release/SpeedHaste32X.32x" \
  --out "$ROOT/test-results/emulator"
