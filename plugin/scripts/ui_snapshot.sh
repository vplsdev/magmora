#!/usr/bin/env bash
# Screenshots of the plug-in's editor (1x and 2x, 75 to 150 %, knob positions, states), read back from its own
# drawable in a window that is never shown. usage: plugin/scripts/ui_snapshot.sh [output folder]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
. "$ROOT/scripts/env.sh"
B="${MAGMORA_BUILD_DIR:-$BLOCKAMP_PRIVATE/build/plugin}"
OUT="${1:-$BLOCKAMP_PRIVATE/renders/ui}"
NAME="$(python3 "$ROOT/plugin/scripts/product.py" get BUNDLE_NAME)"
mkdir -p "$OUT"
HOST="$("$ROOT/plugin/scripts/build_test_host.sh")"
for bundle in "$NAME.vst3" "$NAME.component"; do
  "$HOST" "$B/products/${MAGMORA_CONFIG:-Release}/$bundle" shots "$OUT"
done
