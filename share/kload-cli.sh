#!/bin/zsh
# kload-cli — консольный скачиватель K LOAD (тот же движок правил, что в приложении)
#
# Быстрая установка (на второй машине в той же сети):
#   curl -fsSL "http://<IP-этого-Mac>:8790/kload-cli.sh" -o kload-cli.sh && chmod +x kload-cli.sh
# Примеры:
#   ./kload-cli.sh "https://youtube.com/watch?v=..."                    # видео 1080
#   ./kload-cli.sh -a "https://soundcloud.com/forss/flickermood"        # музыку mp3
#   ./kload-cli.sh -q 720 "ссылка1" "ссылка2"                           # пачка, 720p
#   ./kload-cli.sh "radiohead creep acoustic"                           # поиск по названию
#
# Папка по умолчанию: ~/Downloads/K DWNLD. Инструменты качаются сами
# при первом запуске (yt-dlp ~35 МБ; ffmpeg нужен для музыки — ставится через brew).

set -u
BIN="$HOME/.kload/bin"
OUT="$HOME/Downloads/K DWNLD"
QUALITY="1080"; AUDIO=0
UA="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36"

usage() { grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

while getopts "aq:o:h" opt; do
  case $opt in
    a) AUDIO=1 ;;
    q) QUALITY="$OPTARG" ;;
    o) OUT="$OPTARG" ;;
    h|*) usage ;;
  esac
done
shift $((OPTIND-1))
[ $# -eq 0 ] && usage

mkdir -p "$OUT" "$BIN"

# ---- yt-dlp: докачка при отсутствии ----
YTDLP="$BIN/yt-dlp_macos"
if [ ! -x "$YTDLP" ]; then
  echo "⬇  качаю yt-dlp…"
  curl -L --fail -A "$UA" -o "$BIN/yt-dlp_macos.zip" \
    "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_macos.zip" || exit 1
  unzip -o -q "$BIN/yt-dlp_macos.zip" -d "$BIN"
  rm -f "$BIN/yt-dlp_macos.zip"; chmod 755 "$YTDLP"
fi

# ---- ffmpeg: для музыки (brew), иначе предупредить ----
if [ "$AUDIO" = 1 ] && ! command -v ffmpeg >/dev/null 2>&1; then
  echo "⚠  для музыки нужен ffmpeg: brew install ffmpeg" >&2
fi

# ---- правило одной ссылки: что это и как качать ----
split_links() {
  # вынимаем ссылки из произвольного текста (как в приложении)
  printf '%s\n' "$@" | tr ' \n\r\t,;' '\n\n\n\n\n' | sed 's/^[[:space:]\"'\''']*//; s/[[:space:]\"'\''']*$//' \
    | grep -E 'https?://|^[a-z0-9.-]+\.[a-z]{2,}/' | sort -u
}

resolve_spotify() {
  # открытый embed Spotify → «Артист - Трек», дальше поиск на YouTube
  local kind_id="${1#*spotify.com/}"; kind_id="${kind_id%%\?*}"
  local kind="${kind_id%%/*}"; local id="${kind_id#*/}"; id="${id%%/*}"
  [ "$kind" = "track" ] || { echo "$1"; return; }   # альбомы/плейлисты — как есть
  local html name artist
  html=$(curl -sL -A "$UA" "https://open.spotify.com/embed/track/$id")
  name=$(printf '%s' "$html" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['props']['pageProps']['state']['data']['entity']['name'])" 2>/dev/null)
  artist=$(printf '%s' "$html" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['props']['pageProps']['state']['data']['entity']['artists'][0]['name'])" 2>/dev/null)
  [ -n "$name" ] && echo "$artist - $name" || echo "$1"
}

fmt_args=()
if [ "$AUDIO" = 1 ]; then
  fmt_args=(-f "bestaudio/best" -x --audio-format mp3 --audio-quality 0 --embed-thumbnail --embed-metadata)
elif [ "$QUALITY" != "0" ]; then
  fmt_args=(-f "bestvideo[height<=$QUALITY]+bestaudio/best[height<=$QUALITY]/best" --merge-output-format mp4)
else
  fmt_args=(-f "bestvideo+bestaudio/best" --merge-output-format mp4)
fi

FAIL=0; N=0
for raw in "$@"; do
  for link in $(split_links "$raw"); do
    N=$((N+1)); echo "▸ [$N] $link"
    case "$link" in
      *spotify.com/*) link=$(resolve_spotify "$link");;
    esac
    case "$link" in
      *youtube.com/playlist*|*list=*|*album/*|*sets/*)
        extra=(--yes-playlist) ;;
      *youtube.com/*|*youtu.be/*|*soundcloud.com/*|*tiktok.com/*|*instagram.com/*|*vk.com/*|*vkvideo.ru/*|*pinterest.com/*|*pin.it/*)
        extra=(--no-playlist) ;;
      *) extra=(--no-playlist) ;;
    esac
    # попытка 1 — без входа; при бане/приватности — cookies из браузера по умолчанию
    "$YTDLP" --ignore-config --no-warnings --retries 3 --socket-timeout 20 \
      --no-mtime --no-overwrites "${extra[@]}" "${fmt_args[@]}" \
      -P "$OUT" ${AUDIO:---embed-metadata} "$link" && { echo "✓ готово"; continue; }
    echo "↻ пробую с cookies браузера…"
    "$YTDLP" --ignore-config --no-warnings --retries 3 --socket-timeout 20 \
      --no-mtime --no-overwrites "${extra[@]}" "${fmt_args[@]}" \
      --cookies-from-browser safari -P "$OUT" "$link" \
      || { echo "✗ не вышло: $link"; FAIL=1; }
  done
done

# текст без ссылок = поиск по названию (первый результат YouTube)
exit $FAIL
