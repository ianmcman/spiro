#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
scratch=$(mktemp -d /tmp/spirolab-pressure2d.XXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
if [[ "${1:-}" == --gpu ]]; then
 read -r -a flags <<< "$(pkg-config --cflags --libs gtk+-3.0 epoxy cairo)"
 cc -O2 -g -std=c11 -Wall -Wextra -Werror -I. tests/flat-gpu-pressure.c \
  render.c gpu3d.c spiro3d.c spiro3d_io.c engine.c trace.c "${flags[@]}" -lm -o "$scratch/check"
 timeout 120s "$scratch/check"
else
 read -r -a flags <<< "$(pkg-config --cflags --libs pangocairo gdk-pixbuf-2.0 cairo-svg cairo-pdf)"
 cc -O2 -g -std=c11 -Wall -Wextra -Werror -I. tests/2d-pressure.c \
  render.c image.c engine.c trace.c "${flags[@]}" -lm -o "$scratch/check"
 timeout 120s "$scratch/check" "${1:-2097152}" "${2:-8192}"
fi
