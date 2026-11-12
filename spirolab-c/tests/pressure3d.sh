#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
scratch=$(mktemp -d /tmp/spirolab-pressure.XXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
# Raise the limit only in disposable copies; production limits stay unchanged.
cp ./*.[ch] "$scratch/"
mkdir "$scratch/tests"
cp tests/pressure3d.c "$scratch/tests/"
sed -i 's/^#define SP3_MAX_SAMPLES .*/#define SP3_MAX_SAMPLES 8000000/' "$scratch/spiro3d.h"
read -r -a flags <<< "$(pkg-config --cflags --libs gtk+-3.0 epoxy cairo)"
cc -O2 -g -std=c11 -Wall -Wextra -Werror -I"$scratch" \
 "$scratch/tests/pressure3d.c" "$scratch/gpu3d.c" "$scratch/spiro3d.c" \
 "$scratch/spiro3d_io.c" "$scratch/engine.c" "$scratch/trace.c" \
 "${flags[@]}" -lm -o "$scratch/pressure"
timeout 60s "$scratch/pressure" "${1:-128000}"
