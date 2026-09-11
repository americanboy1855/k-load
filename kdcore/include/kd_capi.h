#pragma once

// C-API поверх kdcore — единая точка входа для Flutter (dart:ffi) и будущих
// JUCE-плагинов. Всё общение строками — UTF-8; структурированные ответы —
// JSON-строки, размениваемые через kd_string_free / обратные вызовы.
//
// События на Dart приезжают через Dart_PostCObject (порт задаётся
// kd_set_event_port): JSON вида
//   {"type":"queue_changed"}                     — очередь изменилась, читать kd_snapshot
//   {"type":"probe","text":…, …поля Probe…}      — разбор ссылки для карточки
//   {"type":"vpn","state":"on"|"off"}            — сменилось состояние VPN

#include <stdint.h>

#if defined(_WIN32)
  #define KD_EXPORT __declspec(dllexport)
#else
  #define KD_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Инициализация Dart API DL — вызвать один раз до kd_set_event_port,
// передав NativeApi.initializeApiDLData из Dart.
KD_EXPORT intptr_t kd_attach_dart (void* dart_initialize_api_dl_data);

typedef struct kd_engine kd_engine;

// tools_dir: где лежат yt-dlp/ffmpeg; NULL — стандартный поиск
// (env K_LOAD_TOOLS → App Support → бандл → дерево репозитория).
KD_EXPORT kd_engine* kd_engine_create (const char* tools_dir);
KD_EXPORT void kd_engine_destroy (kd_engine* e);

// Подписка на события (nativePort из SendPort.nativePort). 0 — успех.
KD_EXPORT int kd_set_event_port (kd_engine* e, int64_t native_port);

// Снимок очереди — JSON-массив заданий (новые сверху). Освободить kd_string_free.
KD_EXPORT char* kd_snapshot (kd_engine* e);

// Разбить текст на ссылки — JSON-массив строк. Освободить kd_string_free.
KD_EXPORT char* kd_split_links (const char* text);

// Постановка в очередь. links_json — JSON-массив строк ссылок;
// options_json — JSON:
//   {"dest":"/path", "mode":"video"|"audio", "quality":"720"|"1080"|"2160"|"best",
//    "audioFormat":"mp3"|"m4a"|"wav"|"flac", "wholePlaylist":-1|0|1,
//    "playlistLimit":N (только первые N роликов плейлиста),
//    "nameOverride":"...", "sections":"10:00-10:20",
//    "forceOverwrite":true (заменить существующий файл — «скачать ещё раз»)}
// — все поля необязательны.
// sections — ХРОН, отрезок «начало-конец» (таймкоды или секунды); действует
// только на одиночные файлы, у плейлистов игнорируется.
// Возвращает число поставленных заданий (0 — ничего не распознано).
KD_EXPORT int kd_enqueue_batch (kd_engine* e, const char* links_json, const char* options_json);
KD_EXPORT int kd_enqueue_photo (kd_engine* e, const char* link, const char* options_json);

// Предсказание итоговых имён файлов и проверка папки назначения — источник
// правды для окна «этот файл уже скачан» (работает после очистки списка и
// перезапуска). request_json: {"files":[...]} — заявки, собранные из тех же
// данных, что уйдут в очередь:
//   {"kind":"template", "dir":"/dest", "title":"заголовок из разбора",
//    "service":0 (Detector::Service), "ext":"mp4",
//    "suffix":" [m4a]" (для не-mp3 аудио, иначе пусто), "sections":"0:00-0:10"}
//   {"kind":"literal", "dir":…, "name":"имя от приложения",
//    "ext":…, "suffix":…, "sections":…}  — поиск по названию, ролики
//    плейлиста («01 - …»), фотография Pinterest
//   {"kind":"flat", "dir":…, "playlistTitle":"…", "index":1,
//    "title":"…", "ext":…} — плейлист внутри пачки (папка + «01 - …»)
// Ответ: {"results":[{"i":0,"dir":…,"name":"Имя.mp4","path":"/dest/Имя.mp4",
//   "exists":true|false}, …]}. Сети и разбора не требует. Освободить
// kd_string_free.
KD_EXPORT char* kd_predict_files (kd_engine* e, const char* request_json);

KD_EXPORT void kd_cancel (kd_engine* e, int id);
KD_EXPORT void kd_remove (kd_engine* e, int id);
KD_EXPORT void kd_clear_finished (kd_engine* e);

// Глобальная пауза очереди: текущие процессы останавливаются (yt-dlp
// закрывает .part — продолжение с той же позиции), следующие задания не
// подаются. kd_set_paused(e, 0) возвращает приостановленное к докачке.
KD_EXPORT void kd_set_paused (kd_engine* e, int paused);
KD_EXPORT int kd_is_paused (kd_engine* e);

// Повтор заданий, упавших по сети (VPN вернулся). Возвращает число
// поставленных обратно в очередь.
KD_EXPORT int kd_retry_network_failed (kd_engine* e);

// Блокирующий разбор ссылки/названия (JSON Probe). Освободить kd_string_free.
KD_EXPORT char* kd_probe_blocking (kd_engine* e, const char* text);

// Фоновый разбор: результат приедет событием {"type":"probe", …}.
KD_EXPORT void kd_probe_async (kd_engine* e, const char* text);

// То же для текстового запроса с выбором источника поиска
// («youtube» / «soundcloud»; пусто/NULL — авто).
KD_EXPORT void kd_probe_async_source (kd_engine* e, const char* text, const char* source);

// Локальный файл превью по адресу картинки: при первом вызове скачивается
// (блокирующе, до 8с), затем отдаётся из дискового кэша. Пусто — превью
// нет. Освободить kd_string_free.
KD_EXPORT char* kd_thumb_path (kd_engine* e, const char* url);

// Состояние VPN: 0 — неизвестно, 1 — включён, 2 — выключен.
KD_EXPORT int kd_vpn_state (kd_engine* e);

// Папка назначения по умолчанию — JSON {"base":…,"folder":…,"subfolder":"K LOAD"}.
// Освободить kd_string_free.
KD_EXPORT char* kd_default_dest (kd_engine* e);

// Лежат ли инструменты по месту поиска (без них качать нечем) — JSON
// {"found":true|false,"dir":"…"}.
KD_EXPORT char* kd_tools_status (kd_engine* e);

KD_EXPORT void kd_string_free (char* s);
KD_EXPORT const char* kd_version();

#ifdef __cplusplus
} // extern "C"
#endif
