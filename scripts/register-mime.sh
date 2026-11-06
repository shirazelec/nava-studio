#!/bin/sh
# Register Nava Studio as the default handler for every media type listed in
# its .desktop file.
#
#   register-mime.sh register   [--user=NAME] [--local-only]
#   register-mime.sh unregister [--user=NAME] [--local-only]
#
# The .deb postinst calls this as root with --user=<desktop user>: the system
# wide fallback goes to /etc/xdg/mimeapps.list and the real user's file
# (~/.config/mimeapps.list) is rewritten as that user, never as root.
set -e

MODE=${1:-register}
shift 2>/dev/null || true

TARGET_USER=""
LOCAL_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --user=*)   TARGET_USER=${arg#--user=} ;;
        --local-only) LOCAL_ONLY=1 ;;
    esac
done

DESKTOP=nava-studio.desktop
DESKTOP_FILE=${NAVA_DESKTOP_FILE:-/usr/share/applications/$DESKTOP}
if [ ! -f "$DESKTOP_FILE" ]; then
    DESKTOP_FILE="$(cd "$(dirname "$0")/.." 2>/dev/null && pwd)/data/$DESKTOP"
fi

TYPES=$(sed -n 's/^MimeType=//p' "$DESKTOP_FILE" 2>/dev/null | tr ';' ' ' | tr -s ' ')
if [ -z "$TYPES" ]; then
    echo "register-mime: no MimeType entries in $DESKTOP_FILE" >&2
    exit 0
fi

# Rewrite one mimeapps.list: force every supported type to nava-studio
# (register) or drop nava-studio from the front (unregister). Everything else
# in the file, including other sections, is preserved.
rewrite_mimeapps() {
    file=$1
    [ -n "$file" ] || return 0
    mkdir -p "$(dirname "$file")" 2>/dev/null || true
    [ -f "$file" ] || printf '[Default Applications]\n' > "$file"

    tmp=$(mktemp) || return 0
    awk -v types="$TYPES" -v desktop="$DESKTOP" -v mode="$MODE" '
        function flush_wanted(   t) {
            if (mode != "register") return
            for (t in want)
                if (!(t in seen)) print t "=" desktop ";"
        }
        BEGIN {
            n = split(types, T, " ")
            for (i = 1; i <= n; i++) if (T[i] != "") want[T[i]] = 1
        }
        {
            line = $0
            if (line ~ /^\[/) {
                if (in_defs) flush_wanted()
                print line
                in_defs = (line == "[Default Applications]")
                next
            }
            if (in_defs && index(line, "=") > 0) {
                k = substr(line, 1, index(line, "=") - 1)
                if (k in want) {
                    v = substr(line, index(line, "=") + 1)
                    gsub(/[ \t\r]+$/, "", v)
                    m = split(v, A, ";")
                    out = ""
                    for (j = 1; j <= m; j++) {
                        if (A[j] == "" || A[j] == desktop) continue
                        out = out A[j] ";"
                    }
                    if (mode == "register") out = desktop ";" out
                    if (out != "") print k "=" out
                    seen[k] = 1
                    next
                }
            }
            print line
        }
        END { if (in_defs) flush_wanted() }
    ' "$file" > "$tmp" 2>/dev/null || { rm -f "$tmp"; return 0; }

    if ! grep -q '^\[Default Applications\]' "$tmp"; then
        { printf '[Default Applications]\n'; cat "$tmp"; } > "$tmp.2" && mv "$tmp.2" "$tmp"
    fi
    cat "$tmp" > "$file" 2>/dev/null || true
    rm -f "$tmp" "$tmp.2"
}

# Rewrite the current user's per-user file.
local_file="${XDG_CONFIG_HOME:-$HOME/.config}/mimeapps.list"
rewrite_mimeapps "$local_file"

# ---- system-wide fallback + delegation to the desktop user -------------
if [ "$LOCAL_ONLY" = 0 ] && [ "$(id -u)" = 0 ]; then
    rewrite_mimeapps /etc/xdg/mimeapps.list

    if [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != root ]; then
        h=$(getent passwd "$TARGET_USER" 2>/dev/null | cut -d: -f6)
        if [ -n "$h" ] && [ -d "$h" ]; then
            self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
            su -s /bin/sh "$TARGET_USER" -c \
                "HOME='$h' XDG_CONFIG_HOME='$h/.config' sh '$self' '$MODE' --local-only" \
                >/dev/null 2>&1 || true
        fi
    fi
fi

command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
exit 0
