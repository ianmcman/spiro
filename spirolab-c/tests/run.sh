#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
check_dir=$(mktemp -d /tmp/spirolab-checks.XXXXXX)
trap 'rm -rf -- "$check_dir"' EXIT
read -r -a compile_flags <<< "${CHECK_CFLAGS:--O2 -g -std=c11 -Wall -Wextra -Werror}"
read -r -a cairo_flags <<< "$(pkg-config --cflags --libs cairo cairo-svg cairo-pdf)"
read -r -a image_flags <<< "$(pkg-config --cflags --libs gdk-pixbuf-2.0 cairo cairo-svg cairo-pdf)"
for test_name in engine-check trace-check geometry-accuracy trace-roundtrip; do
 cc "${compile_flags[@]}" -I. "tests/$test_name.c" engine.c trace.c -lm -o "$check_dir/$test_name"
 "$check_dir/$test_name"
done
for test_name in render-check overlay-check pen-lift-check; do
 cc "${compile_flags[@]}" -I. "tests/$test_name.c" render.c engine.c -lm "${cairo_flags[@]}" -o "$check_dir/$test_name"
 "$check_dir/$test_name"
done
cc "${compile_flags[@]}" -I. tests/design-integrity.c document.c render.c engine.c trace.c -lm "${image_flags[@]}" -o "$check_dir/design-integrity"
"$check_dir/design-integrity"
read -r -a trace_image_flags <<< "$(pkg-config --cflags --libs gdk-pixbuf-2.0 pangocairo)"
cc "${compile_flags[@]}" -I. tests/image-check.c image.c engine.c trace.c -lm "${trace_image_flags[@]}" -o "$check_dir/image-check"
"$check_dir/image-check"
cc "${compile_flags[@]}" -I. tests/3d-check.c spiro3d.c engine.c trace.c -lm -o "$check_dir/3d-check"
"$check_dir/3d-check"
read -r -a three_flags <<< "$(pkg-config --cflags --libs cairo glib-2.0)"
cc "${compile_flags[@]}" -I. tests/3d-io-check.c spiro3d.c spiro3d_io.c engine.c trace.c -lm "${three_flags[@]}" -o "$check_dir/3d-io-check"
"$check_dir/3d-io-check"
./spirolab --render-3d "$check_dir/3d.png"
./spirolab --stl-3d "$check_dir/3d.stl"
./spirolab --render-demo "$check_dir/demo.png"
./spirolab --trace-text Ian "$check_dir/name.png" --terms 16384
if ./spirolab --unknown >/dev/null 2>&1; then
 echo 'unknown CLI argument was accepted' >&2; exit 1
fi
if [[ -n "${SPIROLAB_FACE_FIXTURE:-}" ]]; then
 read -r -a ai_flags <<< "$(pkg-config --cflags --libs gdk-pixbuf-2.0 cairo libonnxruntime)"
 cc "${compile_flags[@]}" -I. tests/ai-check.c ai.c -lm "${ai_flags[@]}" -o "$check_dir/ai-check"
 "$check_dir/ai-check" models "$SPIROLAB_FACE_FIXTURE"
fi
