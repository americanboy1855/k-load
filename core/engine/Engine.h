#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "Detector.h"

// Качать видео или музыку. У музыкальных сервисов выбора нет — там всегда
// звук, что бы ни было выбрано.
enum class MediaMode { video, audio };

// Высота кадра; best — без ограничения.
enum class VideoQuality { q720, q1080, q2160, best };

enum class AudioFormat { mp3, m4a, wav, flac };

struct QueueItem
{
    enum class State { queued, working, done, failed };

    /// Отмена читается из UI-потока и рабочего — прячем атомик в общую
    /// кучу, чтобы сам QueueItem оставался копируемым (снимки для UI).
    struct Flags { std::atomic<bool> cancelled { false }; };

    int id = 0;
    juce::String link;
    Detector::Service service = Detector::Service::unknown;

    State state = State::queued;
    float progress = 0.0f;
    /// Что происходит — человеческая строка: «Качаю 2 из 14», «Склейка…»,
    /// текст ошибки. Живёт отдельно от state, чтобы строка не ветвилась.
    juce::String stage = Str::utf8 ("В очереди");
    juce::String title;
    int itemIndex = 0, itemTotal = 1, skipped = 0;
    /// Номер в пачке, когда качается несколько ссылок сразу.
    int batchIndex = 0, batchTotal = 0;
    juce::StringArray files;

    /// Что именно качать — замораживается при постановке в очередь: чипы
    /// на экране человек может крутить, пока очередь работает.
    bool isAudio = false;
    int maxHeight = 0;                    // 0 — без ограничения («Макс.»)
    AudioFormat audioFormat = AudioFormat::mp3;
    bool wholePlaylist = false;
    juce::File dest;                      // пусто — автоматическая папка
    /// Невидимая цепочка входа: браузер по умолчанию и остальные
    /// установленные. Имена не попадают в интерфейс, только в журнал.
    juce::StringArray cookieChain;
    /// Найденному по названию треку имя файла даёт запрос, а не ютубовский
    /// «… (Official Video)».
    juce::String nameOverride;
    /// Pinterest-фотография: качаем напрямую, без yt-dlp.
    bool isPhoto = false;

    std::shared_ptr<Flags> flags = std::make_shared<Flags>();

    bool cancelled() const { return flags->cancelled.load (std::memory_order_relaxed); }
    void setCancelled() { flags->cancelled.store (true, std::memory_order_relaxed); }
};

using QueueItemPtr = std::shared_ptr<QueueItem>;

/// Что увидели по ссылке до скачивания — содержимое карточки на экране.
struct Probe
{
    bool ok = false;
    juce::String error;                    // человеческая причина, если не вышло
    juce::String link;                     // что просили
    juce::String resolved;                 // для поиска — найденный ролик
    Detector::Service service = Detector::Service::unknown;
    juce::String title, uploader, thumbnail;
    int duration = 0;
    int count = 1;                         // записей в подборке/альбоме
    std::vector<int> heights;              // доступные высоты кадра
    bool isPlaylist = false;               // это подборка/альбом
    bool hasPlaylist = false;              // в ссылке есть list= — спросим, что качать
    bool isPhoto = false;                  // Pinterest-фотография
    bool isSearch = false;                 // искали по названию
};

/// Один-единственный разбор ссылки: пока человек допечатывает, старые
/// разборы отменяются поколением (generation), а не убийством процесса.
class ProbeRunner : private juce::Thread
{
public:
    ProbeRunner() : Thread ("K Downloader probe") { startThread(); }
    ~ProbeRunner() override { wake.signal(); stopThread (8000); }

    void start (const juce::String& text,
                const std::function<Probe (const juce::String&)>& fn,
                std::function<void (const Probe&)> onDone);

private:
    void run() override;

    juce::CriticalSection lock;
    juce::String pending;
    int generation = 0;
    std::function<Probe (const juce::String&)> probeFn;
    std::function<void (const Probe&)> callback;
    juce::WaitableEvent wake;
};

/// Очередь загрузок.
///
/// Задания идут по одному, поочерёдно, в фоне (juce::Thread): параллельная
/// загрузка делит канал и делает прогресс невнятным. Аудиопоток плагина не
/// затрагивается никогда. yt-dlp и ffmpeg ищутся в App Support, в бандле
/// и в дереве репозитория (для разработки).
class Engine : public juce::ChangeBroadcaster, private juce::Thread
{
public:
    struct Options
    {
        juce::File dest;                  // пусто — автоматическая папка
        MediaMode mode = MediaMode::video;
        VideoQuality quality = VideoQuality::q1080;
        AudioFormat audioFormat = AudioFormat::mp3;
        /// Переопределение «что качать» из карточки: подборка целиком или
        /// один ролик. -1 — решает сама ссылка.
        int wholePlaylistOverride = -1;   // -1 авто, 0 нет, 1 да
        /// Имя файла для трека, найденного по названию.
        juce::String nameOverride;
    };

    Engine();
    ~Engine() override;

    /// Пачка ссылок: каждая строка — отдельное задание со своими правилами
    /// (YouTube качается сам, Spotify читается по названию, Pinterest — фото).
    void enqueueBatch (const juce::StringArray& links, const Options& options);

    /// Pinterest-фотография: качаем напрямую, без yt-dlp.
    void enqueuePhoto (const juce::String& link, const Options& options);

    void cancel (int id);
    void remove (int id);
    void clearFinished();
    std::vector<QueueItem> snapshot() const;

    /// Сколько ссылок в тексте выглядят как ссылки — для подписи на кнопке.
    static juce::StringArray splitLinks (const juce::String& text);

    /// Разбор ссылки (или названия трека) для карточки. Блокирующий —
    /// вызывать из ProbeRunner, не с UI-потока и не из очереди загрузок.
    Probe probe (const juce::String& text) const;

    /// Журнал движка: без него жалоба «не качает» не диагностируема.
    static void engineLog (const juce::String& line);

    /// Где лежат yt-dlp и ffmpeg (App Support, бандл или дерево репозитория).
    static juce::File findToolsDir();

private:
    void run() override;
    void processItem (const QueueItemPtr& item);
    void startNative (const QueueItemPtr& item);
    void startResolve (const QueueItemPtr& item);
    void downloadTrack (const QueueItemPtr& item, int index,
                        Detector::Service searchSite,
                        const juce::String& artist, const juce::String& track);
    void startPhotoFallback (const QueueItemPtr& item);
    QueueItemPtr findItem (int id);

    static juce::String humanError (const juce::String& raw);
    static bool retryWithCookies (const juce::String& raw);

    /// Запуск yt-dlp с построчным разбором вывода. false — отменено или
    /// процесс не поднялся (тогда состояние уже выставлено).
    bool runYtDlp (const QueueItemPtr& item, const juce::StringArray& args,
                   int* exitCodeOut = nullptr);
    void consume (const juce::String& line, const QueueItemPtr& item);
    void finish (const QueueItemPtr& item, QueueItem::State state,
                 const juce::String& stage = {});
    void setStage (const QueueItemPtr& item, const juce::String& stage);

    // ---- открытые данные сервисов ----
    static juce::String fetch (const juce::String& url, int timeoutMs = 15000);
    static juce::String between (const juce::String& text,
                                 const juce::String& start, const juce::String& end);
    static juce::var dig (const juce::var& root, const juce::String& path);
    juce::StringArray resolveSpotify (const juce::String& link, juce::String& album) const;
    juce::StringArray resolveAppleMusic (const juce::String& link, juce::String& album) const;
    juce::StringArray resolveOpenGraph (const juce::String& link) const;
    bool downloadPinterestPhoto (const QueueItemPtr& item);
    Probe probePinterestPhoto (const juce::String& link) const;
    Probe probeSearch (const juce::String& query) const;

    // ---- аргументы yt-dlp ----
    juce::StringArray baseArgs (const juce::File& dest, const juce::String& cookie) const;
    static juce::String cleanTrackName (const juce::String& raw);
    static juce::String safeName (const juce::String& s);

    mutable juce::CriticalSection lock;
    std::vector<QueueItemPtr> items;   // новые сверху; работаем снизу (по порядку)
    juce::WaitableEvent wake;
    ProbeRunner probes;                // разборы карточки, вне очереди загрузок

    std::unique_ptr<juce::ChildProcess> current; // процесс текущего задания, живёт только в рабочем потоке
    juce::String errTail;                  // последняя строка с ERROR текущего процесса
    juce::String buffer;                   // недоразобранный хвост stdout

    int nextId = 1;
};
