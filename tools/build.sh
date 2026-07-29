#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/speed-haste-32x"
CACHED_GENDEV="$CACHE_ROOT/32xdk-20220418/opt/toolchains/sega"
GENDEV="${GENDEV:-}"

if [[ -z "$GENDEV" ]]; then
  if [[ -x /opt/toolchains/sega/sh-elf/bin/sh-elf-gcc ]]; then
    GENDEV=/opt/toolchains/sega
  elif [[ -x "$CACHED_GENDEV/sh-elf/bin/sh-elf-gcc" ]]; then
    GENDEV="$CACHED_GENDEV"
  else
    "$ROOT/tools/setup_toolchain.sh"
    GENDEV="$CACHED_GENDEV"
  fi
fi

make -C "$ROOT" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" GENDEV="$GENDEV" rom
printf 'ROM: %s\n' "$ROOT/release/SpeedHaste32X.32x"
