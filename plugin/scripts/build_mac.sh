#!/usr/bin/env bash
# Build the plug-in from the vendored iPlug2, without installing anything and with ad-hoc signing only.
# usage: plugin/scripts/build_mac.sh [APP] [AU] [VST3]      (default: all three)
#   MAGMORA_BUILD_DIR  build root      (default $BLOCKAMP_PRIVATE/build/plugin)
#   MAGMORA_ARCHS      architectures   (default arm64; CI may use "arm64 x86_64")
#   MAGMORA_CONFIG     Release (default) or Debug
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
. "$ROOT/scripts/env.sh"

B="${MAGMORA_BUILD_DIR:-$BLOCKAMP_PRIVATE/build/plugin}"
ARCH_LIST="${MAGMORA_ARCHS:-arm64}"
CONFIG="${MAGMORA_CONFIG:-Release}"
case "$CONFIG" in Release|Debug) ;; *) echo "MAGMORA_CONFIG must be Release or Debug" >&2; exit 2;; esac
mkdir -p "$B/logs"
B="$(cd "$B" && pwd -P)"

PRODUCT() { python3 "$ROOT/plugin/scripts/product.py" get "$1"; }
NAME="$(PRODUCT BUNDLE_NAME)"
PREFIX="$(PRODUCT BUNDLE_DOMAIN).$(PRODUCT BUNDLE_MFR)"
MIN_OS="$(sed -n 's/^MACOSX_DEPLOYMENT_TARGET = //p' "$ROOT/plugin/config/Magmora-mac.xcconfig")"

FORMATS=("$@")
[ ${#FORMATS[@]} -gt 0 ] || FORMATS=(APP AU VST3)
for f in "${FORMATS[@]}"; do
  case "$f" in APP|AU|VST3) ;; *) echo "unknown format: $f" >&2; exit 2;; esac
done

python3 "$ROOT/plugin/scripts/generate.py"   # layout, presets and the Windows resource list

# The DSP library comes from CMake, so the plug-in has the sources and flags that the tests have.
cmake -S "$ROOT" -B "$B/dsp" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" -DBUILD_TESTING=OFF \
  -DMAGMORA_BUILD_TOOLS=OFF -DMAGMORA_BUILD_BENCH=OFF \
  -DCMAKE_OSX_ARCHITECTURES="${ARCH_LIST// /;}" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_OS" > "$B/logs/dsp.log"
cmake --build "$B/dsp" --target magmora_dsp --parallel "${MAGMORA_JOBS:-3}" | tee -a "$B/logs/dsp.log" | tail -1

# Release builds drop debug symbols before signing: their debug map holds the build machine's absolute paths.
STRIP_SETTINGS=()
[ "$CONFIG" = Release ] && STRIP_SETTINGS=(DEPLOYMENT_POSTPROCESSING=YES STRIP_INSTALLED_PRODUCT=YES STRIP_STYLE=debugging)

for f in "${FORMATS[@]}"; do
  case "$f" in
    APP)  scheme=macOS-APP;   ext=app ;;
    AU)   scheme=macOS-AUv2;  ext=component ;;
    VST3) scheme=macOS-VST3;  ext=vst3 ;;
  esac
  log="$B/logs/$f.log"
  if ! xcodebuild -project "$ROOT/plugin/projects/Magmora-macOS.xcodeproj" -scheme "$scheme" \
      -jobs "${MAGMORA_JOBS:-3}" -configuration "$CONFIG" -destination 'generic/platform=macOS' -derivedDataPath "$B/DerivedData" \
      MAGMORA_BUILD_ROOT="$B" SYMROOT="$B/products" OBJROOT="$B/obj" \
      MAGMORA_DSP_LIB="$B/dsp/dsp/libmagmora_dsp.a" BINARY_NAME="$NAME" MAGMORA_BUNDLE_PREFIX="$PREFIX" \
      ARCHS="$ARCH_LIST" DEPLOYMENT_LOCATION=NO ${STRIP_SETTINGS[@]+"${STRIP_SETTINGS[@]}"} build > "$log" 2>&1; then
    grep -E "(error:|BUILD FAILED)" "$log" | head -30 >&2 || true
    echo "$f failed, see $log" >&2
    exit 1
  fi
  if [ "$f" = APP ]; then
    # Xcode registers every app it builds with LaunchServices; undo that, a build is not an install.
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
      -u "$B/products/$CONFIG/$NAME.app" >/dev/null 2>&1 || true
  fi
  echo "$f: $B/products/$CONFIG/$NAME.$ext"
done
