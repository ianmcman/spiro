#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
scratch=$(mktemp -d /tmp/spirolab-desktop-check.XXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
read -r -a flags <<< "$(pkg-config --cflags --libs gtk+-3.0 pangocairo cairo-svg cairo-pdf libonnxruntime epoxy)"
read -r -a compile_flags <<< "${CHECK_CFLAGS:--O2 -g -std=c11 -Wall -Wextra -Werror}"
for name in gpu2d-check live-check; do
 cc "${compile_flags[@]}" -I. "tests/$name.c" \
  render.c engine.c trace.c ai.c document.c image.c spiro3d.c spiro3d_io.c viewer3d.c gpu3d.c gpu2d.c \
  "${flags[@]}" -lm -o "$scratch/$name"
 G_DEBUG=fatal-criticals timeout 120s "$scratch/$name"
done
