# ПЛАН сборки установщика .pkg — K LOAD V.1.0

Статус: ПЛАН. Сборка не начиналась — каждый этап согласовывается отдельно.
Продукт: K LOAD V.1.0 (kvartal records). Платформа: macOS 12+ (universal arm64/x86_64 — определить на этапе 1).

## Этап 1. Сертификаты и подпись
- Нужен Apple Developer Program аккаунт (99 $/год).
- Сертификаты: «Developer ID Application» (подпись приложения) и
  «Developer ID Installer» (подпись самого .pkg).
- Проверка: `security find-identity -v -p codesigning`.
- Подпись бандла: `codesign --deep --force --options runtime --sign "Developer ID Application: ..." "K LOAD.app"`.
- Проверка: `codesign --verify --deep --strict --verbose=2`.
- Решение: dist-istribute только через нотаризацию (Gatekeeper) или
  ad-hoc + правая кнопка «Открыть» (без Dev-аккаунта).

## Этап 2. Нотаризация (если есть Dev-аккаунт)
- App Store Connect API ключ (notarytool) или app-specific пароль.
- `xcrun notarytool submit K_LOAD.pkg --keychain-profile ... --wait`.
- `xcrun stapler staple K_LOAD.pkg` — прибить билет к пакету.
- Проверка на «чистой» машине: `spctl -a -v "K LOAD.app"`.

## Этап 3. Релиз-сборка приложения
- `flutter build macos --release` (без `|| true` — падения codesign
  в дереве проекта обрабатываются вручную: xattr -cr до подписи).
- Ядро kdcore пересобрать в Release (`cmake -B build -DCMAKE_BUILD_TYPE=Release`).
- Проверка свежести: маркер-строка в kernel_blob.bin (грабли dev-build.sh).
- Инструменты (yt-dlp/ffmpeg/deno) — либо бандлить в Resources, либо
  оставить докачку core/fetch-tools.sh при первом запуске. РЕШЕНИЕ НУЖНО.

## Этап 4. Иконка приложения и оформление установщика
- Иконка: AppIcon.appiconset уже заполнен (16–1024). Проверить icns в бандле.
- Фон установщика (опционально): 3200×2000 png в тёмно-оранжевой стилистике.
- Локализации установщика (ru + en).

## Этап 5. Версия и структура пакета
- Версия: 1.0 (CFBundleShortVersionString = 1.0, CFBundleVersion = 1;
  pubspec.yaml version: 1.0.0+1; тег v1.0.0 в git).
- Идентификатор: ru.kvartal.kload (уже задан).
- Структура компонентного pkg:
  - Payload: K LOAD.app → /Applications/K LOAD.app
  - postinstall: ничего критичного (кэш и настройки создаются при запуске)
- Дистрибутив: `productbuild --distribution Distribution.xml` с title,
  welcome/readme/license/license-en файлами, фоном.

## Этап 6. Список зависимостей (что попадает в пакет/докачивается)
- FluttermacOS.framework, App.framework (собирается Flutter).
- libkdcore.dylib (наше ядро, статически линкует nlohmann/json).
- Внешние исполняемые: yt-dlp (около 40 МБ), ffmpeg, ffprobe, deno —
  бандлить (пакет +100–150 МБ, но работает офлайн и без докачки) или
  докачивать fetch-tools.sh при первом запуске. РЕШЕНИЕ НУЖНО.
- Права доступа: ключницы не запрашиваются (cookie-цепочки убраны).

## Этап 7. Тексты установщика (ЧЕРНОВИКИ — требуют одобрения)
- Приветствие (welcome):
  «K LOAD — загрузчик музыки и видео. Установите приложение —
  и сохраняйте треки и клипы в пару кликов.»
- Лицензия: MIT или проприетарная оговорка kvartal records + пункт
  «Используйте только для контента, на который у вас есть права».
- Завершение: «Готово. K LOAD установлен в Программы.
  Вставьте ссылку — и жмите СКАЧАТЬ.»
- Readme: краткая инструкция (поле ввода, ВИДЕО/МУЗЫКА/ХРОН, диспетчер).

## Этап 8. Сборка и проверка
- `pkgbuild --root <approot> --scripts scripts --identifier ru.kvartal.kload --version 1.0 K_LOAD.pkg`
- `productbuild --distribution ... --package-path ... "K LOAD V.1.0.pkg"`
- Подпись Installer-сертификатом + нотаризация пакета.
- Тест: чистая macOS виртуалка/второй пользователь, установка, запуск,
  скачивание одной ссылки, проверка Gatekeeper.

## Открытые вопросы (нужно решение до сборки)
1. Бандлить инструменты или докачивать при первом запуске?
2. Universal (arm64+x86_64) или только Apple Silicon?
3. Нотаризация есть (Dev-аккаунт) или ad-hoc сборка?
4. Тексты лицензии — чей текст финальный?
