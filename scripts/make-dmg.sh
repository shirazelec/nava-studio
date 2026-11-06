#!/bin/sh
# Build a self-contained macOS .dmg (Apple Disk Image) installer for Nava Studio.
#
#   ./scripts/make-dmg.sh [build_dir]
#
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="${1:-$HERE/build}"
VERSION=$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' "$HERE/src/app.c" | head -1)
: "${VERSION:=1.0.0}"

if [ ! -f "$BUILD_DIR/nava-studio" ]; then
    echo "Error: $BUILD_DIR/nava-studio not found. Build the project first." >&2
    exit 1
fi

DIST="$HERE/dist"
mkdir -p "$DIST"

APP_NAME="Nava Studio"
APP_BUNDLE="$DIST/${APP_NAME}.app"
rm -rf "$APP_BUNDLE"

mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources"
mkdir -p "$APP_BUNDLE/Contents/libs"

# Copy binary as nava-studio-bin
cp "$BUILD_DIR/nava-studio" "$APP_BUNDLE/Contents/MacOS/nava-studio-bin"
chmod +x "$APP_BUNDLE/Contents/MacOS/nava-studio-bin"

# Create launcher script as nava-studio
cat > "$APP_BUNDLE/Contents/MacOS/nava-studio" << 'LAUNCHER'
#!/bin/sh
DIR="$(cd "$(dirname "$0")/.." && pwd)"
export DYLD_LIBRARY_PATH="$DIR/libs${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
if [ -d "$DIR/Resources/share/glib-2.0/schemas" ]; then
    export GSETTINGS_SCHEMA_DIR="$DIR/Resources/share/glib-2.0/schemas"
fi
if [ -d "$DIR/Resources/share" ]; then
    export XDG_DATA_DIRS="$DIR/Resources/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
fi
exec "$DIR/MacOS/nava-studio-bin" "$@"
LAUNCHER
chmod +x "$APP_BUNDLE/Contents/MacOS/nava-studio"

# Copy resources and data
cp -r "$HERE/resources" "$APP_BUNDLE/Contents/Resources/" 2>/dev/null || true
cp -r "$HERE/data" "$APP_BUNDLE/Contents/Resources/" 2>/dev/null || true

# Copy GLib schemas from Homebrew if present
for schema_dir in /opt/homebrew/share/glib-2.0/schemas /usr/local/share/glib-2.0/schemas; do
    if [ -d "$schema_dir" ]; then
        mkdir -p "$APP_BUNDLE/Contents/Resources/share/glib-2.0/schemas"
        cp -r "$schema_dir"/* "$APP_BUNDLE/Contents/Resources/share/glib-2.0/schemas/" 2>/dev/null || true
        if command -v glib-compile-schemas >/dev/null 2>&1; then
            glib-compile-schemas "$APP_BUNDLE/Contents/Resources/share/glib-2.0/schemas" 2>/dev/null || true
        fi
        break
    fi
done

# Bundle all dynamic libraries with dylibbundler
if command -v dylibbundler >/dev/null 2>&1; then
    echo "==> Bundling dynamic libraries with dylibbundler..."
    dylibbundler -od -b \
        -x "$APP_BUNDLE/Contents/MacOS/nava-studio-bin" \
        -d "$APP_BUNDLE/Contents/libs" \
        -p "@executable_path/../libs/" \
        -s /opt/homebrew/lib \
        -s /usr/local/lib || true
fi

# Deduplicate LC_RPATH load commands on macOS (modern dyld aborts on duplicates)
if command -v python3 >/dev/null 2>&1 && command -v otool >/dev/null 2>&1 && command -v install_name_tool >/dev/null 2>&1; then
    echo "==> Deduplicating LC_RPATH in bundled binaries and libraries..."
    python3 - "$APP_BUNDLE" << 'PYEOF'
import os, sys, subprocess, re

app_bundle = sys.argv[1] if len(sys.argv) > 1 else "dist/Nava Studio.app"

def get_rpaths(target):
    try:
        out = subprocess.check_output(["otool", "-l", target], text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return []
    rpaths = []
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for j in range(i + 1, min(i + 5, len(lines))):
                p_line = lines[j].strip()
                if p_line.startswith("path "):
                    m = re.match(r"^path\s+(.*?)(?:\s+\(offset\s+\d+\))?$", p_line)
                    if m:
                        rpaths.append(m.group(1).strip())
                    break
    return rpaths

def clean_file(target):
    for _ in range(25):
        rpaths = get_rpaths(target)
        if not rpaths:
            break
        seen = set()
        dup_to_delete = None
        for r in rpaths:
            norm = r.rstrip('/')
            if norm in seen or r in seen:
                dup_to_delete = r
                break
            seen.add(r)
            seen.add(norm)
        
        if dup_to_delete is None:
            break
        
        print(f"  Removing duplicate rpath '{dup_to_delete}' from {os.path.basename(target)}")
        subprocess.run(["install_name_tool", "-delete_rpath", dup_to_delete, target], capture_output=True)

    if os.path.basename(target) == "nava-studio-bin":
        rpaths = [r.rstrip('/') for r in get_rpaths(target)]
        if "@executable_path/../libs" not in rpaths:
            print("  Re-adding '@executable_path/../libs/' to nava-studio-bin")
            subprocess.run(["install_name_tool", "-add_rpath", "@executable_path/../libs/", target], capture_output=True)

for root, _, files in os.walk(app_bundle):
    for f in files:
        full = os.path.join(root, f)
        if os.path.islink(full):
            continue
        try:
            with open(full, 'rb') as fp:
                magic = fp.read(4)
                if magic in [b'\xfe\xed\xfa\xce', b'\xfe\xed\xfa\xcf',
                             b'\xce\xfa\xed\xfe', b'\xcf\xfa\xed\xfe',
                             b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca']:
                    clean_file(full)
        except Exception:
            pass
PYEOF
fi

# Generate iconset and .icns
if [ -f "$HERE/data/icons/nava-studio.png" ] && command -v sips >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
    ICONSET="$DIST/NavaStudio.iconset"
    rm -rf "$ICONSET"
    mkdir -p "$ICONSET"
    sips -z 16 16     "$HERE/data/icons/nava-studio.png" --out "$ICONSET/icon_16x16.png" >/dev/null 2>&1 || true
    sips -z 32 32     "$HERE/data/icons/nava-studio.png" --out "$ICONSET/icon_16x16@2x.png" >/dev/null 2>&1 || true
    sips -z 32 32     "$HERE/data/icons/nava-studio.png" --out "$ICONSET/icon_32x32.png" >/dev/null 2>&1 || true
    sips -z 64 64     "$HERE/data/icons/nava-studio.png" --out "$ICONSET/icon_32x32@2x.png" >/dev/null 2>&1 || true
    sips -z 128 128   "$HERE/data/icons/nava-studio.png" --out "$ICONSET/icon_128x128.png" >/dev/null 2>&1 || true
    sips -z 256 256   "$HERE/data/icons/nava-studio.png" --out "$ICONSET/icon_128x128@2x.png" >/dev/null 2>&1 || true
    sips -z 256 256   "$HERE/data/icons/nava-studio.png" --out "$ICONSET/icon_256x256.png" >/dev/null 2>&1 || true
    sips -z 512 512   "$HERE/data/icons/nava-studio.png" --out "$ICONSET/icon_256x256@2x.png" >/dev/null 2>&1 || true
    sips -z 512 512   "$HERE/data/icons/nava-studio.png" --out "$ICONSET/icon_512x512.png" >/dev/null 2>&1 || true
    iconutil -c icns "$ICONSET" -o "$APP_BUNDLE/Contents/Resources/nava-studio.icns" >/dev/null 2>&1 || true
    rm -rf "$ICONSET"
fi

# Write Info.plist
cat > "$APP_BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>nava-studio</string>
    <key>CFBundleIconFile</key>
    <string>nava-studio.icns</string>
    <key>CFBundleIdentifier</key>
    <string>ir.shirazelectronic.navastudio</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>Nava Studio</string>
    <key>CFBundleDisplayName</key>
    <string>Nava Studio</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$VERSION</string>
    <key>CFBundleVersion</key>
    <string>$VERSION</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSHumanReadableCopyright</key>
    <string>Copyright © Shiraz Electronic. All rights reserved.</string>
</dict>
</plist>
PLIST

# Final Ad-hoc Code Signing (MUST be performed last after Info.plist and all resources are in place)
if command -v codesign >/dev/null 2>&1; then
    echo "==> Cleaning extended attributes and ad-hoc signing bundle..."
    xattr -cr "$APP_BUNDLE" 2>/dev/null || true
    find "$APP_BUNDLE/Contents/libs" -name "*.dylib" -exec codesign --force -s - {} + 2>/dev/null || true
    find "$APP_BUNDLE/Contents/MacOS" -type f -exec codesign --force -s - {} + 2>/dev/null || true
    codesign --force --deep -s - "$APP_BUNDLE" 2>/dev/null || true
    codesign --verify --deep --strict "$APP_BUNDLE" 2>/dev/null && echo "Code signature verified successfully" || true
fi

# Create DMG with drag-and-drop to /Applications
DMG_STAGE="$DIST/.dmg-stage"
rm -rf "$DMG_STAGE"
mkdir -p "$DMG_STAGE"
cp -r "$APP_BUNDLE" "$DMG_STAGE/"
ln -s /Applications "$DMG_STAGE/Applications"

DMG_OUT="$DIST/NavaStudio-${VERSION}-macos-arm64.dmg"
rm -f "$DMG_OUT"

if command -v hdiutil >/dev/null 2>&1; then
    hdiutil create -volname "Nava Studio" \
        -srcfolder "$DMG_STAGE" \
        -ov -format UDZO \
        "$DMG_OUT"
    echo "Created DMG: $DMG_OUT"
else
    echo "hdiutil not available, skipping DMG creation" >&2
fi

rm -rf "$DMG_STAGE"
