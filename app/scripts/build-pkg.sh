#!/bin/bash
# Сборка дистрибутивного установщика K LOAD V1.0 (.pkg).
# Использование: ./scripts/build-pkg.sh  (из папки app/)
# Требует: собранный Release-бандл (flutter build macos --release —
# падение codesign в дереве с iCloud не мешает: подпись делается здесь),
# Release-ядро kdcore/build-release/libkdcore.dylib, инструменты в
# core/tools (+ universal ffmpeg/ffprobe из evermeet/osxexperts).

set -e
cd "$(dirname "$0")/.."

APP="build/macos/Build/Products/Release/K LOAD.app"
[ -d "$APP" ] || { echo "нет $APP — сначала flutter build macos --release"; exit 1; }

STAGE="/tmp/kload-pkg-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/K LOAD.app"
cp ../kdcore/build-release/libkdcore.dylib "$STAGE/K LOAD.app/Contents/MacOS/"

T="$STAGE/K LOAD.app/Contents/Resources/tools"
mkdir -p "$T"
cp -R ../core/tools/ytdlp "$T/ytdlp"
cp ../core/tools/ffmpeg "$T/ffmpeg"
cp ../core/tools/ffprobe "$T/ffprobe"
[ -f "$HOME/Library/Application Support/K LOAD/tools/deno" ] && \
  cp "$HOME/Library/Application Support/K LOAD/tools/deno" "$T/deno"
[ -d "scripts/pkg/LICENSES" ] && cp -R scripts/pkg/LICENSES "$T/LICENSES"

xattr -cr "$STAGE/K LOAD.app"
codesign --force --deep --sign - "$STAGE/K LOAD.app"
codesign --verify --deep --strict "$STAGE/K LOAD.app"

pkgbuild --root "$STAGE" --identifier ru.kvartal.kload --version 1.0.0 \
  --install-location /Applications --scripts scripts/pkg/scripts \
  kload-component.pkg

productbuild --distribution scripts/pkg/Distribution.xml \
  --package-path . --resource scripts/pkg \
  "K-LOAD-V1.0-setup.pkg"

echo "Готово: K-LOAD-V1.0-setup.pkg"
