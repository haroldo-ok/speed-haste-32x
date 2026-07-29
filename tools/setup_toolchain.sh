#!/usr/bin/env bash
set -euo pipefail

# Installs the pinned 32XDK binary toolchain in the user's cache, never /opt.
CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/speed-haste-32x"
SDK_ROOT="$CACHE_ROOT/32xdk-20220418"
GENDEV="$SDK_ROOT/opt/toolchains/sega"
ARCHIVE="$CACHE_ROOT/chillys-sega-devkit-20220418-opt.tar.zst"
URL="https://github.com/viciious/32XDK/releases/download/20220418/chillys-sega-devkit-20220418-opt.tar.zst"
PYDEPS="$CACHE_ROOT/python"

if [[ -x "$GENDEV/sh-elf/bin/sh-elf-gcc" ]]; then
  echo "32XDK is already installed: $GENDEV"
  echo "Build with: make GENDEV=$GENDEV"
  exit 0
fi

mkdir -p "$CACHE_ROOT" "$SDK_ROOT" "$PYDEPS"
if [[ ! -f "$ARCHIVE" ]]; then
  echo "Downloading 32XDK 20220418 (about 145 MiB)..."
  curl -L --fail --retry 3 "$URL" -o "$ARCHIVE"
fi

if ! PYTHONPATH="$PYDEPS" python3 -c 'import zstandard' >/dev/null 2>&1; then
  echo "Installing Python zstandard module in $PYDEPS..."
  python3 -m pip install --quiet --target "$PYDEPS" zstandard
fi

if [[ ! -x "$GENDEV/sh-elf/bin/sh-elf-gcc" ]]; then
  echo "Extracting toolchain (about 450 MiB)..."
  PYTHONPATH="$PYDEPS" python3 - "$ARCHIVE" "$SDK_ROOT" <<'PY'
import sys, tarfile
from pathlib import Path
import zstandard
archive, destination = map(Path, sys.argv[1:])
with archive.open('rb') as source:
    with zstandard.ZstdDecompressor().stream_reader(source) as reader:
        with tarfile.open(fileobj=reader, mode='r|') as tar:
            tar.extractall(destination)
PY
fi

"$GENDEV/sh-elf/bin/sh-elf-gcc" --version | head -1
echo "Build with: make GENDEV=$GENDEV"
