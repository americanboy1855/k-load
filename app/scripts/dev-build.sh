#!/bin/zsh
# Дев-сборка K LOAD.
#
# Проект лежит на iCloud-синхронизируемом рабочем столе: провайдер мгновенно
# вешает на свежие файлы com.apple.fileprovider/FinderInfo, и codesign
# отвергает бандл («…detritus not allowed»). Поэтому собранный бандл
# копируется в несинхронизируемую папку (~/Library/Application Support/
# K LOAD dev), там чистится, подписывается ад-хоком и запускается.
#
# Запуск: ./scripts/dev-build.sh
set -e
cd "$(dirname "$0")/.."

flutter build macos --debug || true

SRC="build/macos/Build/Products/Debug/K LOAD.app"
if [ ! -d "$SRC" ]; then
  echo "бандл не собрался: $SRC" >&2
  exit 1
fi

DST="$HOME/Library/Application Support/K LOAD dev/K LOAD.app"
rm -rf "$DST"
mkdir -p "$(dirname "$DST")"
cp -R "$SRC" "$DST"

# Ядро рядом с бинарем (разработка); в релизе ляжет в Frameworks.
if [ -f "../kdcore/build/libkdcore.dylib" ]; then
  cp ../kdcore/build/libkdcore.dylib "$DST/Contents/MacOS/"
fi

# Чистка провайдерских атрибутов и ад-хок подпись вне зоны синхронизации.
find "$DST" -exec xattr -d com.apple.FinderInfo {} \; 2>/dev/null || true
find "$DST" -exec xattr -d com.apple.fileprovider.fpfs#P {} \; 2>/dev/null || true
xattr -cr "$DST"
codesign --force --deep --sign - "$DST" 2>/dev/null
codesign --verify --deep --strict "$DST"
echo "Готово: $DST"
echo "Запуск: open '$DST'"
