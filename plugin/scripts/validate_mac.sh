#!/usr/bin/env bash
# Validate the plug-ins in the build folder.
# usage: plugin/scripts/validate_mac.sh [--registered-au | --temporary-au-copy]
#
#   default               pluginval at strictness 10 on the VST3, and the host harness on VST3 and AU
#                         (the harness registers the AU in its own process; nothing is copied)
#   --registered-au       also auval and pluginval on an AU that is already registered with the system
#   --temporary-au-copy   the same, after copying the component into ~/Library/Audio/Plug-Ins/Components.
#                         Refuses if one is there already, and removes its own copy again on exit.
#
#   SOAK_SECONDS  audio seconds of random automation in the harness (default 60; gate G8 wants 1800)
#   PLUGINVAL     the pluginval binary
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
. "$ROOT/scripts/env.sh"
MODE="${1:-}"
case "$MODE" in ''|--registered-au|--temporary-au-copy) ;; *) sed -n '2,12p' "$0" >&2; exit 2;; esac

B="${MAGMORA_BUILD_DIR:-$BLOCKAMP_PRIVATE/build/plugin}"
P="$B/products/${MAGMORA_CONFIG:-Release}"
LOGS="$B/logs"
PLUGINVAL="${PLUGINVAL:-/Applications/pluginval.app/Contents/MacOS/pluginval}"
CONFIG() { python3 "$ROOT/plugin/scripts/product.py" get "$1"; }
NAME="$(CONFIG BUNDLE_NAME)"; SUB="$(CONFIG PLUG_UNIQUE_ID)"; MFR="$(CONFIG PLUG_MFR_ID)"
mkdir -p "$LOGS"
FAILED=0

check() {  # log name, command...
  local log="$LOGS/validate-$1.log"; shift
  if "$@" > "$log" 2>&1; then echo "pass  $log"; else echo "FAIL  $log"; FAILED=1; fi
}

check pluginval-vst3 "$PLUGINVAL" --strictness-level 10 --timeout-ms 300000 \
  --sample-rates 44100,48000,96000,192000 --block-sizes 1,64,128,256,512,1024,4096 --validate "$P/$NAME.vst3"

HOST="$("$ROOT/plugin/scripts/build_test_host.sh")"
for bundle in "$NAME.vst3" "$NAME.component"; do
  for test in audio state automation ui; do check "$bundle-$test" "$HOST" "$P/$bundle" "$test"; done
  check "$bundle-soak" "$HOST" "$P/$bundle" soak "${SOAK_SECONDS:-60}"
done

if [ -n "$MODE" ]; then
  if [ "$MODE" = --temporary-au-copy ]; then
    DEST="$HOME/Library/Audio/Plug-Ins/Components/$NAME.component"
    if [ -e "$DEST" ]; then echo "$DEST exists: not touching it" >&2; exit 2; fi
    trap 'rm -rf "$DEST"; killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true' EXIT
    mkdir -p "$(dirname "$DEST")"
    cp -R "$P/$NAME.component" "$DEST"
    killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
  fi
  check auval auval -v aufx "$SUB" "$MFR"
  check pluginval-au "$PLUGINVAL" --strictness-level 10 --timeout-ms 300000 --validate "AudioUnit:Effects/aufx,$SUB,$MFR"
else
  echo "skip  auval and pluginval on the AU: they need a registered component (see the options)"
fi
exit "$FAILED"
