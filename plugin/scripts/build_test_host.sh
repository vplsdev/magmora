#!/usr/bin/env bash
# Build the test host (plugin/tests) and print its path. Developer tool: never linked into or shipped with a plug-in.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
. "$ROOT/scripts/env.sh"
OUT="${MAGMORA_BUILD_DIR:-$BLOCKAMP_PRIVATE/build/plugin}/tests"
SDK="$ROOT/third_party/iPlug2/Dependencies/IPlug/VST3_SDK"
mkdir -p "$OUT"
export TMPDIR="$OUT"
cc -O1 -dynamiclib -framework ApplicationServices "$ROOT/plugin/tests/no_cursor.c" \
  -install_name "@executable_path/libno_cursor.dylib" -o "$OUT/libno_cursor.dylib"
c++ -O1 -std=c++17 -fobjc-arc -DRELEASE=1 -DNDEBUG=1 -DMAGMORA_SOURCE_DIR="\"$ROOT\"" -Wno-deprecated-declarations \
  -I"$ROOT/plugin/tests" -I"$ROOT/dsp/include" -I"$SDK" \
  "$ROOT/plugin/tests/host.mm" \
  "$SDK/pluginterfaces/base/funknown.cpp" "$SDK/pluginterfaces/base/coreiids.cpp" \
  "$SDK/public.sdk/source/common/commoniids.cpp" "$SDK/public.sdk/source/vst/vstinitiids.cpp" \
  "$SDK/public.sdk/source/common/memorystream.cpp" "$SDK/public.sdk/source/vst/hosting/parameterchanges.cpp" \
  "$OUT/../dsp/dsp/libmagmora_dsp.a" -framework Cocoa -framework AudioToolbox -framework Metal -framework QuartzCore -framework Accelerate \
  "$OUT/libno_cursor.dylib" -o "$OUT/magmora-host"
echo "$OUT/magmora-host"
