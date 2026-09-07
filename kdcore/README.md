# kdcore — ядро скачивания K LOAD

Единое чистое C++17 ядро без JUCE. Один и тот же код обслуживает Flutter-приложение
(через `libkdcore.dylib` + C-API) и будущий VST3/AU-плагин (напрямую). Порт с
`core/engine` (JUCE-версия), поведение перенесено 1:1.

## Сборка

```sh
cd kdcore
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Результат: `build/libkdcore.dylib` (universal: arm64 + x86_64) и `build/kd_tests`.

## Тесты

```sh
./build/kd_tests          # чистые: детектор, ссылки, имена, C-API
./build/kd_tests --live   # + живые источники: probe и сквозные скачивания
```

Живые тесты прогонять после каждого обновления yt-dlp. Для поиска инструментов
ядро само находит `core/tools` (прогулка вверх по дереву); можно задать вручную:
`K_LOAD_TOOLS=/path/to/tools ./build/kd_tests --live`.

## Инструменты

yt-dlp + статические ffmpeg/ffprobe лежат в `core/tools` (в git не попадают —
большие). Докачка: `core/fetch-tools.sh` (evermeet.cx — статика, папка `libs/`
не нужна; сборки ffmpeg с внешними `libs/` ядро не поднимет).

## Поиск инструментов (findToolsDir)

1. env `K_LOAD_TOOLS` (для тестов/нестандартных установок)
2. `~/Library/Application Support/K LOAD/tools`
3. `/Library/Application Support/K LOAD/tools`
4. ресурсы собственного бандла (`Contents/Resources`)
5. `tools/` рядом с бинарником
6. установленные `~/Applications/K LOAD.app`, `/Applications/K LOAD.app`
7. прогулка вверх по дереву репозитория (`core/tools`)

## C-API (`include/kd_capi.h`)

Все строки — UTF-8. Сложные ответы — JSON-строки (освобождать `kd_string_free`).
События — на Dart-порт (`kd_set_event_port` + `kd_attach_dart` с
`NativeApi.initializeApiDLData`): `queue_changed` / `probe` / `vpn` как JSON.

## Отличия от JUCE-версии (сознательные)

- Папка загрузок по умолчанию — `Загрузки/K DWNLD` (новое имя продукта);
  детектор свежего проекта DAW перенесён не будет — он нужен только плагину,
  вернём его на этапе JUCE в адаптер плагина.
- Журнал движка: `~/Library/Application Support/K LOAD/engine.log`.
- `splitLinks` снимает кавычки вокруг ссылок (копипаст из мессенджеров).
- Приложение не JUCE-плагин: конвейер событий — Dart-порт вместо ChangeBroadcaster.
- Победил редкий гонок: колбэк VpnMonitor под мьютексом, состояние передаётся
  аргументом (владелец мог уже обнулить члены к моменту join в деструкторе).
