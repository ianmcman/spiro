#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
check_dir=$(mktemp -d /tmp/spirolab-sanitizers.XXXXXX)
trap 'rm -rf -- "$check_dir"' EXIT
flags=(-O1 -g -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer)
for name in engine-check trace-check geometry-accuracy trace-roundtrip; do
 cc "${flags[@]}" -I. "tests/$name.c" engine.c trace.c -lm -o "$check_dir/$name"
 "$check_dir/$name"
done
cc "${flags[@]}" -I. tests/allocation-check.c engine.c -lm -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -o "$check_dir/allocation-check"
"$check_dir/allocation-check"
read -r -a cairo_flags <<< "$(pkg-config --cflags --libs cairo cairo-svg cairo-pdf)"
cc "${flags[@]}" -I. tests/pen-lift-check.c render.c engine.c -lm "${cairo_flags[@]}" -o "$check_dir/pen-lift-check"
"$check_dir/pen-lift-check"
read -r -a image_flags <<< "$(pkg-config --cflags --libs gdk-pixbuf-2.0 pangocairo)"
cc "${flags[@]}" -I. tests/image-check.c image.c engine.c trace.c -lm "${image_flags[@]}" -o "$check_dir/image-check"
"$check_dir/image-check"

cc "${flags[@]}" -I. tests/3d-check.c spiro3d.c engine.c trace.c -lm -o "$check_dir/3d-check"
"$check_dir/3d-check"
