# K LOAD cli — раздача для второй машины

Консольная версия скачивателя: те же правила, что в приложении
(YouTube, SoundCloud, TikTok, Instagram, Pinterest; Spotify — через
поиск по названию трека; плейлисты YouTube целиком; пачки ссылок).

## Как забрать на второй машине (та же Wi-Fi/сеть)

1. Узнай адрес этого Mac — он показан в терминале сессии
   (обычно `http://192.168.x.x:8790`).
2. Открой в браузере: `http://<IP>:8790/` и скачай `kload-cli.sh`,
   или сразу:
   ```sh
   curl -fsSL "http://<IP>:8790/kload-cli.sh" -o kload-cli.sh && chmod +x kload-cli.sh
   ```
3. Примеры:
   ```sh
   ./kload-cli.sh "https://www.youtube.com/watch?v=jNQXAC9IVRw"   # видео
   ./kload-cli.sh -a "https://soundcloud.com/forss/flickermood"   # mp3
   ./kload-cli.sh -q 720 "ссылка1" "ссылка2"                      # пачка 720p
   ./kload-cli.sh "a-ha take on me"                               # по названию*
   ```
   \* поиск по названию: вставь название как ссылку не получится —
   используй приложение, либо `yt-dlp "ytsearch1:a-ha take on me"`.

## Требования

- macOS; файлы складываются в `~/Downloads/K DWNLD`.
- yt-dlp скачается сам при первом запуске.
- Для музыки нужен ffmpeg: `brew install ffmpeg`.

## Ограничения

- Spotify: треки качаются «через название» (поиск YouTube); альбомы —
  не поддержаны в cli (в приложении — да).
- Выбор отрезка видео (хрон) в cli пока недоступен — только в приложении.
