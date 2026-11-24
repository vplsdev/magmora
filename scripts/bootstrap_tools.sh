#!/bin/bash
# Build the developer tools into a private directory: a Python venv and ngspice (no global installs).
# usage: scripts/bootstrap_tools.sh [/path/to/private/dir]      (default: $BLOCKAMP_PRIVATE or ./.tools)
# ngspice is BSD-licensed and is only ever run as a separate program; nothing from it is linked.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
P="${1:-${BLOCKAMP_PRIVATE:-$ROOT/.tools}}"
mkdir -p "$P/tools/src" && cd "$P"
export UV_CACHE_DIR="$P/uv-cache"
[ -x venv/bin/python ] || uv venv --python 3.12 venv
uv pip install --python venv/bin/python -r "$ROOT/proto/requirements.txt"
if [ ! -x tools/ngspice/bin/ngspice ]; then
  V=45.2
  SHA=ba8345f4c3774714c10f33d7da850d361cec7d14b3a295d0dc9fd96f7423812d
  cd tools/src
  curl -sL -o ngspice-$V.tar.gz "https://sourceforge.net/projects/ngspice/files/ng-spice-rework/old-releases/$V/ngspice-$V.tar.gz/download"
  echo "$SHA  ngspice-$V.tar.gz" | shasum -a 256 -c -
  tar xzf ngspice-$V.tar.gz && mkdir -p ngspice-$V/build && cd ngspice-$V/build
  ../configure --prefix="$P/tools/ngspice" --disable-debug --without-x --with-readline=no --disable-openmp \
     --enable-cider=no --enable-osdi=no --enable-xspice=no CFLAGS="-O2" > configure.log
  # the tarball's timestamps make `make` want to re-run the autotools; they are not needed
  AT="ACLOCAL=: AUTOCONF=: AUTOMAKE=: AUTOHEADER=: MAKEINFO=:"
  make -j8 $AT CXXFLAGS="-O2 -Wno-invalid-specialization" > make.log
  make install $AT > install.log
fi
echo "tools ready in $P ; now: source scripts/env.sh"
