#!/usr/bin/env bash
# Copy the needed subset of iPlug2 at the pinned commit into third_party/iPlug2.
# usage: scripts/vendor_iplug2.sh <iplug2-checkout>
#
# The checkout is only read: tracked files come from `git archive` of the pinned commit, so local
# modifications there cannot leak in. The VST3 SDK is not tracked by iPlug2 (it is a separate download);
# it is copied from the checkout's working tree and must match VST3_TREE_SHA256 below.
# Nothing is built here and the copy is never edited by hand. Our patches are applied by
# scripts/iplug2_patches/apply.py and listed in VENDORED.md; change them there and re-run this script.
set -euo pipefail
export LC_ALL=C

COMMIT=de5a4fb14fd964247f0c70f2c90b32876a3bfc7c
VST3_VERSION="VST 3.8.1"
VST3_TREE_SHA256=ccc801c4e5dbf28c01f3142d475bfb9c44360dc2b66abd1c8d8594845ae8abbf

[ $# -eq 1 ] || { echo "usage: $0 <iplug2-checkout>" >&2; exit 2; }
SRC="$(cd "$1" && pwd -P)"
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
DEST="${IPLUG2_VENDOR_DEST:-$ROOT/third_party/iPlug2}"   # the override is for checking this script
case "$DEST/" in "$SRC"/*) echo "refusing to write inside the reference checkout" >&2; exit 2;; esac
git -C "$SRC" cat-file -e "$COMMIT^{commit}" 2>/dev/null || { echo "commit $COMMIT is not in $SRC" >&2; exit 1; }

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

mkdir -p "$ROOT/third_party"
TMP="$(mktemp -d "$ROOT/third_party/.vendor.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# ---- iPlug2 and the dependencies it tracks ------------------------------------------------------
D=Dependencies
WDL_FILES="assocarray.h denormal.h dirscan.h heapbuf.h mergesort.h mutex.h ptrlist.h queue.h
  wdl_base64.h wdlatomic.h wdlcstring.h wdlendian.h wdlstring.h wdltypes.h wdlutf8.h lice/lice.h"
SWELL_FILES="commctrl.h shlobj.h windows.h swell.h swell.cpp swell-ini.cpp swellappmain.h swellappmain.mm
  swell-appstub.mm swell-dlg.mm swell-dlggen.h swell-functions.h swell-gdi.mm swell-gdi-internalpool.h
  swell-internal.h swell-kb.mm swell-menu.mm swell-menugen.h swell-misc.mm swell-miscdlg.mm swell-types.h
  swell-wnd.mm swell_resgen.pl swell_resgen.sh"

PATHS=(
  LICENSE.txt common-mac.xcconfig common-win.props
  Scripts/parse_config.py Scripts/parse_iostr.py Scripts/get_archive_name.py

  # IPlug: no AAX, AUv3, VST2, WEB, ReaperExt; of Extras only small iPlug2-licensed headers without extra deps
  ':(glob)IPlug/*' IPlug/APP IPlug/AUv2 IPlug/VST3
  IPlug/Extras/DCBlocker.h IPlug/Extras/Easing.h IPlug/Extras/NChanDelay.h IPlug/Extras/Smoothers.h

  # IGraphics: NanoVG backend on macOS and Windows; no Extras, test controls, Skia, WebView
  ':(glob)IGraphics/*' ':(glob)IGraphics/Controls/*'
  IGraphics/Drawing/IGraphicsNanoVG.h IGraphics/Drawing/IGraphicsNanoVG.cpp IGraphics/Drawing/IGraphicsNanoVG_src.m
  ':(glob)IGraphics/Platforms/IGraphicsMac*' ':(glob)IGraphics/Platforms/IGraphicsWin*'
  ':(glob)IGraphics/Platforms/IGraphicsCoreText*'

  # Standalone I/O. The `include` folder (ASIO) is deliberately absent.
  $D/IPlug/RTAudio/RtAudio.h $D/IPlug/RTAudio/RtAudio.cpp $D/IPlug/RTAudio/README.md
  $D/IPlug/RTMidi/RtMidi.h $D/IPlug/RTMidi/RtMidi.cpp $D/IPlug/RTMidi/README.md

  $D/IGraphics/NanoVG/LICENSE.txt $D/IGraphics/NanoVG/src
  $D/IGraphics/NanoSVG/LICENSE.txt $D/IGraphics/NanoSVG/src
  $D/IGraphics/MetalNanoVG/LICENSE
  $D/IGraphics/MetalNanoVG/src/nanovg_mtl.h $D/IGraphics/MetalNanoVG/src/nanovg_mtl.m
  $D/IGraphics/MetalNanoVG/src/nanovg_mtl_shaders.metal $D/IGraphics/MetalNanoVG/src/mnvg_bitcode/macos.h
  $D/IGraphics/STB
  $D/IGraphics/glad_GL2    # the GL2 backend on Windows does not compile without it
)
for f in $WDL_FILES; do PATHS+=("WDL/$f"); done
for f in $SWELL_FILES; do PATHS+=("WDL/swell/$f"); done

git -C "$SRC" -c core.autocrlf=false archive --format=tar "$COMMIT" -- "${PATHS[@]}" | tar -x -C "$TMP" -f -
rm -f "$TMP/IGraphics/Controls/IWebViewControl.h"

# ---- VST3 SDK (MIT): what the single-component plug-in targets compile, not hosting, wrappers or VSTGUI
SDK="$SRC/$D/IPlug/VST3_SDK"
OUT="$TMP/$D/IPlug/VST3_SDK"
mkdir -p "$OUT"
grep -q "\"$VST3_VERSION\"" "$SDK/pluginterfaces/vst/vsttypes.h" || { echo "expected $VST3_VERSION" >&2; exit 1; }
P=public.sdk/source
(
  cd "$SDK"
  {
    echo LICENSE.txt
    echo base/LICENSE.txt
    echo pluginterfaces/LICENSE.txt
    echo public.sdk/LICENSE.txt
    find base/source base/thread pluginterfaces/base pluginterfaces/gui pluginterfaces/vst -type f \( -name '*.h' -o -name '*.cpp' \)
    for f in memorystream pluginview; do echo $P/common/$f.h; echo $P/common/$f.cpp; done
    echo $P/common/commoniids.cpp
    for f in pluginfactory.h pluginfactory.cpp pluginfactory_constexpr.h moduleinit.h moduleinit.cpp macmain.cpp dllmain.cpp macexport.exp; do echo $P/main/$f; done
    for f in vstaudioeffect vstbus vstcomponent vstcomponentbase vsteditcontroller vstnoteexpressiontypes \
             vstparameters vstpresetfile vstrepresentation vstsinglecomponenteffect; do echo $P/vst/$f.h; echo $P/vst/$f.cpp; done
    echo $P/vst/vstinitiids.cpp
    for f in vstaudioprocessoralgo vsteventshelper vsthelpers vstspeakerarray utility/processcontextrequirements; do echo $P/vst/$f.h; done
    for f in h cpp; do echo $P/vst/hosting/parameterchanges.$f; done
  } | sort > "$TMP/.vst3-files"
  tar -c -f - -T "$TMP/.vst3-files"
) | tar -x -C "$OUT" -f -
GOT="$(cd "$OUT" && while read -r f; do sha256 "$f"; done < "$TMP/.vst3-files" | sha256 | cut -d' ' -f1)"
rm "$TMP/.vst3-files"
[ "$GOT" = "$VST3_TREE_SHA256" ] || { echo "VST3 SDK subset differs from the pin: got $GOT" >&2; exit 1; }

# ---- our patches ------------------------------------------------------------------------------
python3 "$ROOT/scripts/iplug2_patches/apply.py" "$TMP"

# ---- record what was copied ----------------------------------------------------------------------
{
  echo "# Vendored iPlug2"
  echo
  echo "Generated by \`scripts/vendor_iplug2.sh\`, then patched as listed under Patches. Make no other edits:"
  echo "\`scripts/license_audit.py\` checks every file against \`MANIFEST.sha256\`."
  echo
  echo "* iPlug2 commit: \`$COMMIT\`"
  echo "* VST3 SDK: $VST3_VERSION (MIT), separate download, subset pinned by SHA-256 \`$VST3_TREE_SHA256\`"
  echo "* Licences per path: \`../LICENSES.json\`; texts: \`../licenses/\` and the licence files in this tree"
  echo
  echo "Left out on purpose: AAX, VST2, AUv3, WEB and ReaperExt wrappers; the ASIO headers of RtAudio; Extras"
  echo "with other licences or such dependencies (HIIR/Oversampler, LanczosResampler/RealtimeResampler, NoiseGate,"
  echo "OSC, Faust); IGraphics Extras, Skia and WebView; WDL beyond the include closure of the files below"
  echo "(in particular swell/gtk* and resample.*); SDK logos."
  echo
  echo "## Patches"
  echo
  echo "1. \`IGraphics/Platforms/IGraphicsMac_view.h\` and \`IGraphicsMac_view.mm\`: the number-only text entry formatter"
  echo "   (\`IGRAPHICS_FORMATTER\`, its interface, implementation and one call site in \`createTextEntry\`) was replaced."
  echo "   Upstream credited it to a blog post that states no licence. The replacement is MIT, part of this project,"
  echo "   and was written from the \`NSFormatter\` API without using the removed code: an allowed character set and a"
  echo "   length limit, checked on each edit."
  echo
  echo "\`scripts/vendor_iplug2.sh\` applies them with \`scripts/iplug2_patches/apply.py\`, which holds the replacement text."
  echo
  echo "## Files"
  echo
  (cd "$TMP" && find . -type f ! -name VENDORED.md | sed 's|^\./|    |' | sort)
} > "$TMP/VENDORED.md"
(cd "$TMP" && find . -type f ! -name MANIFEST.sha256 | sed 's|^\./||' | sort | while read -r f; do sha256 "$f"; done) > "$TMP/MANIFEST.sha256"

chmod 755 "$TMP"
rm -rf "$DEST"
mv "$TMP" "$DEST"
trap - EXIT
echo "vendored $(wc -l < "$DEST/MANIFEST.sha256" | tr -d ' ') files into third_party/iPlug2 at $COMMIT"
