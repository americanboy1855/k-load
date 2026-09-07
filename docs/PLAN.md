# K LOAD — план разработки

Нативный загрузчик медиа. Этап 2 (Flutter macOS) → затем Этап 3 (VST3/AU на JUCE).
Полный контекст переноса: `/tmp/HANDOFF-K-LOAD.md` (если очищен — этот файл + `core/` достаточны).

## Зафиксировано
- Имя: **K LOAD**. Папка загрузок: **K DWNLD** внутри выбранной пользователем папки.
- macOS первым; Windows позже. Единое C++ ядро без JUCE (`libkdcore` + C-API): Flutter через FFI, будущий VST — напрямую.
- Дизайн: новый визуальный мир с нуля. Лого kvartal (`~/.zcode/skills/webcore/assets/brand/kvartal-logo.svg`) + кнопка Boosty (boosty.to/kvartalrecords/donate).

## Функционал v1
Источники: YouTube, Instagram Reels, TikTok, Яндекс Музыка, ВК, Spotify, Apple Music, SoundCloud, Pinterest.
- Поиск по названию → видео или музыка на выбор; только реально доступные форматы/качества («Макс. качество»).
- Пачки ссылок любых сервисов; плейлист YouTube целиком.
- Drag-out скачанного файла из плашки очереди.
- Превью, если сервис отдаёт; бейдж сервиса-источника.
- Плашка VPN: «Для лучшей работы загрузчика - включите VPN».

## Этапы
1. `kdcore/` — порт core/engine на чистый C++17 (без JUCE), C-API kd_*.h, контрактные тесты, universal dylib. git init заново.
2. `app/` — Flutter macOS: FFI (ffigen), дизайн-мир (2–3 мокапа на выбор → фиксация системы), drag-out (platform channel), бандл инструментов.
3. (после готовности приложения) `plugin/` — JUCE 9 VST3/AU, общий кэш инструментов, auval/pluginval.
4. Подпись/нотаризация, .pkg, CI.

## Риски
yt-dlp меняется → контрактные тесты; ffmpeg static = GPL (решить на дистрибуции); Gatekeeper без подписи.
