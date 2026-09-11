# K LOAD — полный слепок архитектуры для аудита

Дата: 11.09.2026 · HEAD `040e772` · Компилятор фактов: полный обход кода
(app/lib — все файлы целиком, kdcore/src — все файлы целиком, app/macos,
скрипты, тесты). Назначение — база для подробного аудита: где что лежит,
кто за что отвечает, по каким линиям идут данные и где тонкие места.

---

## 1. Обзор системы

**K LOAD** — macOS-приложение-загрузчик медиа (видео/аудио/фото) с
YouTube, TikTok, Instagram, Pinterest, VK, Spotify, Apple Music,
SoundCloud. Пиксельно-телевизионный UI на Flutter, ядро логики на C++17,
сам загрузчик — сторонний **yt-dlp** (+ **ffmpeg** для склейки/конвертации).

```
┌──────────────────────────── macOS ────────────────────────────┐
│ K LOAD.app                                                    │
│  ┌──────────────── Flutter (Dart, AOT) ──────────────┐        │
│  │ main.dart → KloadApp → KLoadScreen (ui/screen)    │        │
│  │ ui/theme (палитра/шрифты), ui/widgets (LED/CRT)   │        │
│  └───────────────┬───────────────────────────────────┘        │
│                  │ dart:ffi (JSON-строки)  ▲ Dart_Port (события)
│  ┌───────────────▼───────────────────────────────────┐        │
│  │ libkdcore.dylib (C++17)                           │        │
│  │  CAPI (kd_*) → Engine (очередь, воркеры)          │        │
│  │  Detector (сервисы) · VpnMonitor · DestResolver   │        │
│  └───────┬──────────────┬───────────────┬────────────┘        │
│          │ exec         │ libcurl       │ route -n get default │
│  ┌───────▼───┐   ┌──────▼─────┐   ┌─────▼──────┐               │
│  │ yt-dlp    │   │ открытые   │   │ VpnMonitor │               │
│  │ ffmpeg    │   │ API/страницы│  └────────────┘               │
│  └───────────┘   └────────────┘                                │
│  MainFlutterWindow.swift: окно 560×670, NSOpenPanel, drag-out  │
└────────────────────────────────────────────────────────────────┘
```

Две линии связи Flutter ↔ ядро:
- **Команды (вниз)**: `dart:ffi` прямые вызовы C-функций `kd_*`; данные —
  UTF-8 JSON-строки.
- **События (вверх)**: `Dart_PostCObject` на `RawReceivePort`; каждое
  событие — JSON-строка `{"type": "queue_changed" | "probe" | "vpn", …}`.

---

## 2. Дерево проекта

```
K DOWNLOADER MAC/
├── app/                          # Flutter-приложение
│   ├── lib/
│   │   ├── main.dart             # точка входа, MaterialApp, тема скроллбара
│   │   ├── kd/kd_bindings.dart   # FFI-обвязка ядра (единственный мост)
│   │   └── ui/
│   │       ├── theme.dart        # палитра Pal, типографика T, DashedBorder
│   │       ├── widgets.dart      # LED-полосы, CRT-атмосфера, иконки, Plate
│   │       └── screen.dart       # ВЕСЬ экран и вся UI-логика (~3800 строк)
│   ├── macos/Runner/
│   │   ├── MainFlutterWindow.swift # окно 560×670, канал kload/native, drag-out
│   │   └── AppDelegate.swift       # штатная заглушка
│   ├── assets/fonts/             # PS2P, Departure Mono, IBM Plex, Handjet, Chakra
│   ├── assets/kvartal.svg        # логотип
│   ├── scripts/dev-build.sh      # дев-сборка → ~/Library/Application Support
│   ├── scripts/gen_icon2.swift   # генератор app_icon_*.png
│   └── test/                     # measure_fit_test, bracket_metrics_test
├── kdcore/                       # C++17-ядро
│   ├── CMakeLists.txt            # SHARED libkdcore (universal arm64;x86_64)
│   ├── include/kd_capi.h         # публичный C-контракт (документирован)
│   ├── src/
│   │   ├── Engine.h/.cpp         # ~3000 строк: вся логика скачивания
│   │   ├── Detector.h            # классификатор ссылок/сервисов
│   │   ├── CAPI.cpp              # мост kd_* ↔ Engine
│   │   ├── VpnMonitor.h/.cpp     # поток VPN (route -n get default)
│   │   ├── DestResolver.h        # папка по умолчанию ~/Downloads/K LOAD
│   │   ├── kd_compat.h           # строковые/файловые утилиты (замена juce)
│   │   ├── kd_process.h          # ChildProcess (fork/exec, poll, SIGTERM)
│   │   ├── kd_thread.h           # Wake — считающий семафор
│   │   ├── kd_http.h             # GET через libcurl (текст/файл)
│   │   ├── kd_url.h              # parseUrl/queryParam
│   │   └── platform/BrowserDefault.h/.mm # цепочка браузеров для cookies
│   ├── tests/kd_tests.cpp        # контрактные (+ --live) тесты, 58 ok
│   ├── third_party/dart/         # dart_api_dl.c (Dart_PostCObject из C++)
│   └── build/                    # libkdcore.dylib, kd_tests
├── core/tools/ytdlp/yt-dlp_macos # бандл-инструменты (репозиторий, dev)
├── docs/                         # PLAN.md, PKG-PLAN.md, HANDOFF, этот файл
└── share/kload-cli.sh            # вспомогательный (8790), не трогать
```

**Инструменты в рантайме** ищутся `Engine::findToolsDir()` по цепочке:
`K_LOAD_TOOLS` (env) → `K_DOWNLOADER_TOOLS` → `~/Library/Application
Support/K LOAD/tools` → `/Library/Application Support/K LOAD/tools` →
`Resources` бандла → `tools/` рядом → установленный K LOAD.app → дерево
репозитория (`core/tools`). Бинарник: `yt-dlp_macos`; рядом обязан быть
`ffmpeg` (+`ffprobe`).

---

## 3. Потоки данных (ключевые сценарии)

### 3.1 Включение
`main()` → `KloadApp` → `KLoadScreen.initState` → `_boot()`:
`KdCore.start()` (dlopen libkdcore, kd_engine_create, порт событий) →
snapshot → `toolsStatus` → `vpnState` → `defaultDest`. Параллельно
boot-анимация (луч кинескопа 1.5 с), reduced-motion — мгновенно.

### 3.2 Вставка ссылки / поиск
ввод → `onChanged` → debounce 600 мс → `_startSeek()`:
`core.splitLinks(текст)`; 1 ссылка → `probeAsync`; >1 → `isBatch`, разборы
по очереди (`_probeNextBatchLink`, каждое событие складывается в
`batchProbes[i]`). Событие `probe` → `_onProbe`: карточка (титул, обложка,
высоты, плейлист/фото/DRM-флаги) или список `results` для текстового
поиска. Сторожевой таймер 25 с → честная ошибка; дозагрузка длительности
через 5 с (`_scheduleDurationRetry`).

### 3.3 СКАЧАТЬ и проверка «уже скачано»
`_download({_GoAction})` (go / overwrite «ещё раз» / skipExisting
«пропустить»):
1. Пачка блокируется, пока разборы не завершены
   (`batchProbing || batchProcessed < batchLinks.length` → return).
2. Строится **план**: для каждой ссылки (не busyVariant) — заявка
   `predictFiles`: `template` (одиночка, имя из заголовка источника),
   `literal` (имя от приложения: поиск по названию, ролики плейлиста,
   фото), `flat` (плейлист внутри пачки: папка + `NN - имя`).
3. `kd_predict_files` в ядре строит имя по тем же правилам, что и сам
   yt-dlp (зеркало sanitize_filename + chronSuffix + safeName) и проверяет
   `fs::exists(dir/name)`.
4. `existing > 0` и action==go → модалка `_KModal`:
   - одиночка: «ЭТОТ ФАЙЛ УЖЕ СКАЧАН» (ОТКРЫТЬ В ПАПКЕ / СКАЧАТЬ ЕЩЁ РАЗ
     / ОТМЕНА);
   - пачка: «УЖЕ СКАЧАНО: N ИЗ M» (ПРОПУСТИТЬ СКАЧАННЫЕ / СКАЧАТЬ ВСЁ
     ЕЩЁ РАЗ / ОТМЕНА), N и M — по ссылкам пачки.
5. enqueue: `enqueueBatch(links, …, forceOverwrite: action==overwrite)` /
   `enqueuePhoto`. «Пропустить»: полные дубли отбрасываются, частичный
   плейлист остаётся — ядро скипнет существующие ролики.

### 3.4 Жизнь задания в ядре
воркер (2 шт.) → `processItem`: photo → `startPhotoFallback`;
needsResolve (Spotify/Apple/VK Music) → `startResolve` → `downloadTrack`;
иначе `startNative`. Внутри: `baseArgs` (--no-mtime --no-overwrites
--print @T/@F …) + ветки форматов; `runYtDlp` — ChildProcess, построчный
`consume` (@T — титул/прогресс «Качаю i из N», @F — файл готов,
`[download] %` — прогресс, «already downloaded» → skipped), stall-защита
(90 с без байтов → 2 повтора), пауза (SIGTERM → .part сохраняется),
финиш-проверки (хрон длиннее 60 %, аудио с длительностью), Pinterest-фото
→ oEmbed + originals + sips.

### 3.5 Пауза и VPN
`setPaused(true)`: текущий процесс глушится SIGTERM (yt-dlp закрывает
.part), задание → paused, очередь не подаётся (воркеры спят по 200 мс).
`setPaused(false)`: paused → queued, докачка с позиции.
`VpnMonitor` (поток, раз в 1 с): `route -n get default` → интерфейс
utun/tun/tap/ppp/ipsec/wg → on/off; debounce 2 проверки. on→off:
автопауза активных + плашка «VPN ОТКЛЮЧЁН…»; off→on: `retryNetworkFailed`
(до 2 автоповторов сетевых сбоев).

### 3.6 Прочие линии
- **Drag-out**: UI шлёт `setZones` (прямоугольники готовых строк, дизайн-
  координаты) → нативные прозрачные NSView → mouseDown начинает
  NSDraggingSession с файлом (copy).
- **Папка назначения**: круглая кнопка → `chooseFolder` → NSOpenPanel.
- **Превью**: `KdCore.thumbPathAsync` (Isolate) → `kd_thumb_path` → дисковый
  кэш `~/Library/Application Support/K LOAD/cache/thumbs`.

---

## 4. Flutter-слой — карта файлов и функций

### 4.1 `lib/main.dart`
| Сущность | Роль |
|---|---|
| `main()` | runApp |
| `KloadApp` | MaterialApp, dark, M3; кастомный `scrollbarTheme` (4px, amber-ползунок, ledOff-дорожка); `home: KLoadScreen` |

### 4.2 `lib/ui/theme.dart`
| Сущность | Роль |
|---|---|
| `Pal.*` | палитра: desk, plastic, plasticEdge, amber #FFB000, soft, dim, white, error #FF5F57, ledOff, amberSoft/Faint/Ghost, screenBg |
| `T.ps/mono/h` | типографика: Press Start 2P (акценты, height 1.45), Departure Mono (текст, 1.25), Handjet (лого) |
| `DashedBorderPainter` | пунктирная/цельная рамка (dash 5, gap 4, radius) |
| `DashedBox` | контейнер с пунктиром (+опц. tap/alignment) |

### 4.3 `lib/ui/widgets.dart`
| Сущность | Роль |
|---|---|
| `plateIconIdle` | цвет «вдавленной» иконки на корпусе |
| `PlateHover` | InheritedWidget: ховер-состояние круглой плашки для иконок |
| `ChainIcon` | иконка скрепки (стрелка в поле), тёмная/амбер на ховере |
| `FolderIcon` | иконка папки (плашка + строки очереди) |
| `TrashIcon` | иконка корзины |
| `VpnIcon` | иконка VPN-плашки |
| `SweepLedRow` | бегущая туда-обратно LED-полоса (ИЩУ…), пиксельная геометрия |
| `LedRow` | статичная полоса (шкала поиска с процентами) |
| `AnimatedLedRow` | полоса очереди: догоняет progress за 900 мс, ведущий сегмент мигает |
| `GlassBackdrop`/`_BackdropPainter` | фон кинескопа: сканлайны, виньетка, блик, «выпуклость» |
| `GlassVeil`/`_VeilPainter` | вуаль НАД контентом: шум (240 точек/170 мс), редкое мерцание; reduced-motion — статична |
| `TrackingBar` | полоса трекинга, изредка пробегает (3.2 с раз в 18–30 с) |
| `BootBeamPainter` | луч включения: чёрный экран → точка → расширение → затухание (1.5 с) |
| `Plate` | круглая кнопка корпуса (папка/$): вдавливание, свечение на ховере |
| `_FillPainter`, `_StrokePainter` | painters иконок (+свечение blur) |

### 4.4 `lib/kd/kd_bindings.dart` — FFI-мост
**Типы**: `_AttachDart, _Create, _VoidEngine, _SetPort, _StringEngine,
_StringString, _Enqueue(2 строки), _Id(int), _IntEngine, _VoidEngineString,
_VoidPtr, _CString, _StringEngineString, _ProbeSource(3 строки),
_ThumbPath, _Predict`.

**KdBindings** (singleton, `open()` ищет dylib: `K_LOAD_DYLIB` → рядом с
бинарем → 9 уровней вверх/`kdcore/build` → cwd): лукапы всех символов;
`attachDart()`, `version()`, `_take()` (строка → Dart + free).

**События**: `KdEvent` (sealed) → `KdQueueChanged` | `KdVpnEvent(on)` |
`KdProbeEvent(json)` — поля-геттеры: ok, title, serviceTitle, error, link,
resolved, thumbnail, uploader, duration, count, isPlaylist, hasPlaylist,
isPhoto, isSearch, heights, drm, shortVideo, entries[{url,title}], service.

**KdItem** — снимок задания: id, title, link, stage, state, serviceTitle,
progress, isAudio, isPhoto, audioFormat, container, sections, maxHeight,
itemIndex/Total, batchIndex/Total, files, dest.

**KdCore** (singleton): RawReceivePort → Stream<KdEvent>.
Методы: `snapshot()`, `splitLinks(text)`, `enqueueBatch(links, {dest,
audio, quality, audioFormat, wholePlaylist, nameOverride, sections,
playlistLimit, container, imageFormat, durationHint, forceOverwrite})`,
`enqueuePhoto(link, {dest, imageFormat})`, `probeAsync(text, {source})`,
`probeBlocking(text)`, `predictFiles(files)→[{i,dir,name,path,exists}]`,
`cancel(id)`, `remove(id)`, `clearFinished()`, `setPaused/​isPaused`,
`retryNetworkFailed()`, `vpnState()`, `defaultDest()`, `toolsStatus()`,
`thumbPath/​thumbPathAsync` (статические, для Isolate), `address`.

**serviceLabel(int)**: 0 YOUTUBE … 8 ВК МУЗЫКА, 9 SOUNDCLOUD, иначе САЙТ.

### 4.5 `lib/ui/screen.dart` — экран и вся UI-логика

**Константы**: `tvW=560, tvH=670` (канвас-телевизор, cover-масштаб в окно).

**Каталог ошибок** (гл. 9.2) + `userMessageText(m)` → `«TITLE — [hint]»`.

**Модели**: `_DupeNotice{batch, existing, total, openFolder}`,
`_GoAction{go, overwrite, skipExisting}`.

**Состояние `_KLoadScreenState`** (группы):
- ядро/события: `core`, `sub`
- телевизор: `booted, beamStarted, beamC, trackC, tracking, trackTimer`
- VPN: `vpnState(0/1/2), vpnDismissed, vpnEpoch, vpnPlateShown`,
  `vpnPlateVisible`
- поиск: `query, searchFocus, searchFocused/Hover, phase(idle/seeking/
  found), debounce, seekAnim, seekPct`
- карточка: `probe, isBatch, batchCount, batchLinks, mediaMode, musicOnly,
  mode(video/music), chronOn, chronOpen, chronLocked, chronFrom/To`
- результаты/пачка: `searchResults, selectedResultUrl, showResultList,
  resultError, durationRetried, searchingNow, _probeWatchdog,
  batchProbing, batchIdx, batchProcessed, batchDuration, batchProbes[]`
- диспетчер: `items, queuePaused, destFolder, toolsFound, queueShown,
  clearHover, dupeNotice, _dupeClosing, _rowKeys, _canvasKey,
  _lastZonesKey, _queueScroll, _resultsScroll, _queueKey`
- форматы: `quality(1080), qualityOpen, videoContainer(mp4),
  audioFormat(mp3), imageFormat(jpg), playlistLimit, countCtrl`
- панели: `openPanel(''|chron|count|quality|vformat|aformat), chipKeys,
  panelX/_chronX, _uiColumnKey`
- native: `_native = MethodChannel('kload/native')`
- тост: `toastText, toastTimer`

**Методы** (по группам):

*Жизненный цикл/события*: `initState` (boot+анимации), `dispose`,
`_boot`, `_onEvent` (vpn→автопауза/recover; probe→пачка/карточка; иначе
refresh), `_refreshItems` (якорь прокрутки), `_queueAnchorId/
_restoreQueueAnchor`, `_recoverAfterVpn`, `_scheduleDragZones`,
`_scheduleTracking`, `_finishBoot`.

*Поиск/разбор*: `_onInputChanged`, `_startSeek`, `_armProbeWatchdog`,
`seekingFailed`, `_probeNextBatchLink`, `_animateSeek`, `batchBadge`,
`_onProbe`, `_scheduleDurationRetry`, `_selectResult` (мгновенная карточка
из строки + догрузка), `rawText`, `vpnVisible`, `cardVisible`,
`resultsOpen`, `searchFailed`, `_looksLikeLink`, `_showBackToResults`.

*Скачивание и дубли*: `_forcedAudio(service)` (1,6,7,8,9 — всегда звук),
`_audioSuffix(fmt)`, `_download({action})` — см. §3.3, `_variantSig`,
`_itemSig`, `_contentKey` (канонический ID контента), `sectionsArg`.

*Очередь*: `_openPath`, `_chooseDestFolder`, `_showToast`, `_trashRow`
(файл с диска + строка), `_queue`, `_compactQueue` (2 строки под выдачей),
`_queueHeader` ([ОЧИСТИТЬ] справа, baseline, AnimatedOpacity, сдвиг 3.1),
`_sortedItems` (working → queued/paused → failed → done), `hasActiveTasks`,
`_pauseButton`, `_toggleQueuePause`, `_clearQueueHistory`
(setPaused(false)+clearFinished+queuePaused=false).

*Строки/карточки*: `_queueRow` (титул @T→probe→файл, статус или ошибка
единым форматом, LED, кнопки ×/↻/папка/корзина), `_foundCard` (пачка /
ошибка / DRM / обычная), `_mediaOptions` (чипы форматов/качества),
`_audioLabel`, `_dropChip`, `_optionsPanel`, `_drmTitle`, `_sourceUrl`,
`_batchDurLine`, `_durLine`, `fmtLongDur`, `_badge` (+[ССЫЛКА]),
`safeFileName`, `_canonicalShort`, `_errorActions`, `_errorActionButton`,
`_runErrorAction` (retry/openLink/close/selectVideo/selectMusic/
changeChron/changeFormat).

*Режимы/панели*: `_modesRow` (ВИДЕО/МЕДИА, МУЗЫКА, ХРОН|КОЛ-ВО, пауза,
СКАЧАТЬ · N; canDownload), `_panelBelow`, `_measurePanel`, `_countBox`,
`_chronBox`, `_darkPanel`.

*Поиск/результаты*: `_seekBlock` (ИЩУ…/%), `_resultRow`, `_searchErrorPanel`,
`_resultsPanel`, `_panelClose`.

*Модалки*: `_dupeClosing/_closeDupeNotice(after)`, `_forceDownloadDupe`,
`_dupePlate` (одиночка/пачка — см. §3.3).

*VPN/тост/boot*: `_vpnPlate` (GlitchIn, блюр 2.5, крестик), `_toast`,
`_bootOverlay`, `_tv/_glass/_ui/_band` (каркас: папка, логотип kvartal,
$, grip), `_GripPainter`.

*Виджеты экрана*: `_KvartalLogo` (3 слоя SVG + фосфор), `_ModeChip`,
`_GhostButton` (пунктир, одна строка — «ВЕРНУТЬСЯ К РЕЗУЛЬТАТАМ»),
`_TextLink` («[ОЧИСТИТЬ]»/«[ССЫЛКА]»: подсветка только текста + shadow),
`_GoButton` (СКАЧАТЬ, glow/press), `_KModal` (затемнение+blur+Esc+фон),
`_ModalButton` (равные кнопки), `GlitchIn`/`_GlitchLinesPainter`,
`_SmartCover` (плазма→фото), `_CoverImage`/`_CoverPainter` (кроп полос
hqdefault, cover), `_Plasma`/`_PlasmaPainter` (Байер 4×4, 5 амбров),
`_ActButton` (×/↻/папка/корзина в строке).

---

## 5. Ядро kdcore — карта функций

### 5.1 `Detector.h` — классификация
`serviceFor(raw)` (youtube/youtubeMusic/instagram/tiktok/pinterest/vk/
spotify/appleMusic/vkMusic/soundcloud/unknown по хостам; vk.com + /audio →
vkMusic), `needsResolve` (spotify, appleMusic, vkMusic), `cookieSensitive`,
`title(s)`, `hasPlaylist`, `isCollection` (playlist?list=/album//sets/,
без watch?v=), `looksLikeLink` (есть `.` и `/`).

### 5.2 Утилиты
- `kd_compat.h`: trim/lower/contains/startsWith/endsWith/fromFirst/
  fromLast/upToFirst/searchRegex/indexOf*/containsOnly/replaceAll/
  replaceChar/getInt/getDouble/splitTokens/splitWhitespace/splitLines/
  join/containsVec/urlEscape; pathStr/isFile/isDir/ensureDir.
- `kd_process.h :: ChildProcess`: `start` (fork+execvp, stdout+stderr в
  одну трубу), `read(buf,n,timeout)` (poll), `isRunning`, `waitExitCode`,
  `kill` (**SIGTERM** — .part сохраняется), `closeOutput`.
- `kd_thread.h :: Wake` — считающий семафор (signal/waitForever/waitMs);
  исправляет потерю пробуждений при 2 воркерах.
- `kd_http.h` — libcurl: `fetch(url,timeout[,headers])`, `downloadToFile`;
  UA Chrome, FollowLocation, SSL on.
- `kd_url.h :: parseUrl/queryParam` — схема/хост/путь (нижний регистр),
  query-параметры.
- `platform/BrowserDefault.h/.mm` — `defaultBrowser()`, `build()` (цепочка
  установленных браузеров для `--cookies-from-browser`), `isInstalled`.

### 5.3 `DestResolver.h`
`folderName()` = «K LOAD»; `homeDir()`; `baseFolder()` = ~/Downloads (нет —
~/Documents); `defaultFolder()` = base + «K LOAD».

### 5.4 `VpnMonitor.h/.cpp`
Поток «kd-vpn»: раз в 1 с `checkOnce()` = `route -n get default` →
интерфейс utun/tun/tap/ppp/ipsec/wg; debounce: 2 подряд одинаковых →
State::on/off + onChange. `setOnChange` под мьютексом; `state()`.

### 5.5 `Engine.h/.cpp`

**Данные**: `QueueItem` (id, link, service, state(queued/working/paused/
done/failed), progress, stage, title, itemIndex/Total, skipped,
batchIndex/Total, files[], isAudio, maxHeight, audioFormat, wholePlaylist,
dest, cookieChain[], nameOverride, sections, playlistLimit, container,
imageFormat, isPhoto, durationHint, sectionsWarning, autoRetries,
forceOverwrite, flags→cancelled); `Options` (dest, mode, quality, audioFormat,
wholePlaylistOverride, nameOverride, sections, playlistLimit, container,
imageFormat, durationHint, forceOverwrite); `Probe` (ok, error, link,
resolved, service, title/uploader/thumbnail, duration, count, heights[],
isPlaylist, hasPlaylist, isPhoto, isSearch, drm, shortVideo, results[],
entryUrls[], entryTitles[]); `ProbeRunner` (поколения разборов: pending +
generation, один поток); `Engine` (2 воркера, mutex, Wake, pausedFlag,
pauseRequested, probeCache 5 мин/48, thumbIndex).

**Публичные методы Engine**:

| Метод | Строка | Назначение |
|---|---|---|
| `splitLinks` | 269 | текст → ссылки (кавычки, дубликаты) |
| `enqueueBatch` | 288 | ссылки → QueueItem (isAudio форс: soundcloud/ytmusic/needsResolve; wholePlaylist=isCollection; dest/контейнер/секции) |
| `enqueuePhoto` | 356 | Pinterest-фото, native-путь |
| `cancel / remove / clearFinished` | 373/387/397 | отмена (queued→cancelled), удаление строки, очистка (queued→cancelled; working/paused живут) |
| `retryNetworkFailed` | 417 | failed с сетевой причиной → queued, ≤2 раз |
| `snapshot` | 447 | копия items под мьютексом |
| `setPaused` | 521 | пауза/снятие (paused→queued, SIGTERM→.part) |
| `probe / probeAsync / buildAndCache` | 2316/—/2353 | разбор карточки; кэш 5 мин (только удачные и с длительностью) |
| `predictFiles` (static) | 2246 | предсказание имён + exists (см. §6) |
| `ytFileName` (static) | 2210 | зеркало sanitize_filename yt-dlp |
| `cleanTrackName / safeName` | 2161/2197 | чистка «(Official Video)» / файлобезопасное имя (110) |
| `cachedThumbnail / prefetchThumbnail / thumbCacheDir` | 1762/1810/1755 | дисковый кэш превью |
| `findToolsDir / engineLog` | 589/36 | поиск инструментов; журнал в App Support |
| `splitLinks` см. выше; `fetch/between` | 1700/1705 | обёртки http; вырезка between |

**Приватные методы Engine**:

| Метод | Строка | Назначение |
|---|---|---|
| `workerLoop` | 471 | 2 воркера: пауза-сон 200 мс → первый queued → processItem; будим соседа |
| `processItem` | 551 | роутинг: photo→photoFallback, needsResolve→startResolve, иначе startNative; Pinterest-fail → фото-фолбэк |
| `pauseItem` | 514 | state=paused, «ПРИОСТАНОВИЛ...» |
| `finish/setStage` | 456/463 | смена состояния/статуса + onChange |
| `baseArgs` | 650 | общий хвост yt-dlp: ignore-config, no-warnings, newline, retries 5, socket 20, concurrent-fragments 4, **no-mtime, no-overwrites**, print @T/@F, -P dest, ffmpeg-location, cookies? |
| `runYtDlp` | 713 | запуск+цикл чтения: пауза→SIGTERM; stall 90 с → 2 попытки (2-я с --no-part и чисткой .part-Frag); «запуск/завершён» в engine.log |
| `consume` | 840 | строки вывода: [Merger]→«Склеиваю», already downloaded→skipped, ERROR→errTail, [download] %→progress, **@T**→title/index/stage, **@F**→files[]+финальный титул |
| `startNative` | 1056 | одиночка/поиск: ветки аудио (-x, force-overwrites всегда, [ext]-суффикс), instagram/pinterest/tiktok (-f b/bv*+ba), высота, merge; nameOverride→-o literal + parse-metadata; шаблон `%(title).120B[ [ext]][ [хрон]].%(ext)s`; хрон: --download-sections + force-keyframes, Pinterest — резать ffmpeg'ом; проверки отрезка до/после; фото-fallback |
| `startPlaylist` | 922 | wholePlaylist: flat -J → список urls → по-роликово `subdir/NN - %(title).100B.%(ext)s` (subdir=safeName(титул)); недоступные пропускаются; «Готово · файлов: N · пропущено: M» |
| `startResolve` | 1443 | каталоги: resolveSpotify/Apple/OpenGraph → tracks; альбом → папка; по трекам downloadTrack |
| `downloadTrack` | 1582 | «Артист - Название»: точные кандидаты (candidateMatches) → ytsearch/scsearch5; имя safeName(query)+[ext]+хрон |
| `retryWithCookies` | 1393 | детект «нужен вход» (login/cookie/private/rate-limit/403…) |
| `humanError` | 1402 | сырая ошибка yt-dlp → человеческая строка (антибот, закрыто, 403, DRM, нет видео, недоступно, формат, сеть) |
| `fetch/between/youtubeVideoId` | 1700/1705/1715 | сеть, вырезки, извлечение ID |
| `nameTokens/candidateMatches` | 1818/1834 | сверка найденного трека с запросом (токены, исполнитель, длительность) |
| `resolveSpotify/resolveAppleMusic` | 1874/1973 | embed/lookup API → треки альбома/трека |
| `resolveOpenGraph` | 2026 | og:title для ВК Музыки и прочих |
| `startPhotoFallback/downloadPinterestPhoto` | 2053/2068 | oEmbed → originals → файл → sips (jpg/png) |
| `probePinterestPhoto/probeDrm` | 2559/2727 | карточки фото/DRM (oEmbed, «ФАЙЛ ЗАЩИЩЁН DRM») |
| `searchAppleMusicList/searchSpotifyList/searchPinterest` | 2588/2644/2714 | каталоги текстового поиска (iTunes Search, Spotify-токен, Pinterest закрыт) |
| `probeSearch` | 2816 | YouTube: страница выдачи ytInitialData → videoRenderer (до 8) → фолбэк ytsearch; SoundCloud: scsearch; объединение |
| `predictFiles` | 2246 | см. §6 |

**Static-хелперы**: `appDataRoot` (~/Library/Application Support),
`ytdlpBinaryName`, `audioFormatName/ext`, `fmtSeconds`, `chronSuffix`
(« [MM:SS–MM:SS]», en-dash), `jtext/jnum/dig`, `findVideoRenderer(s)/
runsText/searchThumb`, `childPath` (PATH+наши инструменты), `captureOut`
(тихий -J).

### 5.6 `CAPI.cpp` — каждый экспорт

| Функция | Мостит |
|---|---|
| `kd_attach_dart` | Dart_InitializeApiDL |
| `kd_engine_create(tools_dir)` / `kd_engine_destroy` | Engine + VpnMonitor + порт |
| `kd_set_event_port` | порт для событий (JSON-строки) |
| `kd_snapshot` | массив itemToJson (все поля QueueItem) |
| `kd_split_links` | Engine::splitLinks |
| `kd_enqueue_batch` | parseOptions (dest/mode/quality/audioFormat/wholePlaylist/nameOverride/sections/playlistLimit/**container/imageFormat**/durationHint/forceOverwrite) → Engine |
| `kd_enqueue_photo` | Engine::enqueuePhoto |
| `kd_cancel / kd_remove / kd_clear_finished` | управление |
| `kd_set_paused / kd_is_paused` | пауза |
| `kd_retry_network_failed` | автоповторы |
| `kd_probe_blocking / kd_probe_async / kd_probe_async_source` | разборы |
| `kd_predict_files` | Engine::predictFiles (без движка, static) |
| `kd_thumb_path` | Engine::cachedThumbnail |
| `kd_vpn_state / kd_default_dest / kd_tools_status` | сервисные |
| `kd_string_free / kd_version` | строки/версия |

### 5.7 Тесты `tests/kd_tests.cpp` (58 ok)
Detector (12), link-логика, имена (safeName/cleanTrackName), `ytFileName`
(полноширинные, tab/\n, усечение 120B на кириллице), `predictFiles`
(template/literal/flat + exists по tmp-папке), CAPI-базовое, пауза очереди
на фейковом загрузчике (2 воркера, пауза/докачка/отмена/clearFinished).
`--live` — probes и сквозные скачивания (YouTube/SoundCloud/фрагменты).

---

## 6. Нейминг файлов — сводная таблица (источник правды: ядро)

| Случай | Путь/имя | Пример |
|---|---|---|
| Одиночка видео | `dest/ytName(≤120B).ext` | `Me at the zoo.mp4` |
| Одиночка аудио ≠ mp3 | `… [ext].ext` | `Song [m4a].m4a` |
| ХРОН | `… [MM:SS–MM:SS].ext` | `Me at the zoo [00:00–00:10].mp4` |
| Поиск по названию | `safeName(запрос).ext` | `radiohead creep.mp3` |
| Ролик плейлиста (карточка) | `dest/safeName(плей)/NN - имя.ext` | `Плей/01 - Al Johnson - Peaceful.mp4` |
| Плейлист в пачке | то же, flat-шаблон | `Мой микс/02 - Трек？.mp4` |
| Каталог (Spotify/Apple/ВКМ) | `safeName(«Артист - Трек»)[ [ext]].ext` | `Forss - Flickermood.mp3` |
| Фото Pinterest | `safeName(титул).jpg|png` | `Pin.jpg` |

`ytFileName` = зеркало sanitize_filename yt-dlp (проверено на бинаре
2026.08.19): `/→⧸ \→⧹ :→： *→＊ ?→？ "→＂ <→＜ >→＞ |→｜`, `\n`→пробел,
прочие control — вон, усечение `%(title).NB` — **сырыми байтами до**
санитизации, UTF-8-граница целая.

---

## 7. Нативный слой macOS (`app/macos/Runner/MainFlutterWindow.swift`)

- Окно: titlebar скрыт/прозрачен, fullSizeContentView, фон = пластик,
  zoom мёртв, fullscreen запрещён, 560×670, aspect-ratio, 0.8×–1.4×.
- Канал `kload/native`: `chooseFolder` (NSOpenPanel) и `setZones`
  (дизайн-координаты → contentView c cover-масштабом, y перевёрнут).
- `DragOutHelper`/`DragSourceView`: невидимые NSView над готовыми строками;
  mouseDown (файл существует) → NSDraggingSession, `.copy`; кнопки строк
  остаются вне зон (w−84).

---

## 8. Диск и логи

| Путь | Что |
|---|---|
| `~/Downloads/K LOAD` | папка по умолчанию |
| `~/Library/Application Support/K LOAD/engine.log` | журнал каждого запуска/завершения yt-dlp с аргументами и errTail |
| `~/Library/Application Support/K LOAD/cache/thumbs` | кэш превью |
| `~/Library/Application Support/K LOAD/tools` | инструменты (fetch-tools/.pkg) |
| `~/Library/Application Support/K LOAD dev/K LOAD.app` | дев-бандл (dev-build.sh: сборка → копия вне iCloud → xattr -cr → ad-hoc codesign → verify) |

Логи Dart: `open … --stdout/--stderr /tmp/kload_*.log` (debugPrint).

---

## 9. Поверхность аудита — где тонко (честный список)

1. **`screen.dart` ~3800 строк, один State** — вся UI-логика в одном
   классе; риск гонок состояния (см. историю: batchProbes, forceDownload).
2. **Гонка «СКАЧАТЬ во время разборов пачки»** — закрыта блокировкой
   кнопки + гвардом, но guard молча делает return: если состояние кнопки
   разойдётся с гвардом, пользователь получит «мёртвую» кнопку без
   объяснения.
3. **Предсказание имён vs yt-dlp** — зеркало проверено на 2026.08.19;
   обновление yt-dlp может поменять sanitize (тестов-сверок с живым
   бинарником нет — только зафиксированные ожидания). Крайние случаи:
   progressive-потоки (ext не сменится на container), IG-фото (предскажем
   mp4, придёт jpg).
4. **Плейлист с недоступным списком** (наш случай PLW4NDfp…) — не
   предсказуем и часто падает целиком («скрыта или требует входа»); часть
   «пропустить» для него = доверие скипу ядра.
5. **`enqueueBatch` без дедупликации** — повторный СКАЧАТЬ той же ссылки
   создаёт вторую задачу (защита только в UI: busyVariant + модалка).
6. **`predictFiles` на UI-потоке** — синхронный FFI; при сотнях роликов
   сотни `stat` — пока быстро, но не измерено.
7. **Устаревшие артефакты загрузок** — `.part/.ytdl/-FragNN` копятся
   (чистятся только при stall-повторе); мусорка диспетчера не удаляет их.
8. **Тосты/статусы вне каталога ошибок** («ПО ССЫЛКЕ ФОТОГРАФИЯ…»,
   «УЖЕ СТОИТ В ОЧЕРЕДИ») — формат единый, но не в списке из 13.
9. **cookieChain** — невидимый подбор cookies из браузеров: сделан, но в
   startNative/startPlaylist сейчас только анонимная попытка
   (`attempts = {""}`); retryWithCookies читает errTail, но цепочка не
   используется (код-остаток прежней версии) — уточнить поведение при
   аудите приватности.
10. **`--no-mtime` всегда** — mtime файла = момент скачивания (осознанно:
    «ещё раз» даёт свежий mtime).
11. **dev-бандл — debug-сборка** (kernel_blob с логикой) лежит в
    Application Support; для релиза — release-сборка (PKG-PLAN).
12. **Кэш разборов 5 мин** — смена заголовка источника в течение 5 минут
    не отразится на предсказании (edge для «уже скачано»).

---

## 10. Приложения

### 10.1 Формат ошибок (единый, без точки)
13 позиций каталога: НЕТ СЕТИ ИЛИ VPN / КОНТЕНТ НЕДОСТУПЕН / ЗАПИСЬ
ЗАЩИЩЕНА DRM / ССЫЛКА НЕ РАСПОЗНАНА / ИСТОЧНИК НЕ ПОДДЕРЖИВАЕТ ПОИСК /
НЕВЕРНЫЙ ДИАПАЗОН / ФОРМАТ НЕДОСТУПЕН / АУДИОДОРОЖКИ НЕТ / НЕ УДАЛОСЬ
СОХРАНИТЬ ФАЙЛ / ЗАГРУЗКА ОТМЕНЕНА / ИНСТРУМЕНТЫ НЕ НАЙДЕНЫ / НЕ УДАЛОСЬ
ПОДГОТОВИТЬ ФАЙЛ / ЧТО-ТО ПОШЛО НЕ ТАК — каждая с подсказкой в `[...]`
через « — ». Действия карточки: retry, openLink, close, selectVideo,
selectMusic, changeChron, changeFormat.

### 10.2 Сервисы и маршруты скачивания
| Сервис | Маршрут | Имя файла |
|---|---|---|
| YouTube/YT Music | yt-dlp (video merge / audio -x) | заголовок (±[ext], ±хрон) |
| TikTok / Instagram | yt-dlp (прогрессив/merge, без фильтров высоты) | заголовок |
| Pinterest видео | yt-dlp (+фолбэк на фото; аудио — ffmpeg-извлечение) | заголовок |
| Pinterest фото | oEmbed → originals → curl → sips | safeName(титул) |
| VK (видео) | yt-dlp | заголовок |
| VK Music | resolve (og) → YouTube/SoundCloud | safeName(«Артист - Трек») |
| Spotify / Apple Music | resolve (embed/iTunes) → ytsearch5 | safeName(«Артист - Трек») |
| SoundCloud | yt-dlp (форс-аудио) | заголовок (аудио-вариант) |

### 10.3 Ключевые инварианты (для регрессионного аудита)
1. Проверка «уже скачано» — только по папке назначения; диспетчер не
   источник правды; работает после [ОЧИСТИТЬ] и перезапуска.
2. Хватает одного дубля в пачке для окна; непредсказуемый элемент не
   отключает проверку остальных.
3. Загрузка не начинается, пока разборы пачки не завершены.
4. «ЕЩЁ РАЗ» = `--force-overwrites` ПОСЛЕ baseArgs; старый файл живёт до
   успеха (.part → rename); mtime свежий; копий «(1)» нет.
5. Пауза: SIGTERM → .part; [ОЧИСТИТЬ] сбрасывает паузу;working/paused
   переживают очистку.
6. Ошибки — всегда `ЗАГОЛОВОК — [подсказка]`, без точки.
7. СКАЧАТЬ · N живой: N = ссылки пачки / лимит плейлиста / 1.
