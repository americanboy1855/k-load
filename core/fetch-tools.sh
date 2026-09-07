#!/bin/zsh
# Скачивает инструменты, которых нет в git, и складывает их так, как их
# ждёт build.sh. Без этого шага сборка не запустится: yt-dlp, ffmpeg и
# ffprobe весят больше трёхсот мегабайт и в репозиторий не помещаются.
#
# Запуск: ./vendor/fetch-tools.sh
set -e
# Каноничное место инструментов в репозитории — core/tools.
HERE="${0:A:h}/tools"
mkdir -p "$HERE"
UA="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)"

# ---- yt-dlp: папочная сборка, та же, что качает само приложение ----
# Она стартует за доли секунды, в отличие от однофайловой, которая
# распаковывает себя при каждом запуске (~15 с).
if [ ! -x "$HERE/ytdlp/yt-dlp_macos" ]; then
  echo "yt-dlp: качаю…"
  TMP=$(mktemp -d)
  curl -L --fail -A "$UA" -o "$TMP/ytdlp.zip" \
    "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_macos.zip"
  mkdir -p "$HERE/ytdlp"
  unzip -o -q "$TMP/ytdlp.zip" -d "$HERE/ytdlp"
  rm -rf "$TMP"
  chmod 755 "$HERE/ytdlp/yt-dlp_macos"
  echo "yt-dlp: готово ($HERE/ytdlp)"
else
  echo "yt-dlp: уже на месте"
fi

# ---- ffmpeg и ffprobe: статические сборки с evermeet.cx ----
# Со статикой папка libs/ не нужна вовсе: всё зашито внутрь исполняемого
# файла. Если предпочитаете сборку с общими библиотеками (как раньше,
# через Homebrew) — положите бинарники в vendor/ вручную, а библиотеки
# в vendor/libs, сшив их через install_name_tool с @executable_path/libs/.
fetch_static() {
  local name="$1"
  if [ ! -x "$HERE/$name" ]; then
    echo "$name: качаю…"
    TMP=$(mktemp -d)
    curl -L --fail -A "$UA" -o "$TMP/$name.zip" \
      "https://evermeet.cx/ffmpeg/get/$name/zip"
    unzip -o -q "$TMP/$name.zip" -d "$HERE"
    rm -rf "$TMP"
    chmod 755 "$HERE/$name"
    echo "$name: готово ($HERE/$name)"
  else
    echo "$name: уже на месте"
  fi
}
fetch_static ffmpeg
fetch_static ffprobe

echo
echo "Инструменты на месте. Складываю в Application Support (там их ищет приложение)…"
TOOLS="$HOME/Library/Application Support/K LOAD/tools"
mkdir -p "$TOOLS/ytdlp"
cp -R "$HERE/ytdlp/" "$TOOLS/ytdlp/"
cp "$HERE/ffmpeg" "$HERE/ffprobe" "$TOOLS/"
chown -R "$USER" "$TOOLS"
chmod -R u+rwX,go+rX "$TOOLS"
echo "Готово: $TOOLS"
echo "Дальше: см. README — сборка и установка (juce/install-mac.sh)"
