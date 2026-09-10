#pragma once

// Очередь загрузок и разбор ссылок. Порт core/engine (JUCE) на чистый
// C++17: std::string/filesystem/thread/process вместо juce-типов. Правила
// «кто чем качается» живут в Detector.h.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <map>
#include <chrono>

#include "Detector.h"
#include "kd_compat.h"
#include "kd_process.h"
#include "kd_thread.h"

// Качать видео или музыку. У музыкальных сервисов выбора нет — там всегда
// звук, что бы ни было выбрано.
enum class MediaMode { video, audio };

// Высота кадра; best — без ограничения.
enum class VideoQuality { q720, q1080, q2160, best };

enum class AudioFormat { mp3, m4a, wav, flac, ogg };

struct QueueItem
{
    enum class State { queued, working, done, failed };

    /// Отмена читается из UI-потока и рабочего — прячем атомик в общую
    /// кучу, чтобы сам QueueItem оставался копируемым (снимки для UI).
    struct Flags { std::atomic<bool> cancelled { false }; };

    int id = 0;
    Str link;
    Detector::Service service = Detector::Service::unknown;

    State state = State::queued;
    float progress = 0.0f;
    /// Что происходит — человеческая строка: «Качаю 2 из 14», «Склейка…»,
    /// текст ошибки. Живёт отдельно от state, чтобы строка не ветвилась.
    Str stage = "В очереди";
    Str title;
    int itemIndex = 0, itemTotal = 1, skipped = 0;
    /// Номер в пачке, когда качается несколько ссылок сразу.
    int batchIndex = 0, batchTotal = 0;
    StrVec files;

    /// Что именно качать — замораживается при постановке в очередь: чипы
    /// на экране человек может крутить, пока очередь работает.
    bool isAudio = false;
    int maxHeight = 0;                    // 0 — без ограничения («Макс.»)
    AudioFormat audioFormat = AudioFormat::mp3;
    bool wholePlaylist = false;
    fs::path dest;                        // пусто — автоматическая папка
    /// Невидимая цепочка входа: браузер по умолчанию и остальные
    /// установленные. Имена не попадают в интерфейс, только в журнал.
    StrVec cookieChain;
    /// Найденному по названию треку имя файла даёт запрос, а не ютубовский
    /// «… (Official Video)».
    Str nameOverride;
    /// ХРОН — отрезок «начало-конец» (таймкоды или секунды, как понимает
    /// yt-dlp). Пусто — качаем целиком. Только для одиночных файлов.
    Str sections;
    /// Плейлист: только первые N роликов. 0 — плейлист целиком.
    int playlistLimit = 0;
    /// Контейнер склейки видео (mp4/webm/mkv); пусто — mp4.
    Str container;
    /// Формат изображения (jpg/png); пусто — как скачалось.
    Str imageFormat;
    /// Pinterest-фотография: качаем напрямую, без yt-dlp.
    bool isPhoto = false;
    /// Длительность записи из разбора (сек) — для проверки ХРОНа.
    int durationHint = 0;
    /// Итоговый файл короче ожидаемого отрезка — скачивание не засчитано.
    Str sectionsWarning;
    /// Сколько раз задание уже автоматически повторялось после сети.
    int autoRetries = 0;

    std::shared_ptr<Flags> flags = std::make_shared<Flags>();

    bool cancelled() const { return flags->cancelled.load (std::memory_order_relaxed); }
    void setCancelled() { flags->cancelled.store (true, std::memory_order_relaxed); }
};

using QueueItemPtr = std::shared_ptr<QueueItem>;

/// Один результат текстового поиска (для списка на карточке).
struct SearchResult
{
    Str title;
    Str uploader;                          // исполнитель/канал — отдельной строкой
    Str url;
    int duration = 0;
    Detector::Service service = Detector::Service::unknown; // подпись источника в выдаче
};

/// Что увидели по ссылке до скачивания — содержимое карточки на экране.
struct Probe
{
    bool ok = false;
    Str error;                             // человеческая причина, если не вышло
    Str link;                              // что просили
    Str resolved;                          // для поиска — найденный ролик
    Detector::Service service = Detector::Service::unknown;
    Str title, uploader, thumbnail;
    int duration = 0;
    int count = 1;                         // записей в подборке/альбоме
    std::vector<int> heights;              // доступные высоты кадра
    bool isPlaylist = false;               // это подборка/альбом
    bool hasPlaylist = false;              // в ссылке есть list= — спросим, что качать
    bool isPhoto = false;                  // Pinterest-фотография
    bool isSearch = false;                 // искали по названию
    bool drm = false;                      // запись защищена DRM — скачать нельзя
    bool shortVideo = false;               // TikTok/Instagram: один MP4/MP3 без списков качества
    std::vector<SearchResult> results;     // варианты текстового поиска (до 20)
    /// Плоский список роликов плейлиста — для раскладки на отдельные задачи.
    StrVec entryUrls;
    StrVec entryTitles;
};

/// Один-единственный разбор ссылки: пока человек допечатывает, старые
/// разборы отменяются поколением (generation), а не убийством процесса.
class ProbeRunner
{
public:
    using ProbeFn = std::function<Probe (const Str&)>;
    using DoneFn = std::function<void (const Probe&)>;

    ProbeRunner();
    ~ProbeRunner();

    void start (const Str& text, ProbeFn fn, DoneFn onDone);

private:
    void run();

    std::mutex mutex;
    Str pending;
    int generation = 0;
    ProbeFn probeFn;
    DoneFn callback;
    std::atomic<bool> quitFlag { false };
    Wake wake;
    std::thread thread;
};

/// Очередь загрузок.
///
/// Задания идут по одному, поочерёдно, в фоне: параллельная загрузка делит
/// канал и делает прогресс невнятным. yt-dlp и ffmpeg ищутся в App Support,
/// в бандле приложения и в дереве репозитория (для разработки).
class Engine
{
public:
    struct Options
    {
        fs::path dest;                    // пусто — автоматическая папка
        MediaMode mode = MediaMode::video;
        VideoQuality quality = VideoQuality::q1080;
        AudioFormat audioFormat = AudioFormat::mp3;
        /// Переопределение «что качать» из карточки: подборка целиком или
        /// один ролик. -1 — решает сама ссылка.
        int wholePlaylistOverride = -1;   // -1 авто, 0 нет, 1 да
        /// Имя файла для трека, найденного по названию.
        Str nameOverride;
        /// ХРОН: отрезок «начало-конец» для одиночного файла (не плейлиста).
        Str sections;
        /// Плейлист: скачать только первые N роликов. 0 — целиком.
        int playlistLimit = 0;
        /// Контейнер склейки видео: mp4/webm/mkv.
        Str container;
        /// Формат изображения для Pinterest: jpg/png.
        Str imageFormat;
        /// Длительность записи из разбора (сек) — проверка ХРОНа до запуска.
        int durationHint = 0;
    };

    /// Все изменения очереди. Приезжает из рабочих потоков — интерфейсу
    /// (или C-API) самому решать, как доставить события на свой поток.
    std::function<void()> onChange;

    Engine();
    ~Engine();

    /// Пачка ссылок: каждая строка — отдельное задание со своими правилами
    /// (YouTube качается сам, Spotify читается по названию, Pinterest — фото).
    void enqueueBatch (const StrVec& links, const Options& options);

    /// Pinterest-фотография: качаем напрямую, без yt-dlp.
    void enqueuePhoto (const Str& link, const Options& options);

    void cancel (int id);
    void remove (int id);
    void clearFinished();
    std::vector<QueueItem> snapshot() const;

    /// Повтор заданий, упавших по сети (VPN мигнул): максимум два
    /// автоповтора на задание; отменённое человеком не трогается.
    int retryNetworkFailed();

    /// Сколько ссылок в тексте выглядят как ссылки — для подписи на кнопке.
    static StrVec splitLinks (const Str& text);

    /// Разбор ссылки (или названия трека) для карточки. Блокирующий —
    /// вызывать из ProbeRunner, не с UI-потока и не из очереди загрузок.
    /// Разбор ссылки/названия. searchSite — «youtube»/«soundcloud»:
    /// задаёт, где искать текстовый запрос (пусто — авто).
    Probe probe (const Str& text, const Str& searchSite = {}) const;
    Probe buildAndCache (const Str& text, const Str& searchSite) const;

    /// Фоновый разбор: старые запросы отменяются поколением, результат
    /// приезжает в onDone из потока разбора.
    void probeAsync (const Str& text, std::function<void (const Probe&)> onDone,
                     const Str& searchSite = {})
    {
        probes.start (text,
                      [this, searchSite] (const Str& t) { return probe (t, searchSite); },
                      std::move (onDone));
    }

    /// Журнал движка: без него жалоба «не качает» не диагностируема.
    static void engineLog (const Str& line);

    // ---- превью: общий дисковый кэш (кэшируется и метаданными ссылки) ----
    /// Локальный файл превью; при первом вызове скачивает (блокирующе).
    static Str cachedThumbnail (const Str& url, int timeoutMs = 8000);
    /// Фоновая подкачка превью параллельно с полным разбором ссылки.
    static void prefetchThumbnail (const Str& url);

    /// Где лежат yt-dlp и ffmpeg (App Support, бандл или дерево репозитория).
    static fs::path findToolsDir();

    // Чистка названий — утилиты, покрытые контрактными тестами.
    static Str cleanTrackName (const Str& raw);
    static Str safeName (const Str& s);

public:
    /// «0:00», «1:07», «90» -> секунды; не время — -1 (нужно и для имён
    /// фрагментов).
    static int parseTimecode (const Str& s);

private:
    void workerLoop();
    void processItem (const QueueItemPtr& item);
    void startNative (const QueueItemPtr& item);
    void startPlaylist (const QueueItemPtr& item);
    void startResolve (const QueueItemPtr& item);
    void downloadTrack (const QueueItemPtr& item, int index,
                        Detector::Service searchSite,
                        const Str& artist, const Str& track,
                        int expectedDuration = 0, bool strictMatch = false);
    void startPhotoFallback (const QueueItemPtr& item);
    QueueItemPtr findItem (int id);

    static Str humanError (const Str& raw);
    static bool retryWithCookies (const Str& raw);

    /// Запуск yt-dlp с построчным разбором вывода. false — отменено или
    /// процесс не поднялся (тогда состояние уже выставлено).
    bool runYtDlp (const QueueItemPtr& item, const StrVec& args,
                   int* exitCodeOut = nullptr);
    void consume (const Str& line, const QueueItemPtr& item);
    void finish (const QueueItemPtr& item, QueueItem::State state,
                 const Str& stage = {});
    void setStage (const QueueItemPtr& item, const Str& stage);
    void fireChanged();

    // ---- открытые данные сервисов ----
    static Str fetch (const Str& url, int timeoutMs = 15000);
    static Str between (const Str& text, const Str& start, const Str& end);
    StrVec resolveSpotify (const Str& link, Str& album,
                           int* durationSec = nullptr, Str* thumbnail = nullptr) const;
    StrVec resolveAppleMusic (const Str& link, Str& album,
                              int* durationSec = nullptr, Str* thumbnail = nullptr) const;
    /// Яндекс Музыка по каноническому ID трека из ссылки: открытый API
    /// отдаёт точное название, исполнителя и длительность.
    StrVec resolveYandexMusic (const Str& link, Str& album,
                               int* durationSec = nullptr, Str* thumbnail = nullptr) const;
    StrVec resolveOpenGraph (const Str& link) const;
    bool downloadPinterestPhoto (const QueueItemPtr& item);
    Probe probePinterestPhoto (const Str& link) const;
    Probe probeSearch (const Str& query, const Str& site = {}) const;
    Probe searchAppleMusicList (const Str& query) const;
    Probe searchSpotifyList (const Str& query) const;
    Probe searchYandexList (const Str& query) const; // (не вызывается: Яндекс выведен)
    Probe searchYandexRemoved (const Str& query) const;

    /// Сверка найденного на YouTube с тем, что записано в ссылке каталога:
    /// значимые токены названия, исполнитель (в названии или канале) и
    /// длительность.
    static bool candidateMatches (const Str& foundTitle, int foundDur,
                                  const Str& foundUploader,
                                  const Str& artist, const Str& track,
                                  int expectedDur, bool strict);

    /// Кэш разборов: тот же текст не разбирается дважды (5 минут).
    static std::map<Str, std::pair<Probe, long long>> probeCache;
    static std::mutex probeCacheMutex;
    /// Кэш превью: адрес -> локальный файл (плюс дисковый кэш в App Support).
    static std::map<Str, Str> thumbIndex;
    static std::mutex thumbMutex;
    Probe searchAppleMusic (const Str& query) const;
    Probe searchSpotify (const Str& query) const;
    Probe searchYandex (const Str& query) const;
    Probe searchPinterest (const Str& query) const;
    Probe probeDrm (const Str& link, Detector::Service service) const;

    static Str thumbCacheDir();
    static StrVec nameTokens (const Str& raw);

    // ---- аргументы yt-dlp ----
    StrVec baseArgs (const fs::path& dest, const Str& cookie) const;

    /// Длительность локального файла через ffprobe; не вышло — 0.
    double probeFileDuration (const fs::path& file) const;

    mutable std::mutex mutex;
    std::vector<QueueItemPtr> items;   // новые сверху; работаем снизу (по порядку)
    Wake wake;
    ProbeRunner probes;                // разборы карточки, вне очереди загрузок

    std::unique_ptr<ChildProcess> current; // процесс текущего задания, живёт только в рабочем потоке
    Str errTail;                           // последняя строка с ERROR текущего процесса
    Str buffer;                            // недоразобранный хвост stdout

    std::atomic<bool> quit { false };
    std::thread thread;                    // рабочий поток очереди
    int nextId = 1;
};
