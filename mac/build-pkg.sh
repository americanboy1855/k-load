#!/bin/bash
# Сборка дистрибутивного установщика K LOAD (.pkg) — macOS. Версия — из pubspec.
# Запуск: mac/build-pkg.sh  (скрипт сам встаёт в корень репо)
# Требует: собранный Release-бандл (cd app && flutter build macos --release —
# падение codesign в дереве с iCloud не мешает: подпись делается здесь),
# Release-ядро kdcore/build-release/libkdcore.dylib, инструменты в core/tools.

set -e
cd "$(dirname "$0")/.."

# Версия — из pubspec (одно место истины): 1.1.0+2 → 1.1.0.
VERSION=$(grep '^version:' app/pubspec.yaml | sed 's/version: *//; s/+.*//')

APP="app/build/macos/Build/Products/Release/K LOAD.app"
[ -d "$APP" ] || { echo "нет $APP — сначала: cd app && flutter build macos --release"; exit 1; }

STAGE="/tmp/kload-pkg-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/K LOAD.app"
cp kdcore/build-release/libkdcore.dylib "$STAGE/K LOAD.app/Contents/MacOS/"

T="$STAGE/K LOAD.app/Contents/Resources/tools"
mkdir -p "$T"
cp -R core/tools/ytdlp "$T/ytdlp"
cp core/tools/ffmpeg "$T/ffmpeg"
cp core/tools/ffprobe "$T/ffprobe"
# deno: universal из core/tools; App Support — запаска (может быть arm64-only)
if [ -f core/tools/deno ]; then
  cp core/tools/deno "$T/deno"
elif [ -f "$HOME/Library/Application Support/K LOAD/tools/deno" ]; then
  cp "$HOME/Library/Application Support/K LOAD/tools/deno" "$T/deno"
fi
[ -d mac/pkg/LICENSES ] && cp -R mac/pkg/LICENSES "$T/LICENSES"

xattr -cr "$STAGE/K LOAD.app"
codesign --force --deep --sign - "$STAGE/K LOAD.app"
codesign --verify --deep --strict "$STAGE/K LOAD.app"

# Gates Intel и Apple Silicon: каждый бинарник обязан быть universal
for BIN in "$STAGE/K LOAD.app/Contents/MacOS/K LOAD" \
           "$STAGE/K LOAD.app/Contents/MacOS/libkdcore.dylib" \
           "$T/ytdlp/yt-dlp_macos" "$T/ffmpeg" "$T/ffprobe" "$T/deno"; do
  INFO=$(lipo -info "$BIN" 2>&1)
  echo "$INFO" | grep -q x86_64 && echo "$INFO" | grep -q arm64 || \
    { echo "НЕ UNIVERSAL: $BIN — $INFO"; exit 1; }
done
echo "Все бинарники universal (x86_64 + arm64)"

mkdir -p mac/dist
pkgbuild --root "$STAGE" --identifier ru.kvartal.kload --version "$VERSION" \
  --install-location /Applications --scripts mac/pkg/scripts \
  mac/dist/kload-component.pkg

productbuild --distribution mac/pkg/Distribution.xml \
  --package-path mac/dist --resource mac/pkg \
  "mac/dist/K-LOAD-V$VERSION-setup.pkg"

echo "Готово: mac/dist/K-LOAD-V$VERSION-setup.pkg"
