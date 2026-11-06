#!/bin/sh
# Launch Nava Studio with bundled libraries
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -d "$HERE/lib" ]; then
    export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
elif [ -d "$HERE/build/ffmpeg-dist/lib" ]; then
    export LD_LIBRARY_PATH="$HERE/build/ffmpeg-dist/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec "$HERE/nava-studio" "$@"
