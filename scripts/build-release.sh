#!/bin/sh
# Build a portable tarball plus the .deb package from the current tree.
# The .deb itself is produced by scripts/make-deb.sh (single source of truth).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"

VERSION=$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' src/app.c | head -1)
: "${VERSION:=1.0.0}"
ARCH=$(dpkg --print-architecture 2>/dev/null || uname -m)

echo "==> Building Nava Studio $VERSION"
make -j"$(nproc)"

echo "==> Building .deb"
sh scripts/make-deb.sh

REL="release-$VERSION"
rm -rf "$REL"
mkdir -p "$REL"
cp nava-studio "$REL/"
cp -r resources "$REL/"
cp -r data "$REL/"
mkdir -p "$REL/lib"
cp -a build/ffmpeg-dist/lib/. "$REL/lib/"
strip -s "$REL/nava-studio" "$REL/lib/"*.so* 2>/dev/null || true
cp scripts/run.sh "$REL/"
chmod +x "$REL/run.sh"

echo "==> Creating tarball"
TARBALL="dist/NavaStudio-$VERSION-linux-$ARCH.tar.gz"
tar czf "$TARBALL" -C "$REL" .
rm -rf "$REL"

echo "==> Release files:"
ls -lh "$TARBALL" dist/nava-studio_"$VERSION"_"$ARCH".deb
