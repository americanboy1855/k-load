#include "Engine.h"
#include "DestResolver.h"
#include "kd_http.h"
#include "kd_url.h"
#include "platform/BrowserDefault.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mach-o/dyld.h>
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
    std::ofstream out (dir / "engine.log", std::ios::app);
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
         : f == AudioFormat::flac ? "flac" : "mp3";
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

// ---- конструирование ----

Engine::Engine()
{
    thread = std::thread ([this] { workerLoop(); });
}

Engine::~Engine()
{
    quit.store (true, std::memory_order_relaxed);
    wake.signal();
    if (thread.joinable())
        thread.join();
}

void Engine::fireChanged()
{
    if (onChange) onChange();
}

// MARK: - очередь

StrVec Engine::splitLinks (const Str& text)
{
    StrVec links;
    for (const auto& token : kd::splitTokens (text, " \n\r\t,;"))
    {
        auto t = kd::trim (token);
        // Кавычки вокруг ссылки снимаем: копипаст из мессенджеров.
        while (! t.empty() && (t.front() == '"' || t.front() == '\'')) t.erase (t.begin());
        while (! t.empty() && (t.back() == '"' || t.back() == '\'')) t.pop_back();
        if (! t.empty() && Detector::looksLikeLink (t))
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
            // движка ytsearch/scsearch). Голый текст в очередь не попадает.
            if (! Detector::looksLikeLink (link)
                && ! kd::startsWith (link, "ytsearch")
                && ! kd::startsWith (link, "scsearch"))
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
            // отрезок отрезал бы кусок каждой серии.
            item->sections = item->wholePlaylist ? Str() : options.sections;
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
        item->setCancelled();
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
        items.erase (std::remove_if (items.begin(), items.end(),
            [] (const QueueItemPtr& i)
            { return i->state != QueueItem::State::queued
                  && i->state != QueueItem::State::working; }),
            items.end());
    }
    fireChanged();
}

std::vector<QueueItem> Engine::snapshot() const
{
    const std::lock_guard<std::mutex> sl (mutex);
    std::vector<QueueItem> out;
    out.reserve (items.size());
    for (const auto& i : items) out.push_back (*i);
    return out;
}

void Engine::finish (const QueueItemPtr& item, QueueItem::State state, const Str& stage)
{
    item->state = state;
    if (! stage.empty()) item->stage = stage;
    fireChanged();
}

void Engine::setStage (const QueueItemPtr& item, const Str& stage)
{
    item->stage = stage;
    fireChanged();
}

// MARK: - рабочий поток

void Engine::workerLoop()
{
    while (! quit.load (std::memory_order_relaxed))
    {
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
        processItem (next);
    }
}

void Engine::processItem (const QueueItemPtr& item)
{
    if (item->cancelled())
    {
        finish (item, QueueItem::State::failed, "Отменено");
        return;
    }

    item->state = QueueItem::State::working;
    item->stage = "Готовлюсь…";
    fireChanged();

    // Автоматическая папка резолвится в момент старта: в приложении это
    // Загрузки; внутри всегда «K LOAD».
    if (item->dest.empty())
        item->dest = DestResolver::defaultFolder();
    kd::ensureDir (item->dest);

    if (item->isPhoto) { startPhotoFallback (item); return; }
    if (Detector::needsResolve (item->service)) { startResolve (item); return; }

    startNative (item);

    // Pinterest часто оказывается фотографией, которую yt-dlp не видит.
    if (item->state == QueueItem::State::failed
        && item->service == Detector::Service::pinterest
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

// Тихий запуск yt-dlp: весь stdout одним куском (для -J разбора).
static bool captureOut (const StrVec& args, Str& out, int* code = nullptr)
{
    const auto tools = Engine::findToolsDir();
    if (tools.empty()) return false;
    const auto tool = tools / "ytdlp" / ytdlpBinaryName();

    StrVec all;
    all.push_back (kd::pathStr (tool));
    all.insert (all.end(), args.begin(), args.end());

    kd::ChildProcess proc;
    if (! proc.start (all)) return false;

    char chunk[16384];
    std::ostringstream mb;
    for (;;)
    {
        const int n = proc.read (chunk, sizeof (chunk), 30);
        if (n > 0) mb.write (chunk, n);
        else if (n == 0) break; // поток закрыт — процесс закончил вывод
    }
    out = mb.str();
    if (code != nullptr) *code = proc.waitExitCode();
    return true;
}

bool Engine::runYtDlp (const QueueItemPtr& item, const StrVec& args, int* exitCodeOut)
{
    const auto tools = findToolsDir();
    if (tools.empty())
    {
        finish (item, QueueItem::State::failed,
                "Загрузчик не найден — установите инструменты");
        return false;
    }
    const auto tool = tools / "ytdlp" / ytdlpBinaryName();

    StrVec all;
    all.push_back (kd::pathStr (tool));
    all.insert (all.end(), args.begin(), args.end());

    engineLog ("запуск: " + kd::join (all, " "));
    current = std::make_unique<kd::ChildProcess>();
    errTail = {};
    buffer = {};
    if (! current->start (all))
    {
        engineLog ("ошибка: процесс не запустился");
        current = nullptr;
        finish (item, QueueItem::State::failed, "Не удалось запустить загрузчик");
        return false;
    }

    char chunk[8192];
    bool cancelled = false;
    for (;;)
    {
        if (item->cancelled() || quit.load (std::memory_order_relaxed)) { cancelled = true; break; }

        const int n = current->read (chunk, (int) sizeof (chunk), 40);
        if (n > 0)
        {
            buffer += Str (chunk, (size_t) n);
            int nl;
            while ((nl = kd::indexOfChar (buffer, '\n')) >= 0)
            {
                consume (buffer.substr (0, (size_t) nl), item);
                buffer = buffer.substr ((size_t) nl + 1);
            }
        }
        else if (n == 0)
        {
            break;
        }
    }

    if (cancelled) current->kill();

    // Хвост без перевода строки — тоже строка вывода.
    if (! buffer.empty()) consume (buffer, item);
    buffer = {};
    const auto code = current->waitExitCode();
    current->closeOutput();
    engineLog ("завершён: код " + std::to_string (code)
               + (errTail.empty() ? Str() : " | " + errTail));
    current = nullptr;

    if (exitCodeOut != nullptr) *exitCodeOut = code;
    return ! cancelled;
}

void Engine::consume (const Str& line, const QueueItemPtr& item)
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
        item->skipped++;
        return;
    }
    if (kd::contains (raw, "ERROR"))
        errTail = raw;

    // Ход загрузки — обычные строки «[download]  12.3% of 9.78MiB at …».
    // Свой --progress-template вместе с --concurrent-fragments yt-dlp не
    // печатает, поэтому читаем обычные.
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
                item->state = QueueItem::State::working;
                item->progress = (float) (value < 0.0 ? 0.0 : value > 100.0 ? 100.0 : value) / 100.0f;
                // Не топтать «Качаю 2 из 14»: процент прицепляем отдельным хвостом.
                const Str midDot = "\u00B7";
                if (kd::contains (item->stage, midDot))
                    item->stage = kd::trim (kd::upToFirst (item->stage, midDot));
                char pctText[32];
                std::snprintf (pctText, sizeof (pctText), "%.1f", value);
                item->stage = kd::trimEnd (item->stage) + Str (" \u00B7 ") + pctText + "%";
                fireChanged();
            }
        }
        return;
    }
    if (! kd::startsWith (raw, "@")) return;

    // Наши маркеры: @T|индекс|всего|название и @F|путь файла.
    const auto parts = kd::splitTokens (raw, "|");
    if (parts.size() >= 4 && parts[0] == "@T")
    {
        item->itemIndex = kd::getInt (parts[1]);
        if (const int n = kd::getInt (parts[2]); n > 0) item->itemTotal = n;
        if (item->itemTotal > 1) item->title = parts[3];
        item->progress = 0;
        item->state = QueueItem::State::working;
        item->stage = item->itemTotal > 1
            ? "Качаю " + std::to_string (item->itemIndex) + " из " + std::to_string (item->itemTotal)
            : "Качаю…";
        fireChanged();
    }
    else if (parts.size() >= 2 && parts[0] == "@F")
    {
        item->files.push_back (parts[1]);
        item->progress = 1;
        // Заголовок строки — имя файла, пока не пришло настоящее название.
        if (item->title.empty())
            item->title = kd::stem (fs::u8path (parts[1]));
    }
}

// MARK: - обычные сервисы

void Engine::startNative (const QueueItemPtr& item)
{
    // Порядок попыток: сначала без входа (открытые материалы качаются и так),
    // затем невидимо через браузеры — браузер по умолчанию, потом остальные.
    StrVec attempts { "" };
    for (const auto& b : item->cookieChain)
        if (! kd::containsVec (attempts, b))
            attempts.push_back (b);

    for (size_t attempt = 0; attempt < attempts.size(); ++attempt)
    {
        if (item->cancelled() || quit.load (std::memory_order_relaxed)) break;
        if (attempt > 0)
            setStage (item, "Пробую ещё раз…");

        StrVec args = baseArgs (item->dest, attempts[attempt]);

        if (item->isAudio)
        {
            for (const auto& a : kd::splitWhitespace ("-f bestaudio/best -x")) args.push_back (a);
            args.push_back ("--audio-format");
            args.push_back (audioFormatName (item->audioFormat));
            for (const auto& a : kd::splitWhitespace ("--audio-quality 0 --embed-metadata")) args.push_back (a);
            // В WAV обложку не вшить: попытка заканчивается ошибкой, а рядом
            // с файлом остаются картинки.
            if (item->audioFormat != AudioFormat::wav)
                args.push_back ("--embed-thumbnail");
        }
        else if (item->service == Detector::Service::instagram
                 || item->service == Detector::Service::pinterest)
        {
            // По ссылке может лежать картинка — просить у неё высоту бессмысленно.
            args.push_back ("-f");
            args.push_back ("bestvideo*+bestaudio/best");
        }
        else if (item->maxHeight > 0)
        {
            // Хвостовое /best обязательно: у TikTok одна нестандартная дорожка,
            // без запасного варианта yt-dlp отвечал «Requested format is not
            // available».
            args.push_back ("-f");
            args.push_back ("bestvideo[height<=" + std::to_string (item->maxHeight)
                      + "]+bestaudio/best[height<=" + std::to_string (item->maxHeight) + "]/best");
            for (const auto& a : kd::splitWhitespace ("--merge-output-format mp4")) args.push_back (a);
        }
        else
        {
            args.push_back ("-f");
            args.push_back ("bestvideo+bestaudio/best");
            for (const auto& a : kd::splitWhitespace ("--merge-output-format mp4")) args.push_back (a);
        }

        if (item->wholePlaylist)
        {
            // Подборка целиком складывается в свою папку, с нумерацией.
            args.push_back ("--yes-playlist");
            args.push_back ("-o");
            args.push_back ("%(playlist_title).80B/%(playlist_index)02d - %(title).100B.%(ext)s");
        }
        else if (! item->nameOverride.empty())
        {
            // Человек искал трек по названию — файл называется треком, а не
            // «… (Official Video)». Титул в тегах тоже наш, не ютубовский.
            args.push_back ("--no-playlist");
            args.push_back ("-o");
            args.push_back (safeName (item->nameOverride) + ".%(ext)s");
            args.push_back ("--parse-metadata");
            args.push_back (safeName (item->nameOverride) + ":%(title)s");
        }
        else
        {
            // Ролик, открытый внутри плейлиста, качаем как ролик.
            args.push_back ("--no-playlist");
            args.push_back ("-o");
            args.push_back ("%(title).120B.%(ext)s");
        }

        // ХРОН: режем отрезок точно по кадровым границам. Только одиночный
        // файл — у подборки отрезок портил бы каждую серию.
        if (! item->sections.empty() && ! item->wholePlaylist)
        {
            args.push_back ("--download-sections");
            args.push_back ("*" + item->sections);
            args.push_back ("--force-keyframes-at-cuts");
        }

        args.push_back (item->link);

        int code = -1;
        const bool ran = runYtDlp (item, args, &code);
        if (item->cancelled())
        {
            finish (item, QueueItem::State::failed, "Отменено");
            return;
        }
        if (! ran) return; // состояние уже выставлено runYtDlp с настоящей причиной

        if (code == 0 || ! item->files.empty())
        {
            const bool already = item->files.empty() && item->skipped > 0;
            Str stage = already ? Str ("Уже скачано") : Str ("Готово");
            if (item->files.size() > 1)
                stage += " · файлов: " + std::to_string (item->files.size());
            if (item->skipped > 0)
                stage += " · уже было: " + std::to_string (item->skipped);
            item->progress = 1;
            finish (item, QueueItem::State::done, stage);
            return;
        }

        // Ошибка. Открытые материалы качаются с первой попытки; для остальных
        // незаметно для человека пробуем следующий источник входа.
        const bool canRetry = retryWithCookies (errTail) && attempt < attempts.size() - 1;
        if (! canRetry) break;
        engineLog ("попытка без входа не удалась, перехожу к следующему источнику");
    }

    finish (item, QueueItem::State::failed, humanError (errTail));
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

    if (kd::containsAny (low, { "login", "cookies", "private", "rate-limit", "sign in" }))
    {
        // Источники входа в интерфейсе не называем: качаются только
        // открытые материалы.
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

    auto clean = kd::trim (kd::replaceAll (first, "ERROR: ", ""));
    if (clean.size() > 120) clean = clean.substr (0, 120);
    return clean.empty() ? Str ("Загрузка не удалась") : clean;
}

// MARK: - закрытые каталоги: Spotify, Apple, Яндекс, ВК

void Engine::startResolve (const QueueItemPtr& item)
{
    setStage (item, "Читаю каталог…");

    Str album;
    StrVec tracks;
    if      (item->service == Detector::Service::spotify)    tracks = resolveSpotify (item->link, album);
    else if (item->service == Detector::Service::appleMusic) tracks = resolveAppleMusic (item->link, album);
    else                                                     tracks = resolveOpenGraph (item->link);

    if (tracks.empty())
    {
        finish (item, QueueItem::State::failed,
            item->service == Detector::Service::vkMusic
                ? Str ("ВК Музыка треки наружу не отдаёт — вставьте ссылку на этот трек с YouTube")
                : Str ("Не удалось прочитать, что это за трек"));
        return;
    }

    item->itemTotal = (int) tracks.size();

    // Альбом складываем в отдельную папку.
    if (tracks.size() > 1 && ! album.empty())
    {
        const auto folder = item->dest / fs::u8path (safeName (album));
        kd::ensureDir (folder);
        item->dest = folder;
    }

    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        if (item->cancelled() || quit.load (std::memory_order_relaxed)) break;
        // Трек «Артист - Название»: художник уйдёт в теги, искать будем
        // по всей строке.
        const auto query = tracks[(size_t) i];
        const int sep = kd::indexOf (query, " - ");
        downloadTrack (item, i, Detector::Service::youtube,
                       sep > 0 ? query.substr (0, (size_t) sep) : Str(),
                       sep > 0 ? query.substr ((size_t) sep + 3) : query);
    }

    if (item->cancelled())
    {
        finish (item, QueueItem::State::failed, "Отменено");
        return;
    }
    if (item->files.empty())
    {
        finish (item, QueueItem::State::failed, "Ничего не нашлось по названиям треков");
        return;
    }
    item->progress = 1;
    finish (item, QueueItem::State::done,
            "Готово · треков: " + std::to_string (item->files.size()));
}

void Engine::downloadTrack (const QueueItemPtr& item, const int index,
                            const Detector::Service searchSite,
                            const Str& artist, const Str& track)
{
    const auto query = artist.empty() ? track : artist + " - " + track;
    item->itemIndex = index + 1;
    item->title = query;
    item->progress = 0;
    setStage (item, "Качаю " + std::to_string (index + 1)
                  + " из " + std::to_string (item->itemTotal));

    StrVec args = baseArgs (item->dest, {});
    for (const auto& a : kd::splitWhitespace ("-f bestaudio/best -x")) args.push_back (a);
    args.push_back ("--audio-format");
    args.push_back (audioFormatName (item->audioFormat));
    for (const auto& a : kd::splitWhitespace ("--audio-quality 0 --embed-metadata --embed-thumbnail --no-playlist"))
        args.push_back (a);
    const auto name = safeName (query);
    args.push_back ("-o");
    args.push_back (name + ".%(ext)s");
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
    for (const auto& a : kd::splitWhitespace ("--ignore-errors --max-downloads 1")) args.push_back (a);
    // Пять кандидатов: первый результат бывает защищённым или недоступным.
    args.push_back (searchSite == Detector::Service::soundcloud
                  ? "scsearch5:" + query
                  : "ytsearch5:" + query);

    const auto before = item->files.size();
    if (! runYtDlp (item, args)) return; // отменено — состояние уже выставлено
    if (item->files.size() == before && searchSite == Detector::Service::youtube)
    {
        // На YouTube не дался — пробуем SoundCloud: там находятся ремиксы
        // и малоизвестное.
        downloadTrack (item, index, Detector::Service::soundcloud, artist, track);
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

StrVec Engine::resolveSpotify (const Str& link, Str& album) const
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

    StrVec tracks;
    // Альбом и подборка: список лежит целиком.
    const auto list = entity.find ("trackList");
    if (list != entity.end() && list->is_array())
    {
        for (const auto& t : *list)
        {
            if (! t.is_object()) continue;
            const auto tTitle = jtext (t, "title");
            const auto artist = jtext (t, "subtitle");
            if (! tTitle.empty())
                tracks.push_back (artist.empty() ? tTitle : artist + " - " + tTitle);
        }
        if (! tracks.empty()) return tracks;
    }
    // Одиночный трек: имя лежит в name, исполнители — отдельным полем.
    if (album.empty()) return {};
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

StrVec Engine::resolveAppleMusic (const Str& link, Str& album) const
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
        tracks.push_back (artist.empty() ? trackName : artist + " - " + trackName);
    }
    return tracks;
}

StrVec Engine::resolveOpenGraph (const Str& link) const
{
    // ВК Музыка и Яндекс наружу API не дают. Берём то, что страница сама
    // печатает для предпросмотра ссылки: og:title.
    const auto html = fetch (link);
    if (html.empty()) return {};

    auto title = between (html, "<meta property=\"og:title\" content=\"", "\"");
    if (title.empty()) title = between (html, "<meta name=\"og:title\" content=\"", "\"");
    if (title.empty()) return {};
    title = kd::replaceAll (kd::replaceAll (kd::replaceAll (title, "&amp;", "&"), "&quot;", "\""), "&#x27;", "'");

    auto artist = between (html, "<meta property=\"og:description\" content=\"", "\"");
    // Spotify: «Артист · Альбом · Song», Яндекс: «Артист • Трек • 2021».
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
    item->progress = 1;
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

    item->files.push_back (kd::pathStr (finalPath));
    if (! title.empty()) item->title = title;
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

// MARK: - разбор ссылки для карточки

Probe Engine::probe (const Str& text) const
{
    auto build = [&]() -> Probe
    {
        // Не ссылка — ищем трек по названию.
        if (! Detector::looksLikeLink (text))
            return probeSearch (text);

        const auto link = text;
        const auto service = Detector::serviceFor (link);

        // Закрытые каталоги: карточка из открытых данных страницы.
        if (Detector::needsResolve (service))
        {
            Probe p;
            p.ok = true;
            p.link = link;
            p.service = service;

            Str album;
            auto tracks = service == Detector::Service::spotify    ? resolveSpotify (link, album)
                        : service == Detector::Service::appleMusic ? resolveAppleMusic (link, album)
                                                                   : resolveOpenGraph (link);
            if (tracks.empty())
            {
                p.ok = false;
                p.error = service == Detector::Service::vkMusic
                    ? Str ("ВК Музыка треки наружу не отдаёт — найдите трек на YouTube")
                    : Str ("Не удалось прочитать, что это за трек");
                return p;
            }
            p.title = ! album.empty() ? album : tracks[0];
            p.uploader = tracks.size() > 1
                ? Str ("альбом") + " - " + std::to_string (tracks.size()) + Str (" треков")
                : kd::upToFirst (tracks[0], " - ");
            p.count = (int) tracks.size();
            p.isPlaylist = tracks.size() > 1;
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
        StrVec args { "--ignore-config", "--no-warnings", "-J" };
        args.push_back (flat ? "--flat-playlist" : "--no-playlist");
        args.push_back (link);
        int code = -1;
        Str out;
        if (! captureOut (args, out, &code) || code != 0 || out.empty())
        {
            p.ok = false;
            if (Detector::cookieSensitive (service))
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

        if (jtext (data, "_type") == "playlist")
        {
            p.isPlaylist = true;
            int count = kd::getInt (jtext (data, "playlist_count"));
            const auto entries = data.find ("entries");
            if (entries != data.end() && entries->is_array())
                count = count > (int) entries->size() ? count : (int) entries->size();
            p.count = count > 1 ? count : 1;
            if (p.uploader.empty()) p.uploader = "подборка";
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
                }
            std::sort (p.heights.begin(), p.heights.end(), std::greater<int>());
        }
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
Probe Engine::probeSearch (const Str& query) const
{
    Probe p;
    p.link = query;
    p.isSearch = true;
    p.service = Detector::Service::youtube;

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

            auto v = findVideoRenderer (data);
            if (v.is_object())
            {
                const auto id = jtext (v, "videoId");
                int seconds = 0;
                for (const auto& part : kd::splitTokens (runsText (v["lengthText"]), ":"))
                    seconds = seconds * 60 + kd::getInt (part);

                p.ok = true;
                p.resolved = "https://www.youtube.com/watch?v=" + id;
                p.title = runsText (v["title"]);
                if (p.title.empty()) p.title = "Без названия";
                p.uploader = runsText (v["ownerText"]);
                p.duration = seconds;
                const auto th = v.find ("thumbnail");
                if (th != v.end() && th->is_object())
                {
                    const auto arr = th->find ("thumbnails");
                    if (arr != th->end() && arr->is_array() && ! arr->empty())
                    {
                        const auto& last = (*arr)[arr->size() - 1];
                        if (last.is_object()) p.thumbnail = jtext (last, "url");
                    }
                }
                return p;
            }
        }
    }

    // Запасной путь: тот же поиск через сам yt-dlp.
    int code = -1;
    Str out;
    StrVec args { "--ignore-config", "--no-warnings", "--flat-playlist", "-J",
                  "ytsearch1:" + query };
    if (captureOut (args, out, &code) && code == 0 && ! out.empty())
    {
        const auto data = json::parse (out, nullptr, false);
        if (! data.is_discarded())
        {
            const auto entries = data.find ("entries");
            if (entries != data.end() && entries->is_array() && ! entries->empty())
            {
                const auto& e = (*entries)[0];
                p.ok = true;
                p.resolved = jtext (e, "webpage_url");
                p.title = jtext (e, "title", query);
                p.uploader = jtext (e, "uploader");
                p.duration = (int) jnum (e, "duration");
                p.thumbnail = jtext (e, "thumbnail");
            }
        }
    }

    // На YouTube пусто — SoundCloud: там находятся ремиксы и малоизвестное.
    if (! p.ok)
    {
        Str sc;
        StrVec scArgs { "--ignore-config", "--no-warnings", "--flat-playlist", "-J",
                        "scsearch1:" + query };
        if (captureOut (scArgs, sc, &code) && code == 0 && ! sc.empty())
        {
            const auto data = json::parse (sc, nullptr, false);
            if (! data.is_discarded())
            {
                const auto entries = data.find ("entries");
                if (entries != data.end() && entries->is_array() && ! entries->empty())
                {
                    const auto& e = (*entries)[0];
                    p.ok = true;
                    p.service = Detector::Service::soundcloud;
                    p.resolved = jtext (e, "webpage_url");
                    p.title = jtext (e, "title", query);
                    p.uploader = jtext (e, "uploader");
                    p.duration = (int) jnum (e, "duration");
                    p.thumbnail = jtext (e, "thumbnail");
                }
            }
        }
    }

    if (! p.ok)
        p.error = "По этому названию ничего не нашлось ни на YouTube, ни на SoundCloud. Попробуйте добавить исполнителя";
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
