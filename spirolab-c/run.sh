#!/usr/bin/env bash
set -euo pipefail
app_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ ! -x "$app_dir/spirolab" ]]; then
    make -C "$app_dir"
fi
exec "$app_dir/spirolab" "$@"
