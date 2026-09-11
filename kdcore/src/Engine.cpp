#include "Engine.h"
#include "DestResolver.h"
#include "kd_http.h"
#include "kd_url.h"
#include "platform/BrowserDefault.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sstream>

// Очередь заданий, запуск yt-dlp, построчный разбор вывода, открытые данные
// сервисов (Spotify/Apple/Pinterest), невидимая цепочка входа из браузеров.
// Порт core/engine/Engine.cpp с juce-типов на std; поведение 1:1.

using json = nlohmann::json;

static const char* ytdlpBinaryName()
{
    return "yt-dlp_macos"; // Windows-вариант появится на своём этапе
}

// Папка данных приложения: ~/Library/Application Support.
static fs::path appDataRoot()
{
    return DestResolver::homeDir() / "Library" / "Application Support";
}

// Журнал движка: без него жалоба «не качает» не диагностируема.
void Engine::engineLog (const Str& line)
{
    const auto dir = appDataRoot() / "K LOAD";
    std::error_code ec;
    fs::create_directories (dir, ec);
    // Ротация: журнал свыше 1 МБ уезжает в .1 (прошлый затирается) —
    // без этого он растёт бесконечно (аудит KL-008).
    const auto log = dir / "engine.log";
    if (fs::exists (log, ec)
        && fs::file_size (log, ec) > 1024 * 1024)
        fs::rename (log, dir / "engine.log.1", ec);
    std::ofstream out (log, std::ios::app);
    if (! out.good()) return;

    char stamp[32] = {};
    std::time_t t = std::time (nullptr);
    std::tm tm {};
    localtime_r (&t, &tm);
    std::strftime (stamp, sizeof (stamp), "%Y-%m-%d %H:%M:%S", &tm);
    out << "[" << stamp << "] " << line << "\n";
}

static Str audioFormatName (AudioFormat f)
{
    return f == AudioFormat::m4a ? "m4a" : f == AudioFormat::wav ? "wav"
         : f == AudioFormat::flac ? "flac" : f == AudioFormat::ogg ? "vorbis"
         : "mp3";
}

// 136 -> «2:16» — хронометраж в человеческих сообщениях.
static Str fmtSeconds (int total)
{
    if (total <= 0) return "0:00";
    char buf[16] = {};
    std::snprintf (buf, sizeof (buf), "%d:%02d", total / 60, total % 60);
    return buf;
}

// Расширение целевого аудиофайла (ogg, а не внутреннее «vorbis»).
static Str audioExt (AudioFormat f)
{
    return f == AudioFormat::m4a ? "m4a" : f == AudioFormat::wav ? "wav"
         : f == AudioFormat::flac ? "flac" : f == AudioFormat::ogg ? "ogg"
         : "mp3";
}

// Диапазон ХРОНа для имени файла: «[00:00–00:10]», минуты с ведущими
// нулями. Не время — пусто.
static Str chronSuffix (const Str& sections)
{
    const auto parts = kd::splitTokens (sections, "-");
    if (parts.size() < 2) return {};
    const int from = Engine::parseTimecode (parts[0]);
    const int to = Engine::parseTimecode (parts[1]);
    if (from < 0 || to < 0 || to <= from) return {};
    char buf[32] = {};
    std::snprintf (buf, sizeof (buf), " [%02d:%02d\u2013%02d:%02d]",
                   from / 60, from % 60, to / 60, to % 60);
    return buf;
}

// «0:00», «1:07», «2:02:03», «90» -> секунды. Не время — -1: в ХРОНе
// не должно быть ничего, кроме таймкодов и секунд.
int Engine::parseTimecode (const Str& s)
{
    auto t = kd::trim (s);
    if (t.empty()) return -1;
    int total = 0;
    for (const auto& part : kd::splitTokens (t, ":"))
    {
        if (part.empty() || ! kd::containsOnly (part, "0123456789")) return -1;
        const int n = kd::getInt (part);
        if (n < 0 || n > 359999) return -1;
        total = total * 60 + n;
    }
    return total;
}

// Длительность локального файла через ffprobe (сек, дробью). Не вышло — 0.
double Engine::probeFileDuration (const fs::path& file) const
{
    const auto tools = findToolsDir();
    if (tools.empty() || ! kd::isFile (file)) return 0.0;
    kd::ChildProcess probe;
    if (! probe.start ({ kd::pathStr (tools / "ffprobe"), "-v", "quiet",
                         "-show_entries", "format=duration",
                         "-of", "csv=p=0", kd::pathStr (file) }))
        return 0.0;
    Str out;
    char chunk[1024];
    for (;;)
    {
        const int n = probe.read (chunk, (int) sizeof (chunk), 20);
        if (n > 0) out.append (chunk, (size_t) n);
        else if (n == 0) break;
    }
    probe.waitExitCode();
    return kd::getDouble (kd::trim (out));
}

// ---- JSON-хелперы (в оригинале — juce::var) ----

// Текст свойства как он есть: строкой; числа разворачиваются обратно.
static Str jtext (const json& v, const char* key, const Str& def = {})
{
    if (! v.is_object() || ! v.contains (key)) return def;
    const auto& x = v[key];
    if (x.is_string()) return x.get<Str>();
    if (x.is_number_integer() || x.is_number_unsigned())
        return std::to_string (x.is_number_unsigned() ? (long long) x.get<uint64_t>() : x.get<long long>());
    if (x.is_number_float())
    {
        char buf[32];
        std::snprintf (buf, sizeof (buf), "%g", x.get<double>());
        return buf;
    }
    if (x.is_boolean()) return x.get<bool>() ? "true" : "false";
    return def;
}

static double jnum (const json& v, const char* key)
{
    return kd::getDouble (jtext (v, key));
}

// Проход по пути «a.b.c» через вложенные объекты.
static json dig (const json& root, const Str& path)
{
    json cur = root;
    for (const auto& key : kd::splitTokens (path, "."))
    {
        if (! cur.is_object()) return {};
        const auto it = cur.find (key);
        cur = it == cur.end() ? json() : *it;
    }
    return cur;
}

// Все videoRenderer выдачи (для списка результатов на карточке).
static std::vector<json> findAllVideoRenderers (const json& v)
{
    std::vector<json> out;
    if (v.is_object())
    {
        const auto it = v.find ("videoRenderer");
        if (it != v.end() && it->is_object()) out.push_back (*it);
        for (auto it2 = v.begin(); it2 != v.end(); ++it2)
        {
            auto sub = findAllVideoRenderers (it2.value());
            for (auto& f : sub) out.push_back (std::move (f));
        }
    }
    else if (v.is_array())
    {
        for (const auto& item : v)
        {
            auto sub = findAllVideoRenderers (item);
            for (auto& f : sub) out.push_back (std::move (f));
        }
    }
    return out;
}

// Рекурсивный поиск первого объекта с ключом videoRenderer в дереве выдачи
// YouTube (в оригинале — std::function по juce::var).
static json findVideoRenderer (const json& v)
{
    if (v.is_object())
    {
        const auto it = v.find ("videoRenderer");
        if (it != v.end()) return *it;
        for (auto it2 = v.begin(); it2 != v.end(); ++it2)
            if (json found = findVideoRenderer (it2.value()); ! found.is_null()) return found;
    }
    else if (v.is_array())
    {
        for (const auto& item : v)
            if (json found = findVideoRenderer (item); ! found.is_null()) return found;
    }
    return {};
}

// Текст из блоков выдачи: {"runs":[{"text":…}]} или {"simpleText":…}.
static Str runsText (const json& d)
{
    if (! d.is_object()) return {};
    const auto runs = d.find ("runs");
    if (runs != d.end() && runs->is_array() && ! runs->empty())
    {
        const auto& first = (*runs)[0];
        if (first.is_object() && first.contains ("text") && first["text"].is_string())
            return first["text"].get<Str>();
    }
    return jtext (d, "simpleText");
}

// Обложка из результата поиска: поле thumbnail, последняя из thumbnails[]
// или artwork_url (SoundCloud) — что первое встретится.
static Str searchThumb (const json& e)
{
    auto t = jtext (e, "thumbnail");
    if (! t.empty()) return t;
    const auto arr = dig (e, "thumbnails");
    if (arr.is_array() && ! arr.empty())
    {
        const auto& last = arr[arr.size() - 1];
        if (last.is_object()) t = jtext (last, "url");
    }
    if (t.empty()) t = jtext (e, "artwork_url");
    return t;
}

// ---- конструирование ----

Engine::Engine()
{
    // Ограниченная параллельность: два задания одновременно — сеть и
    // интерфейс не перегружаются, новая ссылка качается, не дожидаясь
    // конца плейлиста.
    for (int w = 0; w < 2; ++w)
        workers.emplace_back ([this] { workerLoop(); });
}

Engine::~Engine()
{
    quit.store (true, std::memory_order_relaxed);
    for (size_t i = 0; i < workers.size(); ++i)
        wake.signal(); // по токену на каждого спящего воркера
    for (auto& t : workers)
        if (t.joinable()) t.join();
}

void Engine::fireChanged()
{
    if (onChange) onChange();
}

// MARK: - очередь

StrVec Engine::splitLinks (const Str& text)
{
    StrVec links;
    Str seen = "\n";
    for (const auto& token : kd::splitTokens (text, " \n\r\t,;"))
    {
        auto t = kd::trim (token);
        // Кавычки вокруг ссылки снимаем: копипаст из мессенджеров.
        while (! t.empty() && (t.front() == '"' || t.front() == '\'')) t.erase (t.begin());
        while (! t.empty() && (t.back() == '"' || t.back() == '\'')) t.pop_back();
        // Ссылка с ведущим дефисом стала бы опцией загрузчика (аудит KL-004).
        if (t.empty() || t.front() == '-' || ! Detector::looksLikeLink (t)) continue;
        // Повторы одной вставки в пачку не дублируются.
        if (kd::contains (seen, "\n" + t + "\n")) continue;
        seen += t + "\n";
        links.push_back (t);
    }
    return links;
}

void Engine::enqueueBatch (const StrVec& links, const Options& options)
{
    {
        const std::lock_guard<std::mutex> sl (mutex);
        const auto chain = CookieChain::build();
        const int total = (int) links.size();
        int index = 0;
        for (const auto& link : links)
        {
            // Страховка от мусора: задание — ссылка (или поисковый запрос
            // движка ytsearch/scsearch). Голый текст в очередь не попадает,
            // как и «ссылка» с ведущим дефисом (аудит KL-004).
            if (kd::startsWith (link, "-")
                || (! Detector::looksLikeLink (link)
                    && ! kd::startsWith (link, "ytsearch")
                    && ! kd::startsWith (link, "scsearch")))
                continue;
            auto item = std::make_shared<QueueItem>();
            item->id = nextId++;
            item->link = link;
            item->service = Detector::serviceFor (link);
            // У музыкальных сервисов выбора нет — там всегда звук.
            // Для трека, найденного по названию, режим выбирают на карточке:
            // можно взять и клип.
            item->isAudio = options.mode == MediaMode::audio
                         || Detector::needsResolve (item->service)
                         || item->service == Detector::Service::soundcloud
                         || item->service == Detector::Service::youtubeMusic;
            item->maxHeight = options.mode == MediaMode::audio ? 0
                              : options.quality == VideoQuality::q720  ? 720
                              : options.quality == VideoQuality::q1080 ? 1080
                              : options.quality == VideoQuality::q2160 ? 2160 : 0;
            item->audioFormat = options.audioFormat;
            // Из карточки: «плейлист целиком / только это видео». -1 —
            // решает сама ссылка (чистая подборка целиком, ролик внутри — нет).
            item->wholePlaylist = options.wholePlaylistOverride >= 0
                ? options.wholePlaylistOverride == 1
                : Detector::isCollection (link);
            item->cookieChain = chain;
            item->nameOverride = options.nameOverride;
            // ХРОН имеет смысл только для одиночного файла: у подборки
            // отрезок отрезал бы кусок каждой серии. У каталогов (Spotify,
            // Apple) ссылка на трек может содержать /album/ — это
            // не плейлист, отрезок действует как у всех.
            item->sections = item->wholePlaylist
                             && ! Detector::needsResolve (item->service)
                ? Str() : options.sections;
            item->playlistLimit = options.playlistLimit;
            item->container = options.container;
            item->imageFormat = options.imageFormat;
            item->durationHint = options.durationHint;
            item->forceOverwrite = options.forceOverwrite;
            item->dest = options.dest;
            item->batchIndex = total > 1 ? ++index : 0;
            item->batchTotal = total;
            items.push_back (item);
        }
    }
    wake.signal();
    fireChanged();
}

QueueItemPtr Engine::findItem (int id)
{
    const std::lock_guard<std::mutex> sl (mutex);
    for (auto& i : items)
        if (i->id == id) return i;
    return nullptr;
}

void Engine::enqueuePhoto (const Str& link, const Options& options)
{
    {
        const std::lock_guard<std::mutex> sl (mutex);
        auto item = std::make_shared<QueueItem>();
        item->id = nextId++;
        item->link = link;
        item->service = Detector::Service::pinterest;
        item->isPhoto = true;
        item->cookieChain = CookieChain::build();
        item->dest = options.dest;
        items.push_back (item);
    }
    wake.signal();
    fireChanged();
}

void Engine::cancel (int id)
{
    if (auto item = findItem (id))
    {
        bool wasPaused = false;
        {
            const std::lock_guard<std::mutex> sl (mutex);
            wasPaused = item->state == QueueItem::State::paused;
            item->setCancelled();
        }
        // Приостановленное никто не качает: воркер спит до снятия паузы,
        // отмечаем отработанным сразу, а не после возобновления.
        // finish() сам берёт мьютекс — нельзя держать его здесь
        // (std::mutex несъёмный, иначе дедлок, см. аудит).
        if (wasPaused) finish (item, QueueItem::State::failed, "Отменено");
        wake.signal(); // поднять спящий поток
        fireChanged();
    }
}

void Engine::remove (int id)
{
    {
        const std::lock_guard<std::mutex> sl (mutex);
        items.erase (std::remove_if (items.begin(), items.end(),
            [id] (const QueueItemPtr& i) { return i->id == id; }), items.end());
    }
    fireChanged();
}

void Engine::clearFinished()
{
    {
        const std::lock_guard<std::mutex> sl (mutex);
        // Очередь сначала реально отменяем: с флагом cancelled воркер её
        // не возьмёт (лента подачи пропускает отменённое), — файлы не
        // начнут качаться после очистки.
        for (auto& i : items)
            if (i->state == QueueItem::State::queued) i->setCancelled();
        // Убираем всё, кроме текущей загрузки и приостановленного:
        // завершённое, ошибки и отменённую очередь. Файлы на диске целы.
        items.erase (std::remove_if (items.begin(), items.end(),
            [] (const QueueItemPtr& i)
            { return i->state != QueueItem::State::working
                  && i->state != QueueItem::State::paused; }), // живое — остаётся
            items.end());
    }
    fireChanged();
}

int Engine::retryNetworkFailed()
{
    int n = 0;
    {
        const std::lock_guard<std::mutex> sl (mutex);
        for (auto& i : items)
        {
            if (i->state != QueueItem::State::failed) continue;
            if (i->autoRetries >= 2) continue;      // предел автоповторов
            if (i->cancelled()) continue;           // отменённое не трогаем
            // Только сетевые причины: остальным повтор не поможет.
            const auto low = kd::lower (i->stage);
            if (! kd::containsAny (low, { "сеть", "отказал",
                                          "timed out", "connection", "unreachable" }))
                continue;
            i->autoRetries += 1;
            i->state = QueueItem::State::queued;
            i->progress = 0;
            i->stage = "В очереди — повтор после VPN";
            ++n;
        }
    }
    if (n > 0)
    {
        wake.signal();
        fireChanged();
    }
    return n;
}

std::vector<QueueItem> Engine::snapshot() const
{
    const std::lock_guard<std::mutex> sl (mutex);
    std::vector<QueueItem> out;
    out.reserve (items.size());
    for (const auto& i : items) out.push_back (*i);
    return out;
}

// Изменение задания под мьютексом: его одновременно читают snapshot
// (интерфейс), другие воркеры и clearFinished (см. TSan-аудит).
// Аудит KL-011: недокачанный «хвост» после повтора с --no-part остаётся
// с финальным именем и позже считается скачанным. Убираем свежие файлы
// задания, чей stem совпадает с ожидаемым названием, если файлов нет.
static void cleanupFreshPartial (const QueueItemPtr& item)
{
    if (item->dest.empty() || item->title.empty()) return;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator (item->dest, ec))
    {
        if (! entry.is_regular_file (ec)) continue;
        if (kd::stem (entry.path()) != item->title) continue;
        struct stat st {};
        if (::stat (entry.path().string().c_str(), &st) != 0) continue;
        if (st.st_mtime >= item->startedWall - 2)
            fs::remove (entry.path(), ec);
    }
}

void Engine::finish (const QueueItemPtr& item, QueueItem::State state, const Str& stage)
{
    // Недокачанный «хвост» (--no-part после сбоя сети) не должен считаться
    // скачанным: пока файлов у задания нет — убираем свежие под его именем.
    if (state == QueueItem::State::failed
        && item->files.empty() && item->stallRetried)
        cleanupFreshPartial (item);
    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->state = state;
        if (! stage.empty()) item->stage = stage;
    }
    fireChanged();
}

void Engine::setStage (const QueueItemPtr& item, const Str& stage)
{
    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->stage = stage;
    }
    fireChanged();
}

// MARK: - рабочий поток

void Engine::workerLoop()
{
    while (! quit.load (std::memory_order_relaxed))
    {
        // Глобальная пауза: очередь не подаётся. Спим короткими срезами —
        // снятие паузы не должно зависеть от жетонов Wake: пока воркер
        // добирался сюда, сосед мог съесть предназначенный ему токен.
        if (pausedFlag.load (std::memory_order_relaxed))
        {
            if (quit.load (std::memory_order_relaxed)) break;
            wake.waitMs (200);
            continue;
        }

        QueueItemPtr next;
        {
            const std::lock_guard<std::mutex> sl (mutex);
            // Список показывается в порядке вставки, качаем сверху вниз —
            // берём первое ждущее задание.
            for (auto& i : items)
                if (i->state == QueueItem::State::queued) { next = i; break; }
        }

        if (next == nullptr)
        {
            wake.waitForever();
            continue;
        }

        // Если ждущих заданий ещё много — разбудить второго воркера
        // (Wake с notify_all, но флаг потребляет первый проснувшийся).
        bool more = false;
        {
            const std::lock_guard<std::mutex> sl (mutex);
            for (auto& i : items)
                if (i != next && i->state == QueueItem::State::queued) { more = true; break; }
        }
        if (more) wake.signal();

        processItem (next);
    }
}

void Engine::pauseItem (const QueueItemPtr& item)
{
    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->state = QueueItem::State::paused;
        item->stage = "ПРИОСТАНОВИЛ...";
    }
    fireChanged();
}

bool Engine::isPausedNow (const QueueItemPtr& item)
{
    const std::lock_guard<std::mutex> sl (mutex);
    return item->state == QueueItem::State::paused;
}

void Engine::setPaused (bool p)
{
    if (p)
    {
        pauseRequested.store (true, std::memory_order_relaxed);
        pausedFlag.store (true, std::memory_order_relaxed);
        wake.signal(); // воркер в waitForever выйдет из сна и уснёт до снятия паузы
        fireChanged();
    }
    else
    {
        pauseRequested.store (false, std::memory_order_relaxed);
        pausedFlag.store (false, std::memory_order_relaxed);
        {
            const std::lock_guard<std::mutex> sl (mutex);
            for (auto& i : items)
                if (i->state == QueueItem::State::paused)
                {
                    // yt-dlp продолжит .part с сохранённой позиции; если
                    // источник не умеет — докачает файл заново, остальные
                    // задания очереди не затрагиваются.
                    i->state = QueueItem::State::queued;
                    i->stage = "В очереди";
                }
        }
        wake.signal();
        fireChanged();
    }
}

void Engine::processItem (const QueueItemPtr& item)
{
    if (item->cancelled())
    {
        finish (item, QueueItem::State::failed, "Отменено");
        return;
    }

    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->state = QueueItem::State::working;
        item->stage = "Готовлюсь…";
        // Точка старта попытки: файлы, появившиеся раньше, чужие (см. @F).
        item->startedWall = ::time (nullptr);
    }
    fireChanged();

    // Автоматическая папка резолвится в момент старта: в приложении это
    // Загрузки; внутри всегда «K LOAD».
    if (item->dest.empty())
    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->dest = DestResolver::defaultFolder();
    }
    kd::ensureDir (item->dest);

    if (item->isPhoto) { startPhotoFallback (item); return; }
    if (Detector::needsResolve (item->service)) { startResolve (item); return; }

    startNative (item);

    // Pinterest часто оказывается фотографией, которую yt-dlp не видит.
    // Запрошено аудио — фолбэк на фотографию не имеет смысла: человек
    // не должен видеть «не удалось забрать фотографию» вместо звука.
    bool failedNow = false;
    {
        const std::lock_guard<std::mutex> sl (mutex);
        failedNow = item->state == QueueItem::State::failed;
    }
    if (failedNow
        && item->service == Detector::Service::pinterest
        && ! item->isAudio
        && item->files.empty()
        && ! item->cancelled())
    {
        startPhotoFallback (item);
    }
}

// MARK: - инструменты

fs::path Engine::findToolsDir()
{
    // 1. Указанный вручную (тесты и нестандартные установки).
    if (const char* env = ::getenv ("K_LOAD_TOOLS"))
        if (kd::isDir (fs::u8path (env))) return fs::u8path (env);
    if (const char* env = ::getenv ("K_DOWNLOADER_TOOLS")) // прежнее имя переменной
        if (kd::isDir (fs::u8path (env))) return fs::u8path (env);

    const auto hasFfmpeg = [] (const fs::path& dir)
    {
        return kd::isFile (dir / "ffmpeg") || kd::isFile (dir / "ffmpeg.exe");
    };

    // 2. Установленные инструменты (fetch-tools, .pkg).
    const auto appSupport = appDataRoot() / "K LOAD" / "tools";
    if (hasFfmpeg (appSupport)) return appSupport;
    const auto systemTools = fs::path ("/Library/Application Support/K LOAD/tools");
    if (hasFfmpeg (systemTools)) return systemTools;

    // 3. Ресурсы собственного бандла — у приложения инструменты лежат здесь.
    char exePath [4096] = {};
    uint32_t size = sizeof (exePath);
    fs::path exe;
    if (_NSGetExecutablePath (exePath, &size) == 0)
        exe = fs::u8path (exePath);
    if (! exe.empty())
    {
        const auto resources = exe.parent_path().parent_path() / "Resources";
        if (hasFfmpeg (resources)) return resources;

        // 4. Рядом с приложением.
        const auto besideApp = exe.parent_path() / "tools";
        if (hasFfmpeg (besideApp)) return besideApp;
    }

    // 5. Установленное приложение K LOAD: инструменты одни на машину,
    //    будущий плагин в DAW пользуется ими же.
    for (const auto* appPath : { "~/Applications/K LOAD.app", "/Applications/K LOAD.app" })
    {
        fs::path p (appPath);
        if (kd::startsWith (p.string(), "~"))
            p = DestResolver::homeDir() / p.string().substr (1);
        const auto res = p / "Contents" / "Resources";
        if (hasFfmpeg (res)) return res;
    }

    // 6. Дерево репозитория — разработка без установки (core/tools).
    auto dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int i = 0; i < 8; ++i)
    {
        const auto tools = dir / "core" / "tools";
        if (hasFfmpeg (tools)) return tools;
        if (hasFfmpeg (dir / "vendor")) return dir / "vendor";
        if (! dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

// MARK: - запуск yt-dlp

StrVec Engine::baseArgs (const fs::path& dest, const Str& cookie) const
{
    StrVec args {
        "--ignore-config", "--no-warnings", "--newline", "--no-colors",
        "--retries", "5", "--socket-timeout", "20",
        "--concurrent-fragments", "4", "--no-mtime", "--no-overwrites",
        "--print", "before_dl:@T|%(playlist_index|1)s|%(playlist_count|1)s|%(title)s",
        "--print", "after_move:@F|%(filepath)s",
        "-P", kd::pathStr (dest)
    };
    const auto tools = findToolsDir();
    if (! tools.empty())
    {
        args.push_back ("--ffmpeg-location");
        args.push_back (kd::pathStr (tools));
    }
    // Вход в аккаунт подбирается невидимо: пользователь не видит, чьи
    // cookies взяты, — интерфейс показывает только результат.
    if (! cookie.empty())
    {
        args.push_back ("--cookies-from-browser");
        args.push_back (cookie);
    }
    return args;
}

// PATH для дочерних процессов: наши инструменты плюс домашние и
// системные пути — yt-dlp находит deno/node для JS-челленджей YouTube.
static Str childPath (const fs::path& tools)
{
    Str path = kd::pathStr (tools) + ":/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin";
    if (const char* p = ::getenv ("PATH"))
        path += Str (":") + p;
    return path;
}

// Тихий запуск yt-dlp: весь stdout одним куском (для -J разбора).
// Тихий запуск yt-dlp: весь stdout одним куском (для -J разбора).
// maxSeconds — жёсткий потолок: зависший процесс глохнет, разбор возвращает
// неудачу (аудит KL-010: зависший -J блокировал все разборы навсегда).
static bool captureOut (const StrVec& args, Str& out, int* code = nullptr,
                        int maxSeconds = 120)
{
    const auto tools = Engine::findToolsDir();
    if (tools.empty()) return false;
    const auto tool = tools / "ytdlp" / ytdlpBinaryName();

    StrVec all;
    all.push_back (kd::pathStr (tool));
    all.insert (all.end(), args.begin(), args.end());

    kd::ChildProcess proc;
    if (! proc.start (all, childPath (tools))) return false;

    char chunk[16384];
    std::ostringstream mb;
    const auto started = std::chrono::steady_clock::now();
    for (;;)
    {
        const int n = proc.read (chunk, sizeof (chunk), 30);
        if (n > 0) mb.write (chunk, n);
        else if (n == 0) break; // поток закрыт — процесс закончил вывод
        if (std::chrono::duration_cast<std::chrono::seconds> (
                std::chrono::steady_clock::now() - started).count() > maxSeconds)
        {
            proc.kill();
            proc.waitExitCode();
            Engine::engineLog ("разбор не уложился в " + std::to_string (maxSeconds)
                               + " с — процесс остановлен");
            out = mb.str();
            if (code != nullptr) *code = -1;
            return true;
        }
    }
    out = mb.str();
    if (code != nullptr) *code = proc.waitExitCode();
    return true;
}

bool Engine::runYtDlp (const QueueItemPtr& item, const StrVec& args,
                       RunState& rs, int* exitCodeOut)
{
    const auto tools = findToolsDir();
    if (tools.empty())
    {
        finish (item, QueueItem::State::failed,
                "Загрузчик не найден — установите инструменты");
        return false;
    }
    const auto tool = tools / "ytdlp" / ytdlpBinaryName();

    // Лестница сетевых повторов: возобновление с .part, затем перекачка
    // с нуля (--no-part). Только этот незавершённый файл — очередь и
    // остальные задания не затрагиваются.
    StrVec launchArgs = args;
    for (int stallAttempt = 0;; ++stallAttempt)
    {
        StrVec all;
        all.push_back (kd::pathStr (tool));
        all.insert (all.end(), launchArgs.begin(), launchArgs.end());

        engineLog ("запуск: " + kd::join (all, " ")
                   + (stallAttempt > 0
                          ? " (повтор " + std::to_string (stallAttempt) + ")"
                          : Str()));
        rs.current = std::make_unique<kd::ChildProcess>();
        rs.errTail = {};
        rs.buffer = {};
        if (! rs.current->start (all, childPath (tools)))
        {
            engineLog ("ошибка: процесс не запустился");
            rs.current = nullptr;
            finish (item, QueueItem::State::failed, "Не удалось запустить загрузчик");
            return false;
        }

        char chunk[8192];
        bool cancelled = false;
        bool stalled = false;
        auto lastOutput = std::chrono::steady_clock::now();
        for (;;)
        {
            if (item->cancelled() || quit.load (std::memory_order_relaxed)) { cancelled = true; break; }

            // Глобальная пауза: процесс останавливается корректно (SIGTERM —
            // yt-dlp закрывает .part), задание переходит в состояние paused.
            if (pauseRequested.load (std::memory_order_relaxed))
            {
                rs.current->kill();
                pauseItem (item);
                rs.current->closeOutput(); // иначе дескриптор трубы живёт до деструктора
                rs.current = nullptr;
                return false;
            }

            const int n = rs.current->read (chunk, (int) sizeof (chunk), 40);
            // Сеть пропала, а процесс висит: 90 с без единого байта вывода —
            // глушим и уходим в лестницу повторов.
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds> (now - lastOutput).count() > 90)
            {
                engineLog ("нет вывода 90 с — глушу зависший процесс");
                rs.errTail = "ERROR: network stalled - no output for 90 seconds";
                stalled = true;
                rs.current->kill();
                break;
            }
            if (n > 0)
            {
                lastOutput = std::chrono::steady_clock::now();
                rs.buffer += Str (chunk, (size_t) n);
                int nl;
                while ((nl = kd::indexOfChar (rs.buffer, '\n')) >= 0)
                {
                    consume (rs.buffer.substr (0, (size_t) nl), item, rs.errTail);
                    rs.buffer = rs.buffer.substr ((size_t) nl + 1);
                }
            }
            else if (n == 0)
            {
                break;
            }
        }

        if (cancelled) rs.current->kill();

        // Хвост без перевода строки — тоже строка вывода.
        if (! rs.buffer.empty()) consume (rs.buffer, item, rs.errTail);
        rs.buffer = {};
        const auto code = rs.current->waitExitCode();
        rs.current->closeOutput();
        engineLog ("завершён: код " + std::to_string (code)
                   + (rs.errTail.empty() ? Str() : " | " + rs.errTail));
        rs.current = nullptr;

        if (exitCodeOut != nullptr) *exitCodeOut = code;
        if (cancelled) return ! cancelled;

        // Зависание сети: повтор этого же файла (докачка .part), затем
        // перекачка с нуля; только после — честная ошибка. Резьюм
        // фрагментного скачивания после SIGTERM бывает «залипшим» —
        // перед перекачкой с нуля убираем его недокачанные куски.
        if (stalled && stallAttempt < 2)
        {
            setStage (item, stallAttempt == 0
                ? "Соединение нестабильно — пробую ещё раз"
                : "Соединение нестабильно — качаю заново");
            if (stallAttempt == 1)
            {
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator (item->dest))
                {
                    const auto name = entry.path().filename().string();
                    if (entry.path().extension() == ".part"
                        || entry.path().extension() == ".ytdl"
                        || kd::contains (name, ".part-Frag"))
                        fs::remove (entry.path(), ec);
                }
                launchArgs.push_back ("--no-part");
                // Флаг для уборки: недокачанный файл с финальным именем
                // не должен считаться скачанным (см. finish).
                {
                    const std::lock_guard<std::mutex> sl (mutex);
                    item->stallRetried = true;
                }
            }
            continue;
        }
        return true;
    }
}

void Engine::consume (const Str& line, const QueueItemPtr& item, Str& errTail)
{
    const auto raw = kd::trimStart (kd::trimEnd (line));

    // Склейку yt-dlp объявляет обычной строкой, без нашего префикса.
    if (kd::contains (raw, "[Merger]") || kd::contains (raw, "Merging formats"))
    {
        setStage (item, "Склеиваю видео и звук…");
        return;
    }
    if (kd::contains (raw, "has already been downloaded"))
    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->skipped++;
        return;
    }
    if (kd::contains (raw, "ERROR"))
        errTail = raw;

    // Ход загрузки — обычные строки «[download]  12.3% of 9.78MiB at …».
    // Свой --progress-template вместе с --concurrent-fragments yt-dlp не
    // печатает, поэтому читаем обычные. Проценты в статус не пишем —
    // доля показывается только шкалой.
    if (kd::startsWith (raw, "[download]"))
    {
        const int pct = kd::indexOfChar (raw, '%');
        if (pct > 0)
        {
            const auto head = raw.substr (0, (size_t) pct);
            size_t start = head.size();
            while (start > 0 && kd::containsOnly (head.substr (start - 1, 1), "0123456789."))
                --start;
            const auto value = kd::getDouble (head.substr (start));
            if (value > 0.0)
            {
                {
                    const std::lock_guard<std::mutex> sl (mutex);
                    item->state = QueueItem::State::working;
                    item->progress = (float) (value < 0.0 ? 0.0 : value > 100.0 ? 100.0 : value) / 100.0f;
                }
                fireChanged();
            }
        }
        return;
    }
    if (! kd::startsWith (raw, "@")) return;

    // Наши маркеры: @T|индекс|всего|название и @F|путь файла.
    // Название и путь могут содержать «|» — служебных полей ровно три,
    // дальше берём хвост целиком (аудит: заголовок резался, путь портился).
    if (kd::startsWith (raw, "@T"))
    {
        const auto body = kd::fromFirst (raw, "@T|");
        const int p1 = kd::indexOf (body, "|");
        if (p1 < 0) return;
        const auto tail1 = body.substr ((size_t) p1 + 1);
        const int p2 = kd::indexOf (tail1, "|");
        if (p2 < 0) return;
        const auto tail2 = tail1.substr ((size_t) p2 + 1);
        const int p3 = kd::indexOf (tail2, "|");
        if (p3 < 0) return;
        const auto index = kd::getInt (tail1.substr (0, (size_t) p1));
        const auto total = kd::getInt (tail2.substr (0, (size_t) p2));
        const auto title = tail2.substr ((size_t) p3 + 1);
        {
            const std::lock_guard<std::mutex> sl (mutex);
            item->itemIndex = index;
            if (total > 0) item->itemTotal = total;
            // Название показываем сразу, с первых метаданных источника, а не
            // после скачивания. После файла (маркер @F) имя уточнится до
            // фактического.
            if (! item->nameOverride.empty())
                item->title = item->nameOverride;
            else if (item->title.empty())
                item->title = title + chronSuffix (item->sections);
            item->progress = 0;
            item->state = QueueItem::State::working;
            item->stage = item->itemTotal > 1
                ? "Качаю " + std::to_string (item->itemIndex) + " из " + std::to_string (item->itemTotal)
                : "Качаю…";
        }
        fireChanged();
    }
    else if (kd::startsWith (raw, "@F"))
    {
        // Путь — весь хвост после «@F|» (может содержать «|»).
        const auto path = kd::trim (kd::fromFirst (raw, "@F|"));
        Str accepted;
        if (! path.empty())
        {
            const auto target = fs::u8path (path);
            // Аудит: в files[] не должно попадать чужое. Принимаем только
            // абсолютный путь внутри папки назначения, записанный не раньше
            // старта этой попытки (перевод строки в названии источника
            // мог подставить в маркер любой путь).
            if (target.is_absolute())
            {
                std::error_code ec;
                const auto base = fs::weakly_canonical (item->dest, ec);
                const auto full = fs::weakly_canonical (target, ec).u8string();
                const auto bs = base.u8string();
                const bool inside = ! ec && ! bs.empty()
                    && full.size() > bs.size()
                    && full.compare (0, bs.size(), bs) == 0
                    && (bs == "/" || full[bs.size()] == '/');
                struct stat st {};
                const bool fresh = ! ec
                    && ::stat (kd::pathStr (target).c_str(), &st) == 0
                    && st.st_mtime >= item->startedWall - 2;
                if (inside && fresh)
                    accepted = full;
                else
                    engineLog ("маркер @F отклонён (чужой или старый путь): " + path);
            }
            else
            {
                engineLog ("маркер @F отклонён (путь не абсолютный): " + path);
            }
        }
        if (! accepted.empty())
        {
            const std::lock_guard<std::mutex> sl (mutex);
            item->files.push_back (accepted);
            item->progress = 1;
            // Финальное имя файла — истина: строка «в процессе» и после
            // «Готово» совпадают.
            item->title = kd::stem (fs::u8path (accepted));
        }
    }
}

// MARK: - обычные сервисы

void Engine::startPlaylist (const QueueItemPtr& item)
{
    // Плоский список роликов: быстро, без скачивания. Большому плейлисту
    // даём больше времени, но не бесконечность.
    StrVec args { "--ignore-config", "--no-warnings", "--socket-timeout", "20",
                  "--retries", "1", "--flat-playlist", "-J" };
    args.push_back (item->link);
    Str out;
    int code = -1;
    if (! captureOut (args, out, &code, 300) || code != 0 || out.empty())
    {
        finish (item, QueueItem::State::failed,
            "Не удалось прочитать плейлист — проверьте ссылку и сеть");
        return;
    }
    const auto data = json::parse (out, nullptr, false);
    if (data.is_discarded() || ! data.is_object())
    {
        finish (item, QueueItem::State::failed, "Не удалось прочитать плейлист");
        return;
    }

    const auto listTitle = jtext (data, "title", "Плейлист");
    StrVec urls;
    const auto entries = data.find ("entries");
    if (entries != data.end() && entries->is_array())
        for (const auto& e : *entries)
        {
            if (! e.is_object()) continue;
            auto u = jtext (e, "webpage_url");
            if (u.empty())
            {
                const auto id = jtext (e, "id");
                if (! id.empty()) u = "https://www.youtube.com/watch?v=" + id;
            }
            if (! u.empty()) urls.push_back (u);
            if (item->playlistLimit > 0
                && (int) urls.size() >= item->playlistLimit) break;
        }
    if (urls.empty())
    {
        finish (item, QueueItem::State::failed,
            "В плейлисте нет доступных роликов");
        return;
    }

    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->itemTotal = (int) urls.size();
    }
    const auto subdir = safeName (listTitle);

    // Только анонимные попытки: автоматические cookie браузера вызывают
    // диалог пароля ключницы — запрещено спецификацией.
    StrVec attempts { "" };

    int done = 0, failed = 0;
    Str lastError;

    for (size_t attempt = 0; attempt < attempts.size(); ++attempt)
    {
        done = 0; failed = 0;
        for (size_t i = 0; i < urls.size(); ++i)
        {
            if (item->cancelled() || quit.load (std::memory_order_relaxed))
            {
                finish (item, QueueItem::State::failed, "Отменено");
                return;
            }
            {
                const std::lock_guard<std::mutex> sl (mutex);
                item->itemIndex = (int) i + 1;
                item->itemTotal = (int) urls.size();
                item->progress = (float) i / (float) urls.size();
            }
            setStage (item, "Качаю " + std::to_string (i + 1)
                          + " из " + std::to_string (urls.size()));

            StrVec args = baseArgs (item->dest, attempts[attempt]);
            // «Скачать ещё раз»: после baseArgs — значит, старше --no-overwrites
            // не остаётся; существующий файл заменяется по завершении загрузки.
            if (item->forceOverwrite) args.push_back ("--force-overwrites");
            if (item->isAudio)
            {
                for (const auto& a : kd::splitWhitespace ("-f bestaudio/best -x"))
                    args.push_back (a);
                args.push_back ("--audio-format");
                args.push_back (audioFormatName (item->audioFormat));
                for (const auto& a : kd::splitWhitespace ("--audio-quality 0 --embed-metadata"))
                    args.push_back (a);
                if (item->audioFormat != AudioFormat::wav)
                    args.push_back ("--embed-thumbnail");
            }
            else
            {
                args.push_back ("-f");
                if (item->maxHeight > 0)
                    args.push_back ("bestvideo[height<=" + std::to_string (item->maxHeight)
                              + "]+bestaudio/best[height<=" + std::to_string (item->maxHeight) + "]/best");
                else
                    args.push_back ("bestvideo+bestaudio/best");
                args.push_back ("--merge-output-format");
                args.push_back (item->container.empty() ? Str ("mp4") : item->container);
            }

            // Папка плейлиста и нумерация файлов: 01 -, 02 -, …
            char index[8];
            std::snprintf (index, sizeof (index), "%02d", (int) i + 1);
            args.push_back ("-o");
            args.push_back (subdir + "/" + index + " - %(title).100B.%(ext)s");
            if (i == 0) args.push_back ("--"); // ссылки — только позиционные
            args.push_back (urls[i]);

            const int before = (int) item->files.size();
            RunState rs;
            runYtDlp (item, args, rs);
            if (isPausedNow (item)) return; // пауза — остальные ждут
            if (item->cancelled())
            {
                finish (item, QueueItem::State::failed, "Отменено");
                return;
            }
            if ((int) item->files.size() > before) ++done; else ++failed;
            lastError = rs.errTail;
        }
        if (done > 0) break;
        if (attempt + 1 < attempts.size()
            && retryWithCookies (lastError))
            continue;
        break;
    }

    if (done > 0)
    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->progress = 1;
        Str stage = "Готово · файлов: " + std::to_string (done);
        if (failed > 0) stage += " · пропущено: " + std::to_string (failed);
        finish (item, QueueItem::State::done, stage);
    }
    else
        finish (item, QueueItem::State::failed, humanError (lastError));
}

void Engine::startNative (const QueueItemPtr& item)
{
    // Плейлист: перечисляем ролики и качаем по одному — недоступные
    // пропускаются, а прогресс честный («Качаю 2 из 25»).
    if (item->wholePlaylist)
    {
        startPlaylist (item);
        return;
    }

    // Одна анонимная попытка: автоматические cookie браузера вызывали
    // диалог пароля ключницы — запрещено спецификацией. Контент, который
    // требует входа, честно отклоняется с объяснением.
    const StrVec attempts { "" };

    // Одноразовый повтор без вшивания обложки после её сбоя.
    bool dropThumb = false;
    Str lastErr;
    for (int attempt = 0; attempt < (int) attempts.size(); ++attempt)
    {
        if (item->cancelled() || quit.load (std::memory_order_relaxed)) break;
        if (attempt > 0)
            setStage (item, "Пробую ещё раз…");

        StrVec args = baseArgs (item->dest, attempts[attempt]);
        // «Скачать ещё раз»: перекачка с заменой (--no-overwrites из baseArgs
        // перебивается поздним флагом).
        if (item->forceOverwrite) args.push_back ("--force-overwrites");

        if (item->isAudio && item->service != Detector::Service::pinterest)
        {
            // Аудио: отдельная дорожка; если у записи её нет (например,
            // Pinterest) — звук извлекается из лучшего полного потока.
            // -x ОБЯЗАТЕЛЕН: без него файл остаётся исходным webm/m4a.
            for (const auto& a : kd::splitWhitespace ("-f bestaudio/bv*+ba/b -x")) args.push_back (a);
            // Перекачка источника при повторе: иначе вариант «wav» находил
            // уже скачанный mp3 с тем же базовым именем, конвертировал его
            // в wav и удалял исходный mp3.
            args.push_back ("--force-overwrites");
            args.push_back ("--audio-format");
            args.push_back (audioFormatName (item->audioFormat));
            for (const auto& a : kd::splitWhitespace ("--audio-quality 0 --embed-metadata")) args.push_back (a);
            // В WAV обложку не вшить: попытка заканчивается ошибкой, а рядом
            // с файлом остаются картинки.
            if (item->audioFormat != AudioFormat::wav && ! dropThumb)
                args.push_back ("--embed-thumbnail");
        }
        else if (item->service == Detector::Service::instagram
                 || item->service == Detector::Service::pinterest)
        {
            // По ссылке может лежать картинка — просить у неё высоту
            // бессмысленно. Фильтров высоты нет: качаем максимум источника.
            args.push_back ("-f");
            args.push_back ("bestvideo*+bestaudio/best");
            args.push_back ("--merge-output-format");
            args.push_back (item->container.empty() ? Str ("mp4") : item->container);
        }
        else if (item->service == Detector::Service::tiktok)
        {
            // У TikTok прогрессивные потоки (видео+звук одним файлом) —
            // берём лучший целиком, без принудительного понижения;
            // раздельные потоки — запасной путь.
            args.push_back ("-f");
            args.push_back ("b/bv*+ba");
            args.push_back ("--merge-output-format");
            args.push_back (item->container.empty() ? Str ("mp4") : item->container);
        }
        else if (item->maxHeight > 0)
        {
            // Хвостовое /best обязательно: у TikTok одна нестандартная дорожка,
            // без запасного варианта yt-dlp отвечал «Requested format is not
            // available».
            args.push_back ("-f");
            args.push_back ("bestvideo[height<=" + std::to_string (item->maxHeight)
                      + "]+bestaudio/best[height<=" + std::to_string (item->maxHeight) + "]/best");
            args.push_back ("--merge-output-format");
            args.push_back (item->container.empty() ? Str ("mp4") : item->container);
        }
        else
        {
            args.push_back ("-f");
            args.push_back ("bestvideo+bestaudio/best");
            args.push_back ("--merge-output-format");
            args.push_back (item->container.empty() ? Str ("mp4") : item->container);
        }

        if (item->wholePlaylist)
        {
            // Подборка складывается в свою папку, с нумерацией; лимит —
            // только первые N роликов, если человек задал количество.
            // Недоступные ролики пропускаем: один битый выпуск не должен
            // хоронить весь плейлист.
            args.push_back ("--ignore-errors");
            args.push_back ("--yes-playlist");
            if (item->playlistLimit > 0)
            {
                args.push_back ("--playlist-items");
                args.push_back ("1:" + std::to_string (item->playlistLimit));
            }
            args.push_back ("-o");
            args.push_back ("%(playlist_title).80B/%(playlist_index)02d - %(title).100B.%(ext)s");
        }
        else if (! item->nameOverride.empty())
        {
            // Человек искал трек по названию — файл называется треком, а не
            // «… (Official Video)». Титул в тегах тоже наш, не ютубовский.
            args.push_back ("--no-playlist");
            args.push_back ("-o");
            args.push_back (safeName (item->nameOverride) + chronSuffix (item->sections)
                          + ".%(ext)s");
            args.push_back ("--parse-metadata");
            args.push_back (safeName (item->nameOverride) + ":%(title)s");
        }
        else
        {
            // Ролик, открытый внутри плейлиста, качаем как ролик.
            // У фрагмента по ХРОНУ диапазон — часть имени файла; не-mp3
            // аудио помечается форматом, чтобы варианты не затирали друг
            // друга (источник TikTok, например, сам лежит в mp3).
            Str base = "%(title).120B";
            if (item->isAudio && item->audioFormat != AudioFormat::mp3)
                base += Str (" [") + audioExt (item->audioFormat) + "]";
            args.push_back ("--no-playlist");
            args.push_back ("-o");
            args.push_back (base + chronSuffix (item->sections) + ".%(ext)s");
        }

        // ХРОН: режем отрезок точно по кадровым границам. Только одиночный
        // файл — у подборки отрезок портил бы каждую серию.
        if (! item->sections.empty() && ! item->wholePlaylist)
        {
            // Проверка отрезка до запуска: «0:00–0:10» — это секунды, без
            // смешивания миллисекунд и таймстампов.
            const auto parts = kd::splitTokens (item->sections, "-");
            const int fromSec = parts.size() > 0 ? parseTimecode (parts[0]) : -1;
            const int toSec = parts.size() > 1 ? parseTimecode (parts[1]) : -1;
            if (fromSec < 0 || toSec < 0 || toSec <= fromSec)
            {
                finish (item, QueueItem::State::failed,
                    "Непонятный отрезок — проверьте поля ОТ и ДО");
                return;
            }
            if (item->durationHint > 0 && toSec > item->durationHint + 1)
            {
                finish (item, QueueItem::State::failed,
                    "Выбранный отрезок длиннее записи (ХРОНОМЕТРАЖ · "
                    + fmtSeconds (item->durationHint) + ")");
                return;
            }
            // Pinterest отдаёт HLS со сдвинутыми внутренними метками —
            // секции yt-dlp режет мимо (0:00–0:10 превращались в ~2 сек).
            // Такой источник качаем целиком, режем ffmpeg'ом после.
            if (item->service != Detector::Service::pinterest)
            {
                args.push_back ("--download-sections");
                args.push_back ("*" + item->sections);
                args.push_back ("--force-keyframes-at-cuts");
            }
        }

        // «--» отделяет ссылки от опций: URL из чужого плейлиста, начинающийся
        // с дефиса, не должен стать опцией загрузчика (аудит KL-004).
        args.push_back ("--");
        args.push_back (item->link);

        int code = -1;
        RunState rs;
        const bool ran = runYtDlp (item, args, rs, &code);
        lastErr = rs.errTail;
        if (isPausedNow (item)) return; // пауза
        if (item->cancelled())
        {
            finish (item, QueueItem::State::failed, "Отменено");
            return;
        }
        if (! ran) return; // состояние уже выставлено runYtDlp с настоящей причиной

        if (code == 0 || ! item->files.empty())
        {
            // Pinterest: точная обрезка ffmpeg'ом по скачанному целиком
            // файлу (см. комментарий выше про HLS и метки времени).
            if (! item->sections.empty() && ! item->wholePlaylist
                && item->service == Detector::Service::pinterest
                && ! item->files.empty())
            {
                const auto parts = kd::splitTokens (item->sections, "-");
                const int fromSec = parts.size() > 0 ? parseTimecode (parts[0]) : -1;
                const int toSec = parts.size() > 1 ? parseTimecode (parts[1]) : -1;
                const auto tools = findToolsDir();
                if (fromSec >= 0 && toSec > fromSec && ! tools.empty())
                {
                    setStage (item, "Режу отрезок…");
                    const auto src = fs::u8path (item->files.front());
                    auto cut = src; cut.replace_extension (Str (".cut.mp4"));
                    kd::ChildProcess ff;
                    const bool ran = ff.start ({ kd::pathStr (tools / "ffmpeg"),
                        "-y", "-v", "quiet", "-i", kd::pathStr (src),
                        "-ss", std::to_string (fromSec),
                        "-to", std::to_string (toSec),
                        "-c:v", "libx264", "-preset", "veryfast",
                        "-c:a", "aac", "-movflags", "+faststart",
                        kd::pathStr (cut) });
                    const int ffCode = ran ? ff.waitExitCode() : -1;
                    std::error_code ec;
                    const double got = (ran && ffCode == 0 && kd::isFile (cut))
                        ? probeFileDuration (cut) : 0.0;
                    const double want = (double) (toSec - fromSec);
                    if (got >= want * 0.6)
                    {
                        fs::remove (src, ec);
                        fs::rename (cut, src, ec);
                        if (ec) fs::copy_file (cut, src,
                            fs::copy_options::overwrite_existing, ec);
                        fs::remove (cut, ec);
                        const std::lock_guard<std::mutex> sl (mutex);
                        item->files.clear();
                        item->files.push_back (kd::pathStr (src));
                    }
                    else
                    {
                        fs::remove (cut, ec);
                        fs::remove (src, ec);
                        const std::lock_guard<std::mutex> sl (mutex);
                        item->files.clear();
                        finish (item, QueueItem::State::failed,
                            "Обрезка не удалась: источник отдал короткий поток. "
                            "Попробуйте скачать без ХРОНа");
                        return;
                    }
                }
            }
            // Проверка отрезка после скачивания: итоговый файл должен быть
            // примерно длиной с выбранный диапазон. Короткая огрызка —
            // failed с причиной; битый файл не оставляем.
            if (! item->sections.empty() && ! item->wholePlaylist
                && item->service != Detector::Service::pinterest
                && ! item->files.empty())
            {
                const auto parts = kd::splitTokens (item->sections, "-");
                const int fromSec = parts.size() > 0 ? parseTimecode (parts[0]) : -1;
                const int toSec = parts.size() > 1 ? parseTimecode (parts[1]) : -1;
            const double want = fromSec >= 0 && toSec > fromSec
                ? (double) (toSec - fromSec) : 0.0;
            const double got = probeFileDuration (fs::u8path (item->files.front()));
            // Отрезок обязан быть отрезком: источник, отдавший запись
            // целиком (или огрызок), не засчитывается (аудит KL-006).
            const double slack = want * 0.1 + 3.0;
            if (want > 0 && got > 0 && (got < want - slack || got > want + slack))
            {
                std::error_code ec;
                fs::remove (fs::u8path (item->files.front()), ec);
                {
                    const std::lock_guard<std::mutex> sl (mutex);
                    item->files.clear();
                }
                finish (item, QueueItem::State::failed,
                    "ХРОН не получился: источник отдал запись длиной "
                    + fmtSeconds ((int) (got + 0.5)) + " вместо "
                    + fmtSeconds ((int) (want + 0.5))
                    + ". В этом качестве нет точной резки — выберите другое качество или скачайте без ХРОНа");
                return;
            }
        }
            // Аудио: «Готово» только когда файл физически существует и
            // содержит звуковую дорожку (проверка длительности).
            if (item->isAudio && ! item->files.empty())
            {
                setStage (item, "Проверяю файл…");
                bool audioOk = true;
                for (const auto& f : item->files)
                    if (probeFileDuration (fs::u8path (f)) <= 0.0)
                    {
                        audioOk = false;
                        std::error_code ec;
                        fs::remove (fs::u8path (f), ec);
                    }
                if (! audioOk)
                {
                    const std::lock_guard<std::mutex> sl (mutex);
                    item->files.clear();
                    finish (item, QueueItem::State::failed,
                        "Аудио не удалось обработать — попробуйте ещё раз");
                    return;
                }
            }
            const bool already = item->files.empty() && item->skipped > 0;
            Str stage = already ? Str ("Уже скачано") : Str ("Готово");
            if (item->files.size() > 1)
                stage += " · файлов: " + std::to_string (item->files.size());
            if (item->skipped > 0)
                stage += " · уже было: " + std::to_string (item->skipped);
            {
                const std::lock_guard<std::mutex> sl (mutex);
                item->progress = 1;
            }
            // Pinterest+МУЗЫКА: у HLS-потока звука дорожка не помечена —
            // извлекаем её из скачанного видео сами.
            if (item->isAudio && item->service == Detector::Service::pinterest
                && item->files.size() == 1)
            {
                setStage (item, "Извлекаю звук…");
                const auto tools = findToolsDir();
                const auto src = fs::u8path (item->files.front());
                auto out = src;
                out.replace_extension (Str (".mp3"));
                kd::ChildProcess ff;
                const bool ran = ! tools.empty()
                    && ff.start ({ kd::pathStr (tools / "ffmpeg"),
                        "-y", "-v", "quiet", "-i", kd::pathStr (src),
                        "-vn", "-acodec", "libmp3lame", "-q:a", "0",
                        kd::pathStr (out) });
                const int ffCode = ran ? ff.waitExitCode() : -1;
                std::error_code ec;
                if (ran && ffCode == 0 && kd::isFile (out))
                {
                    fs::remove (src, ec);
                    const std::lock_guard<std::mutex> sl (mutex);
                    item->files.clear();
                    item->files.push_back (kd::pathStr (out));
                    stage = "Готово";
                }
                else
                {
                    fs::remove (out, ec);
                }
            }
            // В диспетчере фрагмент подписан именем файла с диапазоном.
            if (! item->sections.empty() && item->files.size() == 1)
            {
                const std::lock_guard<std::mutex> sl (mutex);
                item->title = kd::stem (fs::u8path (item->files.front()));
            }
            finish (item, QueueItem::State::done, stage);
            return;
        }

        // Ошибка. Открытые материалы качаются с первой попытки; для остальных
        // незаметно для человека пробуем следующий источник входа.
        // Сбой вшивания обложки (бывает на фрагментах) — повтор той же
        // попытки без обложки, загрузка не должна падать из-за картинки.
        if (item->isAudio && ! dropThumb
            && kd::contains (kd::lower (rs.errTail), "thumbnail embedding"))
        {
            dropThumb = true;
            attempt -= 1;
            engineLog ("сбой вшивания обложки — повторяю без неё");
            continue;
        }
        const bool canRetry = retryWithCookies (rs.errTail) && attempt < attempts.size() - 1;
        if (! canRetry) break;
        engineLog ("попытка без входа не удалась, перехожу к следующему источнику");
    }

    finish (item, QueueItem::State::failed, humanError (lastErr));
}

bool Engine::retryWithCookies (const Str& raw)
{
    const auto low = kd::lower (raw);
    return kd::containsAny (low, { "login", "cookie", "private", "rate-limit",
                                   "sign in", "sign-in", "403", "forbidden",
                                   "failed to decrypt", "keyring" })
        || (kd::contains (low, "age") && kd::contains (low, "restrict"));
}

Str Engine::humanError (const Str& raw)
{
    // Из потока ошибок yt-dlp человеку нужна одна понятная строка.
    const auto first = kd::upToFirst (raw, "\n");
    const auto low = kd::lower (first);

    if (kd::contains (low, "sign in to confirm")
        || kd::contains (low, "not a bot"))
        return "YouTube просит подтвердить доступ (проверка «не робот») — включите VPN и повторите";
    if (kd::containsAny (low, { "login", "cookies", "private", "rate-limit", "sign in" }))
    {
        // Источники входа в интерфейсе не называем: качаются только
        // открытые материалы. Пароль ключницы не запрашиваем.
        return "Не получилось: запись закрыта. Качаются только открытые материалы";
    }
    if (kd::contains (low, "403") || kd::contains (low, "forbidden"))
        return "Сайт отказал — попробуйте позже";
    if (kd::contains (low, "drm"))
        return "Запись защищена от скачивания";
    if (kd::contains (low, "no video formats"))
        return "По ссылке нет видео — возможно, это фото";
    if (kd::contains (low, "unavailable") || kd::contains (low, "404") || kd::contains (low, "not found"))
        return "По ссылке ничего нет: запись удалена или скрыта";
    if (kd::containsAny (low, { "unsupported url", "no suitable", "not a valid url" }))
        return "Адрес не опознан — нужна ссылка на саму запись";
    if (kd::contains (low, "requested format is not available"))
        return "В этом качестве записи нет — выберите другое";
    if (kd::contains (low, "network stalled"))
        return "Сеть недоступна — включите VPN и попробуйте снова";
    if (kd::containsAny (low, { "failed to resolve", "connection refused",
                                "connection reset", "timed out", "temporary failure",
                                "network is unreachable", "ssl", "transport error" }))
        return "Сеть недоступна — включите VPN и попробуйте снова";

    auto clean = kd::trim (kd::replaceAll (first, "ERROR: ", ""));
    if (clean.size() > 120) clean = clean.substr (0, 120);
    return clean.empty() ? Str ("Загрузка не удалась") : clean;
}

// MARK: - закрытые каталоги: Spotify, Apple, ВК

void Engine::startResolve (const QueueItemPtr& item)
{
    setStage (item, "Читаю каталог…");

    Str album;
    int durSec = 0;
    Str thumb;
    StrVec tracks;
    if      (item->service == Detector::Service::spotify)
        tracks = resolveSpotify (item->link, album, &durSec, &thumb);
    else if (item->service == Detector::Service::appleMusic)
        tracks = resolveAppleMusic (item->link, album, &durSec, &thumb);
    else
        tracks = resolveOpenGraph (item->link);

    if (tracks.empty())
    {
        finish (item, QueueItem::State::failed,
            item->service == Detector::Service::vkMusic
                ? Str ("ВК Музыка треки наружу не отдаёт — вставьте ссылку на этот трек с YouTube")
                : Str ("Не удалось прочитать, что это за трек"));
        return;
    }

    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->itemTotal = (int) tracks.size();
    }

    // Альбом складываем в отдельную папку.
    if (tracks.size() > 1 && ! album.empty())
    {
        const auto folder = item->dest / fs::u8path (safeName (album));
        kd::ensureDir (folder);
        const std::lock_guard<std::mutex> sl (mutex);
        item->dest = folder;
    }

    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        if (item->cancelled() || quit.load (std::memory_order_relaxed)) break;
        // Трек «Артист - Название»: художник уйдёт в теги, искать будем
        // по всей строке.
        const auto query = tracks[(size_t) i];
        const int sep = kd::indexOf (query, " - ");
        // Ожидаемая длительность известна для одиночного трека — по ней
        // отбраковываются кавер-версии и ускоренные «sped up»-варианты.
        const int expected = tracks.size() == 1 ? durSec : 0;
        downloadTrack (item, i, Detector::Service::youtube,
                       sep > 0 ? query.substr (0, (size_t) sep) : Str(),
                       sep > 0 ? query.substr ((size_t) sep + 3) : query,
                       expected);
    }

    if (item->cancelled())
    {
        finish (item, QueueItem::State::failed, "Отменено");
        return;
    }
    if (item->files.empty())
    {
        finish (item, QueueItem::State::failed,
            "Ничего не нашлось по названиям треков");
        return;
    }
    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->progress = 1;
    }
    // Одиночный трек — просто «Готово», без счётчиков. Фрагмент подписан
    // именем файла с диапазоном.
    if (! item->sections.empty() && item->files.size() == 1)
    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->title = kd::stem (fs::u8path (item->files.front()));
    }
    finish (item, QueueItem::State::done,
            tracks.size() > 1
                ? "Готово · треков: " + std::to_string (item->files.size())
                : Str ("Готово"));
}

// Кандидат для точной сверки трека с каталога.
struct VerifyCandidate
{
    Str title;
    Str uploader;
    Str url;
    int duration = 0;
};

// Кандидаты со страницы выдачи YouTube — HTML-разбор стабильнее плоского
// поиска yt-dlp, который сервис периодически глушит.
static std::vector<VerifyCandidate> ytSearchCandidates (const Str& query)
{
    std::vector<VerifyCandidate> out;
    const auto html = kd::http::fetch (
        "https://www.youtube.com/results?search_query=" + kd::urlEscape (query));
    const Str marker = "var ytInitialData = ";
    const int a = kd::indexOf (html, marker);
    if (a < 0) return out;
    const auto body = html.substr ((size_t) a + marker.size());
    const int b = kd::indexOf (body, ";</script>");
    if (b < 0) return out;
    const auto data = json::parse (body.substr (0, (size_t) b), nullptr, false);
    for (const auto& v : findAllVideoRenderers (data))
    {
        VerifyCandidate c;
        c.url = "https://www.youtube.com/watch?v=" + jtext (v, "videoId");
        if (c.url.size() < 30) continue;
        c.title = runsText (v["title"]);
        c.uploader = runsText (v["ownerText"]);
        for (const auto& part : kd::splitTokens (runsText (v["lengthText"]), ":"))
            c.duration = c.duration * 60 + kd::getInt (part);
        out.push_back (c);
        if (out.size() >= 5) break;
    }
    return out;
}

// Кандидаты плоским поиском yt-dlp (запасной путь).
static std::vector<VerifyCandidate> ytSearchCandidatesFlat (const Str& query)
{
    std::vector<VerifyCandidate> out;
    Str outText;
    int code = -1;
    StrVec args { "--ignore-config", "--no-warnings", "--flat-playlist", "-J",
                  "ytsearch5:" + query };
    if (! captureOut (args, outText, &code) || code != 0 || outText.empty())
        return out;
    const auto data = json::parse (outText, nullptr, false);
    const auto entries = data.is_object() ? data.find ("entries") : data.end();
    if (entries == data.end() || ! entries->is_array()) return out;
    for (const auto& e : *entries)
    {
        if (! e.is_object()) continue;
        VerifyCandidate c;
        c.url = jtext (e, "webpage_url");
        if (c.url.empty()) c.url = jtext (e, "url");
        if (c.url.empty()) continue;
        c.title = jtext (e, "title");
        c.uploader = jtext (e, "uploader");
        c.duration = (int) jnum (e, "duration");
        out.push_back (c);
        if (out.size() >= 5) break;
    }
    return out;
}

void Engine::downloadTrack (const QueueItemPtr& item, const int index,
                            const Detector::Service searchSite,
                            const Str& artist, const Str& track,
                            const int expectedDuration)
{
    const auto query = artist.empty() ? track : artist + " - " + track;
    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->itemIndex = index + 1;
        item->title = query;
        item->progress = 0;
    }
    // Одиночный трек качается без счётчиков: «Качаю…», а не «1 из 1».
    setStage (item, item->itemTotal > 1
        ? "Качаю " + std::to_string (index + 1) + " из " + std::to_string (item->itemTotal)
        : Str ("Качаю…"));

    // Общая часть: звук, теги, имя файла, хрон.
    auto makeArgs = [&]() -> StrVec
    {
        StrVec args = baseArgs (item->dest, {});
        if (item->forceOverwrite) args.push_back ("--force-overwrites");
        for (const auto& a : kd::splitWhitespace ("-f bestaudio/best -x")) args.push_back (a);
        args.push_back ("--audio-format");
        args.push_back (audioFormatName (item->audioFormat));
        for (const auto& a : kd::splitWhitespace ("--audio-quality 0 --embed-metadata --embed-thumbnail --no-playlist"))
            args.push_back (a);
        auto name = safeName (query);
        // У фрагмента по ХРОНУ диапазон — часть имени файла; не-mp3 формат
        // — тоже (иначе mp3-вариант затирался родственным контейнером).
        if (item->audioFormat != AudioFormat::mp3)
            name += Str (" [") + audioExt (item->audioFormat) + "]";
        args.push_back ("-o");
        args.push_back (name + chronSuffix (item->sections) + ".%(ext)s");
        // Теги пишем свои: иначе в файл уедет название ролика с YouTube.
        // Двоеточие делит аргумент пополам, поэтому значения чистим.
        if (! artist.empty())
        {
            args.push_back ("--parse-metadata");
            args.push_back (safeName (artist) + ":%(artist)s");
        }
        args.push_back ("--parse-metadata");
        args.push_back (safeName (track) + ":%(title)s");
        // ХРОН для трека с музыкального сервиса: тот же отрезок, что и в UI.
        if (! item->sections.empty())
        {
            args.push_back ("--download-sections");
            args.push_back ("*" + item->sections);
            args.push_back ("--force-keyframes-at-cuts");
        }
        return args;
    };

    const auto before = item->files.size();

    // 1) Точное сопоставление: кандидаты выдачи сверяем с тем, что
    //    записано в ссылке каталога. Так «sped up»-версии и каверы не
    //    обходят настоящий трек. Только анонимный поиск: cookie браузера
    //    здесь не подставляем — macOS запрашивает пароль ключницы, и
    //    человек видит чужой диалог.
    StrVec verified;
    {
        auto candidates = ytSearchCandidates (query);
        if (candidates.empty())
            candidates = ytSearchCandidatesFlat (query);
        for (const auto& c : candidates)
        {
            if (candidateMatches (c.title, c.duration, c.uploader,
                                  artist, track, expectedDuration))
            {
                verified.push_back (c.url);
                if (verified.size() >= 3) break;
            }
        }
    }

    for (const auto& url : verified)
    {
        if (item->cancelled() || quit.load (std::memory_order_relaxed))
        {
            finish (item, QueueItem::State::failed, "Отменено");
            return;
        }
        StrVec args = makeArgs();
        args.push_back ("--");
        args.push_back (url);
        RunState rs;
        if (! runYtDlp (item, args, rs)) return; // отменено/пауза — состояние выставлено
        if (item->files.size() > before) return; // точный кандидат скачан
    }

    // 2) Прежний путь: yt-dlp берёт первый из выдачи.
    if (verified.empty())
    {
        if (item->cancelled())
        {
            finish (item, QueueItem::State::failed, "Отменено");
            return;
        }
        StrVec args = makeArgs();
        for (const auto& a : kd::splitWhitespace ("--ignore-errors --max-downloads 1"))
            args.push_back (a);
        // Пять кандидатов: первый результат бывает защищённым или недоступным.
        args.push_back ("--");
        args.push_back (searchSite == Detector::Service::soundcloud
                      ? "scsearch5:" + query
                      : "ytsearch5:" + query);
        RunState rs;
        if (! runYtDlp (item, args, rs)) return; // отменено
    }

    if (item->files.size() == before
        && searchSite == Detector::Service::youtube)
    {
        // На YouTube не дался — пробуем SoundCloud: там находятся ремиксы
        // и малоизвестное.
        downloadTrack (item, index, Detector::Service::soundcloud, artist, track,
                       expectedDuration);
    }
}

// MARK: - открытые данные страниц

Str Engine::fetch (const Str& url, const int timeoutMs)
{
    return kd::http::fetch (url, timeoutMs);
}

Str Engine::between (const Str& text, const Str& start, const Str& end)
{
    const int a = kd::indexOf (text, start);
    if (a < 0) return {};
    const auto body = text.substr ((size_t) a + start.size());
    const int b = kd::indexOf (body, end);
    return b < 0 ? Str() : body.substr (0, (size_t) b);
}

// ID ролика YouTube из любой формы ссылки: watch?v=, youtu.be/, shorts/.
static Str youtubeVideoId (const Str& link)
{
    const int v = kd::indexOf (link, "v=");
    if (v >= 0)
    {
        auto tail = link.substr ((size_t) v + 2);
        const int amp = kd::indexOfChar (tail, '&');
        if (amp >= 0) tail = tail.substr (0, (size_t) amp);
        if (tail.size() >= 8 && tail.size() <= 16
            && kd::containsOnly (kd::lower (tail),
                                 "abcdefghijklmnopqrstuvwxyz0123456789-_"))
            return tail;
    }
    const auto host = parseUrl (link).host;
    if (host == "youtu.be" || kd::contains (host, "youtube"))
    {
        // Хвост пути /<id> — для youtu.be и /shorts/.
        const auto path = parseUrl (link).path;
        const int shorts = kd::indexOf (path, "/shorts/");
        if (shorts >= 0)
        {
            auto tail = path.substr ((size_t) shorts + 8);
            const int slash = kd::indexOfChar (tail, '/');
            if (slash > 0) tail = tail.substr (0, (size_t) slash);
            if (tail.size() >= 8) return tail;
        }
        if (host == "youtu.be")
        {
            auto tail = kd::upToFirst (parseUrl (link).path, "/");
            if (tail.size() >= 8) return tail;
        }
    }
    return {};
}

// ---- кэш превью ----

std::map<Str, Str> Engine::thumbIndex;
std::mutex Engine::thumbMutex;

Str Engine::thumbCacheDir()
{
    const auto dir = appDataRoot() / "K LOAD" / "cache" / "thumbs";
    kd::ensureDir (dir);
    return kd::pathStr (dir);
}

Str Engine::cachedThumbnail (const Str& url, const int timeoutMs)
{
    if (url.empty() || ! kd::startsWith (kd::lower (url), "http")) return {};
    {
        const std::lock_guard<std::mutex> lock (thumbMutex);
        const auto it = thumbIndex.find (url);
        if (it != thumbIndex.end() && kd::isFile (fs::u8path (it->second)))
            return it->second;
    }

    // Имя в кэше — хеш FNV-1a адреса: длинные подписанные адреса CDN не
    // обязаны быть именами файлов. Расширение угадываем по адресу.
    Str ext = kd::lower (kd::fromLast (kd::upToFirst (url, "?"), "."));
    if (ext.empty() || ext.size() > 5 || kd::indexOfAnyOf (ext, "/\\") >= 0)
        ext = "jpg";
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : url) { h ^= c; h *= 1099511628211ull; }
    char hex[20] = {};
    std::snprintf (hex, sizeof (hex), "%llx", (unsigned long long) h);
    const auto target = fs::u8path (thumbCacheDir()) / (Str (hex) + "." + ext);

    if (kd::isFile (target))
    {
        const std::lock_guard<std::mutex> lock (thumbMutex);
        thumbIndex[url] = kd::pathStr (target);
        return kd::pathStr (target);
    }

    const auto tmp = target.string() + ".part";
    if (kd::http::downloadToFile (url, tmp, timeoutMs))
    {
        std::error_code ec;
        fs::rename (tmp, target, ec);
        if (ec)
        {
            // Переименовать не вышло (кто-то успел раньше) — ок, файл уже там.
            fs::remove (tmp, ec);
        }
        if (kd::isFile (target))
        {
            const std::lock_guard<std::mutex> lock (thumbMutex);
            thumbIndex[url] = kd::pathStr (target);
            return kd::pathStr (target);
        }
    }
    return {};
}

void Engine::prefetchThumbnail (const Str& url)
{
    if (url.empty()) return;
    std::thread ([url] { cachedThumbnail (url, 6000); }).detach();
}

// ---- сверка найденного с тем, что в ссылке ----

StrVec Engine::nameTokens (const Str& raw)
{
    static const StrVec stop { "the", "feat", "ft", "official", "video", "audio",
        "lyrics", "lyric", "lyricvideo", "remaster", "remastered", "hd", "hq",
        "4k", "8k", "mv", "visualizer", "клип", "премьера", "музыка", "cover" };
    StrVec out;
    for (const auto& t : kd::splitTokens (kd::lower (raw),
                                          " \t-_,./\\()[]:;!?\"'«»&+"))
    {
        if (t.size() < 2) continue;
        if (kd::containsVec (stop, t)) continue;
        out.push_back (t);
    }
    return out;
}

bool Engine::candidateMatches (const Str& foundTitle, const int foundDur,
                               const Str& foundUploader,
                               const Str& artist, const Str& track,
                               const int expectedDur)
{
    const auto title = kd::lower (foundTitle);
    const auto trackToks = nameTokens (track);
    const auto artistToks = nameTokens (artist);
    if (trackToks.empty()) return false;

    // Название: значительная часть токенов трека должна присутствовать.
    int hit = 0;
    for (const auto& t : trackToks)
        if (kd::contains (title, t)) ++hit;
    const bool titleOk = hit * 10 >= (int) trackToks.size() * 6;
    if (! titleOk) return false;

    // Исполнитель: в названии ролика или в названии канала (у выдачи
    // YouTube название часто без исполнителя, канал — с ним).
    bool artistOk = true;
    if (! artistToks.empty())
    {
        const auto hay = title + " " + kd::lower (foundUploader);
        artistOk = false;
        for (const auto& t : artistToks)
            if (kd::contains (hay, t)) { artistOk = true; break; }
    }

    // Длительность: главный свидетель против «sped up» и каверов.
    bool durOk = true;
    if (expectedDur > 0)
    {
        const int diff = foundDur > expectedDur ? foundDur - expectedDur
                                                : expectedDur - foundDur;
        durOk = foundDur > 0 && diff <= 8;
    }

    return titleOk && (artistOk || durOk);
}

StrVec Engine::resolveSpotify (const Str& link, Str& album,
                               int* durationSec, Str* thumbnail) const
{
    // .../track/ID, .../album/ID, .../playlist/ID — иногда с /intl-ru/ внутри.
    Str kind, value;
    for (const auto* k : { "track", "album", "playlist" })
    {
        const Str marker = Str ("/") + k + "/";
        const int p = kd::indexOf (link, marker);
        if (p >= 0)
        {
            kind = marker.substr (1, marker.size() - 2);
            const auto tail = link.substr ((size_t) p + marker.size());
            size_t end = 0;
            while (end < tail.size()
                   && std::isalnum ((unsigned char) tail[end]))
                ++end;
            value = tail.substr (0, end);
            break;
        }
    }
    if (kind.empty()) return {};

    // У страницы-вставки есть готовый JSON со всем содержимым.
    const auto html = fetch ("https://open.spotify.com/embed/" + kind + "/" + value);
    const int open = kd::indexOf (html, "<script id=\"__NEXT_DATA__\"");
    if (open < 0) return {};
    auto bodyStart = html.find ('>', (size_t) open);
    if (bodyStart == Str::npos) return {};
    ++bodyStart;
    const int close = kd::indexOf (html, "</script>", bodyStart);
    if (close < 0) return {};

    const auto data = json::parse (html.substr (bodyStart, (size_t) close - bodyStart),
                                   nullptr, false);
    if (data.is_discarded()) return {};
    const auto entity = dig (data, "props.pageProps.state.data.entity");
    if (! entity.is_object()) return {};

    album = jtext (entity, "name");

    // Обложка: visualIdentity.image[] — массив вариантов одного изображения.
    if (thumbnail != nullptr)
    {
        const auto images = dig (entity, "visualIdentity.image");
        if (images.is_array())
            for (auto it = images.rbegin(); it != images.rend(); ++it)
                if (it->is_object() && it->contains ("url"))
                {
                    *thumbnail = jtext (*it, "url");
                    break;
                }
    }

    // Длительность: у трека своё поле (миллисекунды), у подборки — сумма.
    auto addDuration = [&] (const json& j)
    {
        if (durationSec != nullptr && j.is_object())
            *durationSec += (int) (jnum (j, "duration") / 1000.0);
    };
    if (durationSec != nullptr) *durationSec = 0;

    StrVec tracks;
    // Список треков — только когда тип сущности ЯВНО альбом/подборка.
    // У отсутствующего/нестандартного type трековая ссылка иначе
    // разворачивалась в весь альбом (счётчики «1 из 5»).
    const auto type = kd::lower (jtext (entity, "type"));
    const auto uri = kd::lower (jtext (entity, "uri"));
    const bool looksLikeTrack = type == "track"
        || (type.empty() && kd::contains (uri, "track"));
    const auto list = entity.find ("trackList");
    if (! looksLikeTrack && list != entity.end() && list->is_array())
    {
        for (const auto& t : *list)
        {
            if (! t.is_object()) continue;
            const auto tTitle = jtext (t, "title");
            const auto artist = jtext (t, "subtitle");
            if (tTitle.empty()) continue;
            addDuration (t);
            tracks.push_back (artist.empty() ? tTitle : artist + " - " + tTitle);
        }
        if (! tracks.empty()) return tracks;
    }
    // Одиночный трек: имя лежит в name, исполнители — отдельным полем.
    if (album.empty()) return {};
    addDuration (entity);
    StrVec artists;
    const auto arr = entity.find ("artists");
    if (arr != entity.end() && arr->is_array())
        for (const auto& a : *arr)
            if (a.is_object())
            {
                const auto n = jtext (a, "name");
                if (! n.empty()) artists.push_back (n);
            }
    return { artists.empty() ? album : kd::join (artists, ", ") + " - " + album };
}

StrVec Engine::resolveAppleMusic (const Str& link, Str& album,
                                  int* durationSec, Str* thumbnail) const
{
    // ?i=… — конкретный трек внутри альбома, он важнее номера альбома.
    const auto url = parseUrl (link);
    auto songId = kd::queryParam (url.query, "i");
    const bool isSong = ! songId.empty();
    if (! isSong)
    {
        // Хвост пути из цифр — номер альбома. Путь в parseUrl приведён к
        // нижнему регистру; цифры это не искажает.
        size_t end = url.path.size();
        while (end > 0 && std::isdigit ((unsigned char) url.path[end - 1])) --end;
        if (end < url.path.size()) songId = url.path.substr (end);
    }
    if (songId.empty()) return {};

    // Открытый справочник Apple: по номеру отдаёт исполнителя и название,
    // для альбома — сразу все его треки.
    const auto raw = fetch ("https://itunes.apple.com/lookup?id=" + songId
        + (isSong ? "" : "&entity=song") + "&limit=200");
    const auto parsed = json::parse (raw, nullptr, false);
    if (parsed.is_discarded()) return {};
    const auto results = dig (parsed, "results");
    if (! results.is_array() || results.empty()) return {};

    if (durationSec != nullptr) *durationSec = 0;
    StrVec tracks;
    for (const auto& r : results)
    {
        if (! r.is_object()) continue;
        if (jtext (r, "wrapperType") == "collection")
        {
            album = jtext (r, "collectionName");
            continue;
        }
        const auto trackName = jtext (r, "trackName");
        if (trackName.empty()) continue;
        const auto artist = jtext (r, "artistName");
        if (durationSec != nullptr)
            *durationSec += kd::getInt (jtext (r, "trackTimeMillis")) / 1000;
        // Обложка: 100x100 в справочнике, для карточки просим крупнее.
        if (thumbnail != nullptr && thumbnail->empty())
        {
            auto art = jtext (r, "artworkUrl100");
            if (! art.empty())
                *thumbnail = kd::replaceAll (art, "100x100", "600x600");
        }
        tracks.push_back (artist.empty() ? trackName : artist + " - " + trackName);
    }
    return tracks;
}

StrVec Engine::resolveOpenGraph (const Str& link) const
{
    // ВК Музыка наружу API не даёт. Берём то, что страница сама
    // печатает для предпросмотра ссылки: og:title.
    const auto html = fetch (link);
    if (html.empty()) return {};

    auto title = between (html, "<meta property=\"og:title\" content=\"", "\"");
    if (title.empty()) title = between (html, "<meta name=\"og:title\" content=\"", "\"");
    if (title.empty()) return {};
    title = kd::replaceAll (kd::replaceAll (kd::replaceAll (title, "&amp;", "&"), "&quot;", "\""), "&#x27;", "'");

    auto artist = between (html, "<meta property=\"og:description\" content=\"", "\"");
    // Spotify: «Артист · Альбом · Song».
    const int sepDot = kd::indexOf (artist, "\u00B7");
    const int sepBullet = kd::indexOf (artist, "\u2022");
    int sep = -1;
    if (sepDot >= 0 && sepBullet >= 0) sep = sepDot < sepBullet ? sepDot : sepBullet;
    else sep = sepDot >= 0 ? sepDot : sepBullet;
    if (sep > 0) artist = kd::trim (artist.substr (0, (size_t) sep));

    if (artist.empty() || artist == title) return { title };
    return { artist + " - " + title };
}

// MARK: - Pinterest-фото

void Engine::startPhotoFallback (const QueueItemPtr& item)
{
    if (! downloadPinterestPhoto (item))
    {
        if (! item->cancelled())
            finish (item, QueueItem::State::failed,
                    "Не удалось забрать фотографию по этой ссылке");
        else
            finish (item, QueueItem::State::failed, "Отменено");
        return;
    }
    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->progress = 1;
    }
    finish (item, QueueItem::State::done, "Готово · фотография");
}

bool Engine::downloadPinterestPhoto (const QueueItemPtr& item)
{
    setStage (item, "Забираю фотографию…");

    // yt-dlp пин с фотографией не отдаёт («No video formats found»), зато
    // у Pinterest открытый oEmbed с превью; из адреса превью получается
    // адрес оригинала — размер в пути («236x») меняется на «originals».
    const auto data = json::parse (
        fetch ("https://www.pinterest.com/oembed.json?url=" + kd::urlEscape (item->link)),
        nullptr, false);
    if (data.is_discarded()) return false;
    const auto thumb = jtext (data, "thumbnail_url");
    const auto title = jtext (data, "title");
    if (thumb.empty()) return false;

    // Путь к картинке вырезаем из сырого адреса без смены регистра: у
    // i.pinimg.com пути чувствительны к букве.
    Str rawPath;
    {
        const int schemeEnd = kd::indexOf (thumb, "://");
        const int slash = schemeEnd < 0 ? -1 : kd::indexOfChar (thumb, '/', (size_t) schemeEnd + 3);
        if (slash >= 0)
            rawPath = kd::upToFirst (thumb.substr ((size_t) slash + 1), "?");
    }
    const auto original = "https://" + parseUrl (thumb).host
                        + "/originals/" + (kd::startsWith (rawPath, "/")
                            ? rawPath.substr (1) : rawPath);

    // Если оригинала нет — вернёмся к превью, оно есть всегда.
    Str fileUrl = original;
    fs::path target;
    if (! kd::http::downloadToFile (fileUrl, item->dest / ".kd-photo"))
    {
        fileUrl = thumb;
        if (! kd::http::downloadToFile (fileUrl, item->dest / ".kd-photo"))
            return false;
    }
    target = item->dest / ".kd-photo";

    const auto pathPart = kd::upToFirst (parseUrl (fileUrl).path, "?");
    auto ext = kd::lower (kd::fromLast (pathPart, "."));
    if (ext.empty() || ext.size() > 4 || ext.find_first_of ("/\\") != Str::npos) ext = "jpg";

    // Выбранный формат изображения: jpg/png (иначе — как скачалось).
    auto wantExt = kd::lower (kd::trim (item->imageFormat));
    if (wantExt != "png" && wantExt != "jpg" && wantExt != "jpeg") wantExt.clear();
    if (! wantExt.empty()) ext = wantExt == "png" ? "png" : "jpg";

    const auto name = safeName (title.empty() ? Str ("Фотография Pinterest") : title) + "." + ext;
    const auto finalPath = item->dest / fs::u8path (name);
    std::error_code ec;
    fs::rename (target, finalPath, ec);
    if (ec)
    {
        fs::copy_file (target, finalPath, fs::copy_options::overwrite_existing, ec);
        fs::remove (target);
        if (ec) return false;
    }

    // Выбран формат PNG, а скачалось JPEG (или наоборот) — конвертируем
    // штатным sips и убираем исходник.
    const auto origExt = kd::lower (finalPath.extension().string());
    const bool alreadyWanted =
        (wantExt == "png" && origExt == ".png")
        || (wantExt != "png" && (origExt == ".jpg" || origExt == ".jpeg"));
    if (! wantExt.empty() && ! alreadyWanted)
    {
        auto converted = finalPath;
        converted.replace_extension (wantExt);
        kd::ChildProcess sips;
        if (sips.start ({ "/usr/bin/sips", "-s", "format",
                          wantExt == "png" ? "png" : "jpeg",
                          kd::pathStr (finalPath),
                          "--out", kd::pathStr (converted) }))
        {
            const int code = sips.waitExitCode();
            if (code == 0 && fs::exists (converted))
            {
                fs::remove (finalPath, ec);
                const std::lock_guard<std::mutex> sl (mutex);
                item->files.push_back (kd::pathStr (converted));
                if (! title.empty()) item->title = title;
                return true;
            }
        }
    }

    {
        const std::lock_guard<std::mutex> sl (mutex);
        item->files.push_back (kd::pathStr (finalPath));
        if (! title.empty()) item->title = title;
    }
    return true;
}

// MARK: - чистка названий

Str Engine::cleanTrackName (const Str& raw)
{
    // Убирает ютубовские приписки в скобках: «(Official Video)»,
    // «[Lyrics]». Скобки без этих слов не трогаем — там бывает нужное,
    // вроде «(feat. кто-то)».
    static const StrVec junk { "official video", "official music video",
        "official audio", "official lyric video", "lyric video", "lyrics",
        "audio only", "remaster", "remastered", "hd", "hq", "4k", "8k", "mv",
        "official", "visualizer", "клип", "премьера" };

    auto out = raw;
    for (int pass = 0; pass < 8; ++pass)
    {
        bool changed = false;
        for (const auto& bracket : { std::pair<const char*, const char*> {"(", ")"}, {"[", "]"} })
        {
            const int a = kd::indexOfChar (out, bracket.first[0]);
            if (a < 0) continue;
            const int b = kd::indexOfChar (out, bracket.second[0], (size_t) a + 1);
            if (b < 0) continue;
            const auto inside = kd::lower (out.substr ((size_t) a + 1, (size_t) (b - a - 1)));
            for (const auto& j : junk)
                if (kd::contains (inside, j))
                {
                    out = out.substr (0, (size_t) a) + " " + out.substr ((size_t) b + 1);
                    changed = true;
                    break;
                }
            if (changed) break;
        }
        if (! changed) break;
    }
    while (kd::contains (out, "  ")) out = kd::replaceAll (out, "  ", " ");
    return kd::trim (out);
}

Str Engine::safeName (const Str& s)
{
    // Имя файла без символов, на которых спотыкается файловая система, и
    // без двоеточия — оно делит аргумент --parse-metadata пополам.
    auto out = s;
    for (const char bad : { '/', ':', '%', '"', '\\', '\n', '\r', '\t' })
        out = kd::replaceChar (out, bad, ' ');
    out = kd::trim (out);
    return out.empty() ? Str ("track") : out.substr (0, 110);
}

// MARK: - предсказание имён файлов («этот файл уже скачан»)

Str Engine::ytFileName (const Str& title, int maxBytes)
{
    // Зеркало sanitize_filename yt-dlp в обычном (не restricted) режиме:
    // запрещённые символы не выбрасываются, а заменяются полноширинными
    // двойниками. Проверено офлайн-печатью имени по info-json бинаром
    // 2026.08.19: «a/b» → «a⧸b», перевод строки — в пробел, таб и прочие
    // управляющие — вон; точки и пробелы по краям сохраняются.
    Str s = title;
    if (maxBytes > 0 && (int) s.size() > maxBytes)
    {
        // %(title).NB режет сырые байты ДО санитизации; хвост разбитой
        // UTF-8 буквы не оставляем.
        int cut = maxBytes;
        while (cut > 0 && ((unsigned char) s[(size_t) cut] & 0xC0) == 0x80)
            --cut;
        s = s.substr (0, (size_t) cut);
    }
    Str out;
    out.reserve (s.size());
    for (const char ch : s)
    {
        const auto c = (unsigned char) ch;
        if (c == '\n') { out.push_back (' '); continue; }
        if (c < 32 || c == 127) continue;
        out.push_back (ch);
    }
    static const std::pair<const char*, const char*> bad[] = {
        { "/", "⧸" }, { "\\", "⧹" }, { ":", "：" }, { "*", "＊" },
        { "?", "？" }, { "\"", "＂" }, { "<", "＜" }, { ">", "＞" },
        { "|", "｜" },
    };
    for (const auto& b : bad)
        out = kd::replaceAll (out, b.first, b.second);
    return out;
}

Str Engine::predictFiles (const Str& requestJson)
{
    // Заявки приносит интерфейс — из тех же данных, что уйдут в очередь
    // (заголовок разбора, формат, ХРОН), поэтому имена совпадают байт в
    // байт с тем, что потом назовёт yt-dlp. Сеть и разбор не нужны.
    const auto data = json::parse (requestJson, nullptr, false);
    json results = json::array();
    if (data.is_discarded() || ! data.is_object()
        || ! data.contains ("files") || ! data["files"].is_array())
        return json { { "results", results } }.dump();

    int i = -1;
    for (const auto& r : data["files"])
    {
        ++i;
        const auto dir = jtext (r, "dir");
        const auto ext = jtext (r, "ext");
        const auto sections = jtext (r, "sections");
        const auto suffix = jtext (r, "suffix");
        const auto kind = jtext (r, "kind");

        Str name;
        if (kind == "literal")
        {
            // Имя задаёт приложение (поиск по названию, ролики плейлиста,
            // фотография): safeName + суффикс формата + диапазон ХРОНа.
            name = safeName (jtext (r, "name")) + suffix + chronSuffix (sections);
        }
        else if (kind == "flat")
        {
            // Плейлист внутри пачки: папка по названию подборки, файлы
            // «01 - Название» — шаблон startPlaylist.
            char index[8];
            std::snprintf (index, sizeof (index), "%02d", (int) jnum (r, "index"));
            name = safeName (jtext (r, "playlistTitle")) + "/" + index
                 + " - " + ytFileName (jtext (r, "title"), 100);
        }
        else
        {
            // Одиночная запись: имя даёт заголовок источника. У каталогов
            // (Spotify, Apple, ВК Музыка) файл называется «Артист - Трек»
            // из разбора — как в downloadTrack.
            const auto title = jtext (r, "title");
            const auto service = (Detector::Service) (int) jnum (r, "service");
            name = (Detector::needsResolve (service)
                        ? safeName (title)
                        : ytFileName (title, 120))
                 + suffix + chronSuffix (sections);
        }
        if (! ext.empty()) name += "." + ext;

        const auto target = dir.empty() ? fs::u8path (name)
                                        : fs::u8path (dir) / fs::u8path (name);
        std::error_code ec;
        const bool exists = ! dir.empty() && ! name.empty()
                            && fs::exists (target, ec);
        results.push_back ({ { "i", i },
                             { "dir", dir },
                             { "name", name },
                             { "path", kd::pathStr (target) },
                             { "exists", exists } });
    }
    return json { { "results", results } }.dump();
}

// MARK: - разбор ссылки для карточки

std::map<Str, std::pair<Probe, long long>> Engine::probeCache;
std::mutex Engine::probeCacheMutex;

Probe Engine::probe (const Str& text, const Str& searchSite) const
{
    // Кэш: тот же запрос/ссылка не разбирается повторно 5 минут.
    {
        const std::lock_guard<std::mutex> lock (probeCacheMutex);
        const auto it = probeCache.find (text + "|" + searchSite);
        if (it != probeCache.end())
        {
            const auto stamp = it->second.second;
            const auto now = std::chrono::duration_cast<std::chrono::seconds> (
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            if (now - stamp < 300)
                return it->second.first;
            probeCache.erase (it);
        }
    }

    const auto probe = buildAndCache (text, searchSite);
    // Кэшируем только удачные разборы: сетевая неудача (например, VPN
    // мигнул) не должна пять минут выдавать «ничего не нашлось». Разборы
    // без хронометража тоже не кэшимся — карточка переспросит и догрузит
    // длительность без перезапуска.
    const bool incomplete = probe.ok && probe.duration <= 0
        && ! probe.isSearch && ! probe.isPhoto && ! probe.isPlaylist;
    if (probe.ok && ! incomplete)
    {
        const std::lock_guard<std::mutex> lock (probeCacheMutex);
        if (probeCache.size() > 48) probeCache.clear(); // грубая гигиена
        probeCache[text + "|" + searchSite] = { probe,
            std::chrono::duration_cast<std::chrono::seconds> (
                std::chrono::system_clock::now().time_since_epoch())
                .count() };
    }
    return probe;
}

Probe Engine::buildAndCache (const Str& text, const Str& searchSite) const
{
    auto build = [&]() -> Probe
    {
        // Не ссылка — ищем трек по названию (сайт подсказывает карточка).
        if (! Detector::looksLikeLink (text))
            return probeSearch (text, searchSite);

        const auto link = text;
        const auto service = Detector::serviceFor (link);

        if (Detector::needsResolve (service))
        {
            Probe p;
            p.ok = true;
            p.link = link;
            p.service = service;

            Str album;
            int durSec = 0;
            Str thumb;
            auto tracks = service == Detector::Service::spotify
                              ? resolveSpotify (link, album, &durSec, &thumb)
                        : service == Detector::Service::appleMusic
                              ? resolveAppleMusic (link, album, &durSec, &thumb)
                              : resolveOpenGraph (link);
            if (tracks.empty())
            {
                p.ok = false;
                p.error = service == Detector::Service::vkMusic
                    ? Str ("ВК Музыка треки наружу не отдаёт — найдите трек на YouTube")
                    : Str ("Не удалось прочитать, что это за трек");
                return p;
            }
            p.title = ! album.empty() && tracks.size() > 1 ? album : tracks[0];
            p.uploader = tracks.size() > 1
                ? Str ("альбом") + " - " + std::to_string (tracks.size()) + Str (" треков")
                : kd::upToFirst (tracks[0], " - ");
            p.count = (int) tracks.size();
            p.isPlaylist = tracks.size() > 1;
            // Длительность и превью известны сразу: карточка не ждёт,
            // пока где-то на стороне найдутся форматы.
            p.duration = durSec;
            p.thumbnail = thumb;
            return p;
        }

        // Pinterest: по пину с фотографией yt-dlp отвечает «нет форматов»,
        // поэтому сначала пробуем его, затем карточку собираем из oEmbed.
        if (service == Detector::Service::pinterest)
        {
            Probe viaYtdlp;
            int code = -1;
            Str out;
            StrVec args { "--ignore-config", "--no-warnings", "-J", "--no-playlist" };
            args.push_back (link);
            if (captureOut (args, out, &code) && code == 0 && ! out.empty())
            {
                const auto data = json::parse (out, nullptr, false);
                if (! data.is_discarded() && ! data.is_null())
                {
                    viaYtdlp.ok = true;
                    viaYtdlp.link = link;
                    viaYtdlp.service = service;
                    viaYtdlp.title = jtext (data, "title");
                    viaYtdlp.uploader = jtext (data, "uploader");
                    viaYtdlp.thumbnail = jtext (data, "thumbnail");
                    viaYtdlp.duration = (int) jnum (data, "duration");
                }
            }
            if (viaYtdlp.ok) return viaYtdlp;
            return probePinterestPhoto (link);
        }

        // Все остальные: одним -J. Подборка разбирается плоско — большой
        // плейлист не заставит ждать минуту.
        Probe p;
        const bool flat = Detector::isCollection (link);
        // Превью YouTube известно уже из ID ролика — качаем его параллельно
        // с полным разбором, не дожидаясь списка форматов.
        if (service == Detector::Service::youtube)
        {
            const auto vid = youtubeVideoId (link);
            if (! vid.empty())
                prefetchThumbnail ("https://i.ytimg.com/vi/" + vid + "/hqdefault.jpg");
        }
        StrVec args { "--ignore-config", "--no-warnings", "--socket-timeout", "20",
                      "--retries", "1", "-J" };
        args.push_back (flat ? "--flat-playlist" : "--no-playlist");
        args.push_back ("--");
        args.push_back (link);
        int code = -1;
        Str out;
        if (! captureOut (args, out, &code) || code != 0 || out.empty())
        {
            // DRM узнаём сразу при разборе, чтобы не доводить до плашки очереди.
            if (kd::contains (kd::lower (out), "drm"))
                return probeDrm (link, service);
            p.ok = false;
            // YouTube-проверка «не робот»: не блокируем интерфейс молча —
            // объясняем и предлагаем повтор.
            if (kd::contains (kd::lower (out), "sign in to confirm")
                || kd::contains (kd::lower (out), "confirm you're not a bot"))
                p.error = "YouTube просит подтвердить доступ (проверка «не робот») — включите VPN и повторите";
            else if (Detector::cookieSensitive (service))
                p.error = "Качаются только открытые материалы: запись скрыта или требует входа";
            else
                p.error = "По ссылке ничего нет: запись удалена, скрыта или требует входа";
            return p;
        }
        const auto data = json::parse (out, nullptr, false);
        if (data.is_discarded() || data.is_null())
        {
            p.ok = false;
            p.error = "Не удалось разобрать запись";
            return p;
        }
        p.ok = true;
        p.link = link;
        p.service = service;
        p.hasPlaylist = Detector::hasPlaylist (link);
        p.title = jtext (data, "title", "Без названия");
        p.uploader = jtext (data, "uploader");
        if (p.uploader.empty())
            p.uploader = jtext (data, "channel");
        p.thumbnail = jtext (data, "thumbnail");
        p.duration = (int) jnum (data, "duration");
        // У части роликов maxresdefault просто нет (404) — превью не будет
        // вовсе. Канонический hqdefault существует всегда и уже подкачан
        // параллельно с этим разбором.
        if (service == Detector::Service::youtube)
        {
            const auto vid = youtubeVideoId (link);
            if (! vid.empty())
                p.thumbnail = "https://i.ytimg.com/vi/" + vid + "/hqdefault.jpg";
        }

        if (jtext (data, "_type") == "playlist")
        {
            p.isPlaylist = true;
            int count = kd::getInt (jtext (data, "playlist_count"));
            const auto entries = data.find ("entries");
            if (entries != data.end() && entries->is_array())
                count = count > (int) entries->size() ? count : (int) entries->size();
            p.count = count > 1 ? count : 1;
            if (p.uploader.empty()) p.uploader = "подборка";
            // Обложка подборки: thumbnails[] или первая попавшаяся у роликов.
            if (p.thumbnail.empty()) p.thumbnail = searchThumb (data);
            if (p.thumbnail.empty() && entries != data.end() && entries->is_array())
                for (const auto& e : *entries)
                    if (e.is_object())
                    {
                        p.thumbnail = searchThumb (e);
                        if (! p.thumbnail.empty()) break;
                    }
            // Общий хронометраж плейлиста — сумма длительностей роликов.
            if (entries != data.end() && entries->is_array())
                for (const auto& e : *entries)
                    if (e.is_object())
                        p.duration += (int) jnum (e, "duration");
            // Плоский список роликов: плейлист раскладывается интерфейсом
            // на отдельные задачи с собственным статусом и повтором.
            if (entries != data.end() && entries->is_array())
                for (const auto& e : *entries)
                {
                    if (! e.is_object()) continue;
                    auto u = jtext (e, "webpage_url");
                    if (u.empty()) u = jtext (e, "url");
                    if (u.empty())
                    {
                        const auto id = jtext (e, "id");
                        if (! id.empty()) u = "https://www.youtube.com/watch?v=" + id;
                    }
                    if (u.empty()) continue;
                    p.entryUrls.push_back (u);
                    p.entryTitles.push_back (jtext (e, "title"));
                }
        }
        else
        {
            // Высоты кадра — из списка форматов: 4K у ролика без 4K не показываем.
            const auto formats = data.find ("formats");
            if (formats != data.end() && formats->is_array())
                for (const auto& f : *formats)
                {
                    if (! f.is_object()) continue;
                    const int h = kd::getInt (jtext (f, "height"));
                    if (h > 0 && std::find (p.heights.begin(), p.heights.end(), h) == p.heights.end())
                        p.heights.push_back (h);
                    // Верхнеуровневая длительность бывает пуста (Instagram) —
                    // добираем из любого формата.
                    if (p.duration <= 0)
                        p.duration = (int) jnum (f, "duration");
                }
            std::sort (p.heights.begin(), p.heights.end(), std::greater<int>());
        }
        // TikTok/Instagram — единый простой вид: MP4/MP3 · МАКСИМАЛЬНОЕ.
        p.shortVideo = service == Detector::Service::tiktok
                    || service == Detector::Service::instagram;
        return p;
    };
    auto result = build();
    // Поиск уже кладёт в resolved найденный ролик; обычной ссылке хватит
    // её самой. Перезаписывать нельзя — сотрётся найденное.
    if (result.ok && result.resolved.empty()) result.resolved = result.link;
    return result;
}

Probe Engine::probePinterestPhoto (const Str& link) const
{
    Probe p;
    auto data = json::parse (
        fetch ("https://www.pinterest.com/oembed.json?url=" + kd::urlEscape (link)),
        nullptr, false);
    if (data.is_discarded()) data = json::object();
    const auto thumb = jtext (data, "thumbnail_url");
    if (thumb.empty())
    {
        p.ok = false;
        p.error = "Не удалось прочитать пин";
        return p;
    }
    p.ok = true;
    p.link = link;
    p.service = Detector::Service::pinterest;
    p.isPhoto = true;
    p.title = jtext (data, "title", "Фотография");
    p.thumbnail = thumb;
    return p;
}

// Поиск трека по названию: прямой запрос к странице результатов YouTube —
// одна загрузка вместо запуска целого процесса. yt-dlp остаётся запасным
// путём на случай, если разметка поменяется; если и он пуст — SoundCloud.
// ---- поиск по каталогам (выбранный источник в карточке) ----

// Apple Music: официальный публичный iTunes Search API — 5 результатов.
Probe Engine::searchAppleMusicList (const Str& query) const
{
    Probe p;
    p.isSearch = true;
    p.service = Detector::Service::appleMusic;
    p.link = query;

    const auto body = fetch ("https://itunes.apple.com/search?media=music&limit=5&term="
                             + kd::urlEscape (query));
    const auto data = json::parse (body, nullptr, false);
    if (data.is_discarded() || ! data.is_object()
        || kd::getInt (jtext (data, "resultCount")) < 1)
    {
        p.ok = false;
        p.error = "Apple Music недоступен из вашей сети — попробуйте другой источник";
        return p;
    }
    const auto results = data["results"];
    if (! results.is_array() || results.empty())
    {
        p.ok = false;
        p.error = "В Apple Music по этому запросу ничего не нашлось";
        return p;
    }
    for (const auto& r : results)
    {
        if (! r.is_object()) continue;
        SearchResult sr;
        sr.uploader = jtext (r, "artistName");
        sr.title = sr.uploader.empty()
            ? jtext (r, "trackName")
            : sr.uploader + " - " + jtext (r, "trackName");
        sr.url = jtext (r, "trackViewUrl");
        if (sr.url.empty()) sr.url = jtext (r, "collectionViewUrl");
        sr.duration = kd::getInt (jtext (r, "trackTimeMillis")) / 1000;
        sr.service = Detector::Service::appleMusic;
        if (sr.url.empty()) continue;
        p.results.push_back (sr);
    }
    if (p.results.empty())
    {
        p.ok = false;
        p.error = "В Apple Music по этому запросу ничего не нашлось";
        return p;
    }
    p.ok = true;
    p.resolved = p.results.front().url;
    p.title = p.results.front().title;
    p.uploader = jtext (results[0], "artistName");
    p.duration = p.results.front().duration;
    p.thumbnail = jtext (results[0], "artworkUrl100");
    return p;
}

// Spotify: анонимный токен веб-плеера + публичный поиск — 5 результатов.
// Если сеть блокирует токен — честная ошибка, а не выдуманные результаты.
Probe Engine::searchSpotifyList (const Str& query) const
{
    Probe p;
    p.isSearch = true;
    p.service = Detector::Service::spotify;
    p.link = query;

    const auto tokenBody = fetch (
        "https://open.spotify.com/get_access_token?reason=transport&productType=web_player");
    const auto tokenJson = json::parse (tokenBody, nullptr, false);
    if (tokenJson.is_discarded() || ! tokenJson.is_object()
        || ! tokenJson.contains ("accessToken"))
    {
        p.ok = false;
        p.error = "Spotify недоступен из вашей сети — попробуйте другой источник";
        return p;
    }
    const auto token = jtext (tokenJson, "accessToken");
    const auto body = kd::http::fetch (
        "https://api.spotify.com/v1/search?type=track&limit=5&q="
        + kd::urlEscape (query), 15000,
        { { "Authorization", "Bearer " + token } });
    const auto data = json::parse (body, nullptr, false);
    auto tracks = data.is_object() ? dig (data, "tracks.items") : json();
    if (! tracks.is_array() || tracks.empty())
    {
        p.ok = false;
        p.error = "В Spotify по этому запросу ничего не нашлось";
        return p;
    }
    for (const auto& t : tracks)
    {
        if (! t.is_object()) continue;
        Str artist;
        const auto artists = t.find ("artists");
        if (artists != t.end() && artists->is_array() && ! artists->empty())
        {
            const auto& a = (*artists)[0];
            if (a.is_object()) artist = jtext (a, "name");
        }
        SearchResult sr;
        sr.uploader = artist;
        sr.title = artist.empty() ? jtext (t, "name") : artist + " - " + jtext (t, "name");
        sr.url = jtext (dig (t, "external_urls"), "spotify");
        if (sr.url.empty()) sr.url = jtext (t, "uri");
        sr.duration = (int) (jnum (t, "duration_ms") / 1000);
        sr.service = Detector::Service::spotify;
        if (sr.url.empty()) continue;
        p.results.push_back (sr);
    }
    if (p.results.empty())
    {
        p.ok = false;
        p.error = "В Spotify по этому запросу ничего не нашлось";
        return p;
    }
    p.ok = true;
    p.resolved = p.results.front().url;
    p.title = p.results.front().title;
    p.duration = p.results.front().duration;
    // Обложка первого результата: album.images[] — берём последнюю (крупную).
    const auto images = dig (tracks[0], "album.images");
    if (images.is_array() && ! images.empty())
    {
        const auto& last = images[images.size() - 1];
        if (last.is_object()) p.thumbnail = jtext (last, "url");
    }
    return p;
}

Probe Engine::searchPinterest (const Str& query) const
{
    Probe p;
    p.isSearch = true;
    p.service = Detector::Service::pinterest;
    p.link = query;
    p.ok = false;
    p.error = "Pinterest закрыл автоматический поиск — вставьте ссылку на пин";
    return p;
}

// DRM-запись: скачать нельзя, но карточку собираем из открытых данных —
// у SoundCloud oEmbed работает без входа.
Probe Engine::probeDrm (const Str& link, const Detector::Service service) const
{
    Probe p;
    p.ok = false;
    p.drm = true;
    p.link = link;
    p.service = service;
    p.error = "Файл защищён DRM — скачать его невозможно";
    if (service == Detector::Service::soundcloud)
    {
        const auto meta = fetch ("https://soundcloud.com/oembed?format=json&url="
                                 + kd::urlEscape (link));
        const auto data = json::parse (meta, nullptr, false);
        if (! data.is_discarded() && data.is_object())
        {
            p.title = jtext (data, "title");
            p.thumbnail = jtext (data, "thumbnail_url");
        }
    }
    return p;
}

// SoundCloud-выдача с проверкой скачиваемости. Flat-поиск даёт кандидатов,
// но DRM/подписочные треки (сейчас — весь мейнстрим) падают только на
// полном разборе: пользователь выбирал строку и получал «не могу получить
// данные». Поэтому каждый кандидат проверяем тем же полным -J, каким ядро
// разбирает выбор строки; проверки идут параллельно, чтобы не растягивать
// поиск. В выдачу попадают только реально разбирающиеся треки.
static Probe searchSoundcloudList (const Str& query, size_t cap)
{
    Probe sc;
    sc.link = query;
    sc.isSearch = true;
    sc.service = Detector::Service::soundcloud;

    Str scOut;
    StrVec scArgs { "--ignore-config", "--no-warnings", "--flat-playlist", "-J",
                    "scsearch" + std::to_string (cap + 3) + ":" + query };
    int scCode = -1;
    if (! captureOut (scArgs, scOut, &scCode) || scCode != 0 || scOut.empty())
        return sc;

    const auto data = json::parse (scOut, nullptr, false);
    const auto entries = data.is_object() ? data.find ("entries") : data.end();
    if (entries == data.end() || ! entries->is_array())
        return sc;

    std::vector<SearchResult> candidates;
    for (const auto& e : *entries)
    {
        if (! e.is_object()) continue;
        const auto url = jtext (e, "webpage_url");
        if (url.empty()) continue;
        SearchResult r;
        r.title = jtext (e, "title", query);
        r.uploader = jtext (e, "uploader");
        r.url = url;
        r.duration = (int) jnum (e, "duration");
        r.service = Detector::Service::soundcloud;
        candidates.push_back (r);
        if (candidates.size() >= cap + 3) break;
    }

    std::vector<int> usable (candidates.size(), 0);
    std::vector<std::thread> checks;
    for (size_t i = 0; i < candidates.size(); ++i)
        checks.emplace_back ([&candidates, &usable, i]
        {
            Str out;
            int code = -1;
            usable[i] = captureOut ({ "--ignore-config", "--no-warnings", "-J",
                                      candidates[i].url }, out, &code)
                        && code == 0 && ! out.empty();
        });
    for (auto& t : checks) t.join();

    for (size_t i = 0; i < candidates.size() && sc.results.size() < cap; ++i)
        if (usable[i]) sc.results.push_back (candidates[i]);

    if (! sc.results.empty())
    {
        sc.ok = true;
        sc.resolved = sc.results.front().url;
        sc.title = sc.results.front().title;
        sc.duration = sc.results.front().duration;
    }
    return sc;
}

Probe Engine::probeSearch (const Str& query, const Str& site) const
{
    Probe p;
    p.link = query;
    p.isSearch = true;

    const auto wantYoutube = site.empty() || site == "youtube";
    const auto wantSoundcloud = site.empty() || site == "soundcloud";

    // ---- YouTube: разбор выдачи, затем запасной ytsearch ----
    Probe yt;
    yt.link = query;
    yt.isSearch = true;
    yt.service = Detector::Service::youtube;
    if (wantYoutube)
    {
        const auto html = fetch ("https://www.youtube.com/results?search_query="
                                 + kd::urlEscape (query));
        const Str marker = "var ytInitialData = ";
        const int a = kd::indexOf (html, marker);
        if (a >= 0)
        {
            const auto body = html.substr ((size_t) a + marker.size());
            const int b = kd::indexOf (body, ";</script>");
            if (b >= 0)
            {
                const auto data = json::parse (body.substr (0, (size_t) b), nullptr, false);
                const auto all = findAllVideoRenderers (data);
                if (! all.empty())
                {
                    // Список результатов: все найденные videoRenderer (до 8).
                    for (const auto& item : all)
                    {
                        const auto vid = jtext (item, "videoId");
                        if (vid.empty()) continue;
                        SearchResult sr;
                        sr.title = runsText (item["title"]);
                        if (sr.title.empty()) sr.title = query;
                        sr.uploader = runsText (item["ownerText"]);
                        sr.url = "https://www.youtube.com/watch?v=" + vid;
                        int s2 = 0;
                        for (const auto& part : kd::splitTokens (runsText (item["lengthText"]), ":"))
                            s2 = s2 * 60 + kd::getInt (part);
                        sr.duration = s2;
                        sr.service = Detector::Service::youtube;
                        yt.results.push_back (sr);
                        if (yt.results.size() >= 8) break;
                    }
                    if (! yt.results.empty())
                    {
                        const auto& v = all.front();
                        yt.ok = true;
                        yt.resolved = "https://www.youtube.com/watch?v=" + jtext (v, "videoId");
                        yt.title = runsText (v["title"]);
                        if (yt.title.empty()) yt.title = "Без названия";
                        yt.uploader = runsText (v["ownerText"]);
                        int seconds = 0;
                        for (const auto& part : kd::splitTokens (runsText (v["lengthText"]), ":"))
                            seconds = seconds * 60 + kd::getInt (part);
                        yt.duration = seconds;
                        const auto th = v.find ("thumbnail");
                        if (th != v.end() && th->is_object())
                        {
                            const auto arr = th->find ("thumbnails");
                            if (arr != th->end() && arr->is_array() && ! arr->empty())
                            {
                                const auto& last = (*arr)[arr->size() - 1];
                                if (last.is_object()) yt.thumbnail = jtext (last, "url");
                            }
                        }
                    }
                }
            }
        }

        // Запасной путь: поиск через сам yt-dlp.
        if (! yt.ok)
        {
            int code = -1;
            Str out;
            StrVec args { "--ignore-config", "--no-warnings", "--flat-playlist", "-J",
                          "ytsearch8:" + query };
            if (captureOut (args, out, &code) && code == 0 && ! out.empty())
            {
                const auto data = json::parse (out, nullptr, false);
                if (! data.is_discarded())
                {
                    const auto entries = data.find ("entries");
                    if (entries != data.end() && entries->is_array())
                    {
                        for (const auto& e : *entries)
                        {
                            if (! e.is_object()) continue;
                            // В flat-выдаче YouTube ссылка лежит в url, не в webpage_url.
                            auto url = jtext (e, "webpage_url");
                            if (url.empty()) url = jtext (e, "url");
                            if (url.empty()) continue;
                            SearchResult r;
                            r.title = jtext (e, "title", query);
                            r.uploader = jtext (e, "uploader");
                            r.url = url;
                            r.duration = (int) jnum (e, "duration");
                            r.service = Detector::Service::youtube;
                            if (yt.results.empty()) yt.thumbnail = searchThumb (e);
                            yt.results.push_back (r);
                            if (yt.results.size() >= 8) break;
                        }
                        if (! yt.results.empty())
                        {
                            yt.ok = true;
                            yt.resolved = yt.results.front().url;
                            yt.title = yt.results.front().title;
                            yt.duration = yt.results.front().duration;
                        }
                    }
                }
            }
        }
    }

    // Выбранный каталог: единственный источник, как попросили.
    if (! site.empty() && site != "youtube" && site != "soundcloud")
    {
        if (site == "apple")          p = searchAppleMusicList (query);
        else if (site == "spotify")   p = searchSpotifyList (query);
        else if (site == "pinterest") p = searchPinterest (query);
        else                          p.error = "Такой источник поиска не поддерживается";
        if (! p.ok && p.error.empty())
            p.error = "По этому названию в выбранном каталоге ничего не нашлось";
        return p;
    }

    // АВТО: агрегатор до 20 результатов из каталогов, где текстовый поиск
    // и скачивание реально работают. Кто недоступен из сети — просто без
    // своих строк, поиск не разваливается.
    if (site.empty())
    {
        p.service = Detector::Service::youtube;
        size_t total = 0;
        auto append = [&] (const Probe& src, size_t cap)
        {
            size_t added = 0;
            for (const auto& r : src.results)
            {
                if (added >= cap || total >= 20) break;
                p.results.push_back (r);
                ++added;
                ++total;
            }
        };

        if (yt.ok) append (yt, 8);

        // SoundCloud: только проверенные на скачиваемость треки (DRM и
        // недоступные отсеиваются до показа строки).
        if (wantSoundcloud)
        {
            const auto sc = searchSoundcloudList (query, 5);
            if (! sc.results.empty()) append (sc, 5);
        }

        // Apple Music: официальный публичный iTunes Search API.
        {
            const auto am = searchAppleMusicList (query);
            if (am.ok) append (am, 4);
        }
        // Spotify: анонимный токен веб-плеера; из сети без доступа — без строк.
        {
            const auto sp = searchSpotifyList (query);
            if (sp.ok) append (sp, 4);
        }

        if (p.results.empty())
        {
            p.ok = false;
            p.error = "По этому названию ничего не нашлось. Часть каталогов могла "
                      "быть недоступна из сети — попробуйте ещё раз";
            return p;
        }
        p.ok = true;
        const auto& first = p.results.front();
        p.resolved = first.url;
        p.title = first.title;
        p.duration = first.duration;
        p.service = first.service;
        if (p.service == Detector::Service::youtube)
        {
            const auto vid = youtubeVideoId (p.resolved);
            if (! vid.empty())
                p.thumbnail = "https://i.ytimg.com/vi/" + vid + "/hqdefault.jpg";
        }
        return p;
    }

    // Явный youtube/soundcloud (один источник).
    if (yt.ok)
    {
        p = yt;
    }
    else if (site == "soundcloud")
    {
        p = searchSoundcloudList (query, 5);
    }

    if (! p.ok && p.error.empty())
        p.error = "По этому названию ничего не нашлось. Попробуйте добавить исполнителя";
    return p;
}

// MARK: - ProbeRunner

ProbeRunner::ProbeRunner()
{
    thread = std::thread ([this] { run(); });
}

ProbeRunner::~ProbeRunner()
{
    {
        const std::lock_guard<std::mutex> sl (mutex);
        quitFlag.store (true, std::memory_order_relaxed);
    }
    wake.signal();
    if (thread.joinable())
        thread.join();
}

void ProbeRunner::start (const Str& text, ProbeFn fn, DoneFn onDone)
{
    {
        const std::lock_guard<std::mutex> sl (mutex);
        pending = kd::trim (text);
        probeFn = std::move (fn);
        callback = std::move (onDone);
        generation++;
    }
    wake.signal();
}

void ProbeRunner::run()
{
    for (;;)
    {
        wake.waitForever();
        if (quitFlag.load (std::memory_order_relaxed)) return;

        Str text;
        int gen = 0;
        ProbeFn fn;
        DoneFn done;
        {
            const std::lock_guard<std::mutex> sl (mutex);
            text = pending;
            gen = generation;
            fn = probeFn;
            done = callback;
        }
        if (text.empty() || fn == nullptr) continue;

        // Разбор занимает секунды; если за это время человек допечатал
        // (поколение сменилось) — результат выбрасываем.
        const auto probe = fn (text);
        {
            const std::lock_guard<std::mutex> sl (mutex);
            if (gen != generation) continue;
        }
        if (done)
            done (probe); // доставка на поток интерфейса — забота принимающей стороны
    }
}
