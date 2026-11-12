#!/usr/bin/env bash
# Builds the Linux downloads from a CMake Release build directory:
#   scripts/package_linux.sh <build-dir> <out-dir> <version>
# Writes <out-dir>/CppDesk-x86_64.AppImage and <out-dir>/cppdesk_<version>_amd64.deb.
set -euo pipefail

BUILD=$(realpath "$1")
OUT=$(realpath -m "$2")
VERSION="$3"

# Pinned tool releases, so a rebuild of the same tag produces the same kind of AppImage.
LINUXDEPLOY_TAG="1-alpha-20251107-1"
APPIMAGE_PLUGIN_TAG="1-alpha-20250213-1"
TOOLS="${LINUXDEPLOY_CACHE:-$HOME/.cache/cppdesk-linuxdeploy}"

mkdir -p "$OUT" "$TOOLS"
LINUXDEPLOY="$TOOLS/linuxdeploy-$LINUXDEPLOY_TAG-x86_64.AppImage"
APPIMAGE_PLUGIN="$TOOLS/plugin-$APPIMAGE_PLUGIN_TAG/linuxdeploy-plugin-appimage-x86_64.AppImage"
# These tools run on the build machine, so each download must match its recorded checksum.
fetch() {   # <file> <url> <sha256>
    if [ ! -x "$1" ]; then
        mkdir -p "$(dirname "$1")"
        curl -fsSL -o "$1.part" "$2"
        chmod +x "$1.part"
        mv "$1.part" "$1"
    fi
    echo "$3  $1" | sha256sum -c --quiet -
}
fetch "$LINUXDEPLOY" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-x86_64.AppImage" \
    c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
fetch "$APPIMAGE_PLUGIN" \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/$APPIMAGE_PLUGIN_TAG/linuxdeploy-plugin-appimage-x86_64.AppImage" \
    992d502a248e14ab185448ddf6f6e7d25558cb84d4623c354c3af350c25fccb3

# AppImage: the installed layout plus the shared libraries it links, in one file. No FUSE needed to build.
export APPIMAGE_EXTRACT_AND_RUN=1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
APPDIR="$WORK/AppDir"
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr
# linuxdeploy leaves out libraries it assumes every desktop has. Pango needs FriBidi, which a minimal system
# lacks, so carry it too.
FRIBIDI=$(ldconfig -p | awk '/libfribidi\.so\.0 /{print $NF; exit}')
(
    cd "$WORK"
    # linuxdeploy finds its output plugin by name on PATH.
    PATH="$(dirname "$APPIMAGE_PLUGIN"):$PATH" OUTPUT="CppDesk-x86_64.AppImage" "$LINUXDEPLOY" \
        --appdir "$APPDIR" \
        --desktop-file "$APPDIR/usr/share/applications/cppdesk.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/cppdesk.png" \
        ${FRIBIDI:+--library "$FRIBIDI"} \
        --output appimage
    mv CppDesk-x86_64.AppImage "$OUT/"
)

# .deb: CPack, with dependencies worked out from the binary's libraries.
(cd "$BUILD" && cpack -G DEB -B "$WORK/deb" > "$WORK/cpack.log") || { cat "$WORK/cpack.log"; exit 1; }
cp "$WORK/deb/cppdesk_${VERSION}_amd64.deb" "$OUT/"

ls -la "$OUT/CppDesk-x86_64.AppImage" "$OUT/cppdesk_${VERSION}_amd64.deb"
