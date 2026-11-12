#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
scratch=$(mktemp -d /tmp/spirolab-high-check.XXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
read -r -a flags <<< "$(pkg-config --cflags --libs gdk-pixbuf-2.0 pangocairo cairo-svg cairo-pdf)"
cc -O2 -g -std=c11 -Wall -Wextra -Werror -I. tests/high-resolution-check.c \
 document.c render.c image.c engine.c trace.c "${flags[@]}" -lm -o "$scratch/check"
timeout 120s "$scratch/check"
