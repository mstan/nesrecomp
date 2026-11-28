#!/usr/bin/env bash
# Shared NES macOS build/bundle implementation; source from a title wrapper.
set -euo pipefail
source "$NES_ENGINE/tools/platform_common.sh"
CONFIG="prod"; DO_REGEN=0; DO_DMG=1
ARCH="$(uname -m)"   # arm64 on Apple Silicon, x86_64 on Intel
REPO="${REPO:?Set REPO to the consumer root}"
OUT="$REPO/release-macos"

while [ $# -gt 0 ]; do
  case "$1" in
    --config) CONFIG="$2"; shift 2;;
    --prod) CONFIG="prod"; shift;;
    --debug) CONFIG="debug"; shift;;
    --regen) DO_REGEN=1; shift;;
    --no-dmg) DO_DMG=0; shift;;
    --arch) ARCH="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --rom) NESRECOMP_ROM="$2"; shift 2;;
    --region) REGION="$2"; shift 2;;
    --jobs) NESRECOMP_JOBS="$2"; shift 2;;
    --build-dir) NESRECOMP_BUILD_DIR="$2"; shift 2;;
    -h|--help) sed -n '2,30p' "$0"; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
case "$CONFIG" in prod) FLAGS=( "${PROD_CMAKE_FLAGS[@]}" );; debug) FLAGS=( "${DEBUG_CMAKE_FLAGS[@]}" );;
  *) echo "--config must be prod or debug" >&2; exit 2;; esac
[ "$(uname -s)" = "Darwin" ] || { echo "ERROR: run this on macOS." >&2; exit 1; }
FLAGS+=( -DNESRECOMP_BACKEND=cycle -DNESRECOMP_PPU_IMPL=HLE )
if declare -F nes_platform_configure >/dev/null; then nes_platform_configure; fi
if [ -n "${NESRECOMP_ROM:-}" ]; then FLAGS+=( "-DNESRECOMP_ROM=$NESRECOMP_ROM" ); fi

case "$ARCH" in
  universal) OSX_ARCHS="x86_64;arm64";;
  arm64|x86_64) OSX_ARCHS="$ARCH";;
  *) echo "--arch must be arm64, x86_64, or universal" >&2; exit 2;;
esac

BUILD="${NESRECOMP_BUILD_DIR:-$REPO/build-macos-$CONFIG${BUILD_SUFFIX:-}}"
echo "==================== $APP_NAME ($CONFIG, $ARCH) ===================="
cd "$REPO"

RAN_PREBUILD=0
cleanup() { [ "$RAN_PREBUILD" = "1" ] && [ -n "$POSTBUILD_CMD" ] && { echo "[postbuild] $POSTBUILD_CMD"; eval "$POSTBUILD_CMD" || true; }; return 0; }
trap cleanup EXIT

[ "$DO_REGEN" = "1" ] && [ -n "$REGEN_CMD" ] && { echo "[regen] $REGEN_CMD"; eval "$REGEN_CMD"; }
if [ -n "$PREBUILD_CMD" ]; then echo "[prebuild] $PREBUILD_CMD"; RAN_PREBUILD=1; eval "$PREBUILD_CMD"; fi

echo "[1/4] configure"
nes_configure_build "$REPO" "$BUILD" "$CMAKE_TARGET" "${NESRECOMP_JOBS:-4}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$OSX_ARCHS" "${FLAGS[@]}"

BIN="$(find "$BUILD" -maxdepth 3 -type f -name "$CMAKE_TARGET" -perm -111 | head -1)"
[ -n "$BIN" ] || { echo "ERROR: no binary named '$CMAKE_TARGET' under $BUILD" >&2; exit 1; }
echo "      bin: $BIN"

echo "[3/4] bundle $APP_NAME.app"
mkdir -p "$OUT"
APPDIR="$OUT/$APP_NAME.app"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/Contents/MacOS" "$APPDIR/Contents/Resources" "$APPDIR/Contents/Frameworks"
# The real game binary lives next to a launcher that finds the ROM in the same
# folder as the .app and runs from there (so saves land beside the .app).
cp "$BIN" "$APPDIR/Contents/MacOS/$CMAKE_TARGET"
BUILT_DIR="$(dirname "$BIN")"
[ -d "$BUILT_DIR/assets" ] || { echo 'ERROR: launcher assets missing' >&2; exit 1; }
cp -R "$BUILT_DIR/assets" "$APPDIR/Contents/MacOS/assets"
if [ -d "$REPO/mods/preloaded/packages" ]; then
    mkdir -p "$APPDIR/Contents/MacOS/mods"
    cp -R "$REPO/mods/preloaded/packages" "$APPDIR/Contents/MacOS/mods/packages"
fi
if [ -d "$BUILT_DIR/mods/packages" ]; then
    mkdir -p "$APPDIR/Contents/MacOS/mods/packages"
    for package in "$BUILT_DIR"/mods/packages/nesrecomp.*; do
        [ -d "$package" ] && cp -R "$package" "$APPDIR/Contents/MacOS/mods/packages/"
    done
fi
if declare -F nes_platform_stage >/dev/null; then nes_platform_stage "$APPDIR/Contents/MacOS" "$BUILT_DIR"; fi
python3 "$NES_ENGINE/tools/release_support.py" --stage "$APPDIR/Contents/MacOS" --build "$BUILD" --project "$REPO" --licenses
cat > "$APPDIR/Contents/MacOS/$APP_NAME" <<EOF
#!/bin/sh
DIR="\$(cd "\$(dirname "\$0")" && pwd)"
# Folder the .app sits in (writable, user-facing).
APPDIR="\$(cd "\$DIR/../../.." && pwd)"
export SDL_JOYSTICK_HIDAPI_STEAM=1
cd "\$APPDIR" 2>/dev/null || true
ROM=""
for ext in $ROM_EXTS; do
    [ "\$ext" = "none" ] && break
    for f in "\$APPDIR"/*."\$ext"; do [ -e "\$f" ] && ROM="\$f" && break 2; done
done
if [ "\$#" -eq 0 ]; then
    [ -n "\$ROM" ] && exec "\$DIR/$CMAKE_TARGET" "\$ROM"
    exec "\$DIR/$CMAKE_TARGET" $EXTRA_ARGS
fi
exec "\$DIR/$CMAKE_TARGET" "\$@"
EOF
chmod +x "$APPDIR/Contents/MacOS/$APP_NAME"

cat > "$APPDIR/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleExecutable</key><string>$APP_NAME</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>0.1.0</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF

# Copy + relink the SDL2 dylib (and any other non-system deps) into the bundle.
if command -v dylibbundler >/dev/null 2>&1; then
  dylibbundler -od -b -x "$APPDIR/Contents/MacOS/$CMAKE_TARGET" \
      -d "$APPDIR/Contents/Frameworks" -p @executable_path/../Frameworks
else
  echo "      WARNING: dylibbundler not found â€” .app will need a system SDL2."
fi
# Ad-hoc codesign so Gatekeeper lets it run locally (no Developer ID required).
codesign --force --deep --sign - "$APPDIR" 2>/dev/null || \
  echo "      (codesign skipped â€” run 'xattr -dr com.apple.quarantine' if Gatekeeper blocks it)"
echo "      BUILT: $APPDIR"

if [ "$DO_DMG" = "1" ]; then
  echo "[4/4] package .dmg"
  DMG="$OUT/$APP_NAME-macos-$ARCH.dmg"
  rm -f "$DMG"
  if command -v create-dmg >/dev/null 2>&1; then
    create-dmg --volname "$APP_NAME" --app-drop-link 420 180 "$DMG" "$APPDIR" || \
      hdiutil create -volname "$APP_NAME" -srcfolder "$APPDIR" -ov -format UDZO "$DMG"
  else
    hdiutil create -volname "$APP_NAME" -srcfolder "$APPDIR" -ov -format UDZO "$DMG"
  fi
  echo "      BUILT: $DMG"
else
  echo "[4/4] (--no-dmg) skipped"
fi
