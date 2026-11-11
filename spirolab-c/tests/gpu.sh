#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
check_dir=$(mktemp -d /tmp/spirolab-gpu.XXXXXX)
trap 'rm -rf -- "$check_dir"' EXIT
read -r -a flags <<< "$(pkg-config --cflags --libs gtk+-3.0 epoxy cairo)"
cc -O2 -g -std=c11 -Wall -Wextra -Werror -I. tests/gpu3d-check.c \
    gpu3d.c spiro3d.c spiro3d_io.c engine.c trace.c "${flags[@]}" -lm -o "$check_dir/gpu-check"
"$check_dir/gpu-check"
