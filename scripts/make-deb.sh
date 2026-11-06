#!/bin/sh
# Build a self-contained .deb from the local build tree.
#
#   ./scripts/make-deb.sh
#
# The package bundles the FFmpeg 8 shared libraries next to the binary, so the
# only runtime requirements are the GTK4 stack plus PulseAudio.  On install the
# postinst registers Nava Studio as the default handler for every audio/video
# MIME type it advertises, for the user running the install.
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
NAME=nava-studio
VERSION=$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' "$HERE/src/app.c" | head -1)
: "${VERSION:=1.0.0}"
ARCH=${DEB_ARCH:-$(dpkg --print-architecture)}

# Lowest glibc symbol version the binary actually needs (read from the ELF).
GLIBC_MIN=$(objdump -T "$HERE/nava-studio" 2>/dev/null \
    | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -Vu | tail -1)
: "${GLIBC_MIN:=2.35}"

DIST="$HERE/dist"
ROOT="$DIST/.deb-root"
rm -rf "$ROOT"
mkdir -p "$ROOT/DEBIAN" \
         "$ROOT/opt/$NAME/lib" \
         "$ROOT/usr/bin" \
         "$ROOT/usr/share/applications" \
         "$ROOT/usr/share/icons/hicolor/256x256/apps" \
         "$ROOT/usr/lib/$NAME"

install -m 755 "$HERE/nava-studio" "$ROOT/opt/$NAME/nava-studio"
cp -a "$HERE/build/ffmpeg-dist/lib/." "$ROOT/opt/$NAME/lib/"
ln -s "/opt/$NAME/nava-studio" "$ROOT/usr/bin/nava-studio"
install -m 644 "$HERE/data/nava-studio.desktop" "$ROOT/usr/share/applications/"
install -m 644 "$HERE/data/icons/nava-studio.png" \
    "$ROOT/usr/share/icons/hicolor/256x256/apps/"
install -m 755 "$HERE/scripts/register-mime.sh" "$ROOT/usr/lib/$NAME/register-mime.sh"

SIZE=$(du -sk "$ROOT/opt" | cut -f1)
cat > "$ROOT/DEBIAN/control" <<CTRL
Package: $NAME
Version: $VERSION
Section: sound
Priority: optional
Architecture: $ARCH
Installed-Size: $SIZE
Depends: libc6 (>= $GLIBC_MIN), libgtk-4-1, libglib2.0-0, libcairo2, libpulse0, libgraphene-1.0-0, libvulkan1
Recommends: xdg-utils, desktop-file-utils, hicolor-icon-theme
Maintainer: Shiraz Electronic <info@shirazelectronic.ir>
Homepage: https://shirazelectronic.ir
Description: GTK4 media player powered by FFmpeg 8
 Nava Studio plays audio and video through a bundled FFmpeg 8 build.
 It provides a playlist, 10-band equalizer, audio effects and 14 live
 stage visualizers driven by the decoded audio spectrum and waveform.
 .
 Installing the package makes Nava Studio the default application for the
 audio and video formats it supports.
CTRL

cat > "$ROOT/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e

# The user who invoked sudo, or the logged-in user on a desktop session.
target_user=""
if [ -n "$SUDO_USER" ] && [ "$SUDO_USER" != root ]; then
    target_user=$SUDO_USER
elif [ -n "$PKEXEC_UID" ]; then
    target_user=$(getent passwd "$PKEXEC_UID" 2>/dev/null | cut -d: -f1)
else
    # Fall back to the owner of an active graphical session.
    target_user=$(loginctl list-sessions --no-legend 2>/dev/null \
        | awk '$0 ~ /seat/ {print $3; exit}')
    [ -n "$target_user" ] || target_user=$(id -un 1000 2>/dev/null || true)
fi

command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor || true

if [ -n "$target_user" ] && [ "$target_user" != root ]; then
    NAVA_DESKTOP_FILE=/usr/share/applications/nava-studio.desktop \
        sh /usr/lib/nava-studio/register-mime.sh register --user="$target_user" || true
else
    NAVA_DESKTOP_FILE=/usr/share/applications/nava-studio.desktop \
        sh /usr/lib/nava-studio/register-mime.sh register || true
fi
exit 0
POST

cat > "$ROOT/DEBIAN/postrm" <<'POST'
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    command -v update-desktop-database >/dev/null 2>&1 && \
        update-desktop-database /usr/share/applications || true
    NAVA_DESKTOP_FILE=/usr/share/applications/nava-studio.desktop \
        sh /usr/lib/nava-studio/register-mime.sh unregister || true
fi
exit 0
POST

chmod 755 "$ROOT/DEBIAN/postinst" "$ROOT/DEBIAN/postrm"
dpkg-deb --root-owner-group --build "$ROOT" "$DIST/${NAME}_${VERSION}_${ARCH}.deb" >/dev/null
rm -rf "$ROOT"
echo "==> $DIST/${NAME}_${VERSION}_${ARCH}.deb (glibc >= $GLIBC_MIN)"
