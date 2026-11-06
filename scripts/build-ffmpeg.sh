#!/bin/sh
# Build the vendored FFmpeg 8 in place and install it into build/ffmpeg-dist.
# FFmpeg's configure refuses out-of-tree builds when the source path contains
# whitespace, so it is built inside third_party/ffmpeg.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC="$HERE/third_party/ffmpeg"
DIST="$HERE/build/ffmpeg-dist"

# x86 SIMD needs nasm or yasm; fall back to portable C when neither exists.
if command -v nasm >/dev/null 2>&1 || command -v yasm >/dev/null 2>&1; then
    X86ASM=
else
    X86ASM=--disable-x86asm
    echo "note: nasm/yasm not found, building FFmpeg without x86 SIMD"
fi

if [ ! -f "$SRC/ffbuild/config.mak" ]; then
    cd "$SRC"
    ./configure --prefix="$DIST" \
        --disable-doc --disable-debug --enable-shared --disable-static \
        --enable-gpl --enable-version3 \
        --disable-programs --disable-avdevice \
        --disable-autodetect $X86ASM
fi
make -C "$SRC" -j"$(nproc)"
make -C "$SRC" install
