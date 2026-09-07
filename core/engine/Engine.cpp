#include "Engine.h"
#include "../KDText.h"
#include "../platform/DestResolver.h"
#include "../platform/BrowserDefault.h"

#include <algorithm>

// Очередь заданий, запуск yt-dlp, построчный разбор вывода, открытые данные
// сервисов (Spotify/Apple/Pinterest), невидимая цепочка входа из браузеров.
// Правила «кто чем качается» живут в Detector.h.

static const char* ytdlpBinaryName()
{
#if JUCE_WINDOWS
    return "yt-dlp.exe";
#else
    return "yt-dlp_macos";
#endif
}

// Папка данных приложения. В JUCE userApplicationDataDirectory на macOS —
// это просто ~/Library, «Application Support» дописываем сами.
juce::File appDataRoot()
{
#if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Application Support");
#else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
#endif
}

// Журнал движка: без него жалоба «не качает» не диагностируема.
void Engine::engineLog (const juce::String& line)
{
    auto file = appDataRoot().getChildFile ("K Downloader").getChildFile ("engine.log");
    file.getParentDirectory().createDirectory();
    juce::FileOutputStream out (file);
    out.writeText ("[" + juce::Time::getCurrentTime().toString (true, true) + "] "
                       + line + "\n", false, false, "\n");
    out.flush();
}

static juce::String audioFormatName (AudioFormat f)
{
    return f == AudioFormat::m4a ? "m4a" : f == AudioFormat::wav ? "wav"
         : f == AudioFormat::flac ? "flac" : "mp3";
}

Engine::Engine() : juce::Thread ("K Downloader engine") { startThread(); }

Engine::~Engine()
{
    wake.signal();
    stopThread (8000);
}

// MARK: - очередь

juce::StringArray Engine::splitLinks (const juce::String& text)
{
    juce::StringArray links;
    for (const auto& token : juce::StringArray::fromTokens (text, " \n\r\t,;", "\"'"))
    {
        auto t = token.trim();
        if (t.isNotEmpty() && Detector::looksLikeLink (t))
            links.add (t);
    }
    return links;
}

void Engine::enqueueBatch (const juce::StringArray& links, const Options& options)
{
    {
        const juce::ScopedLock sl (lock);
        auto chain = CookieChain::build();
        const int total = links.size();
        int index = 0;
        for (const auto& link : links)
        {
            // Страховка от мусора: задание — ссылка (или поисковый запрос
            // движка ytsearch/scsearch). Голый текст в очередь не попадает.
            if (! Detector::looksLikeLink (link)
                && ! link.startsWith ("ytsearch")
                && ! link.startsWith ("scsearch"))
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
            item->dest = options.dest;
            item->batchIndex = total > 1 ? ++index : 0;
            item->batchTotal = total;
            items.push_back (item);
        }
    }
    wake.signal();
    sendChangeMessage();
}

QueueItemPtr Engine::findItem (int id)
{
    const juce::ScopedLock sl (lock);
    for (auto& i : items)
        if (i->id == id) return i;
    return nullptr;
}

void Engine::enqueuePhoto (const juce::String& link, const Options& options)
{
    {
        const juce::ScopedLock sl (lock);
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
    sendChangeMessage();
}

void Engine::cancel (int id)
{
    if (auto item = findItem (id))
    {
        item->setCancelled();
        wake.signal(); // поднять спящий поток
        sendChangeMessage();
    }
}

void Engine::remove (int id)
{
    {
        const juce::ScopedLock sl (lock);
        items.erase (std::remove_if (items.begin(), items.end(),
            [id] (const QueueItemPtr& i) { return i->id == id; }), items.end());
    }
    sendChangeMessage();
}

void Engine::clearFinished()
{
    {
        const juce::ScopedLock sl (lock);
        items.erase (std::remove_if (items.begin(), items.end(),
            [] (const QueueItemPtr& i)
            { return i->state != QueueItem::State::queued
                  && i->state != QueueItem::State::working; }),
            items.end());
    }
    sendChangeMessage();
}

std::vector<QueueItem> Engine::snapshot() const
{
    const juce::ScopedLock sl (lock);
    std::vector<QueueItem> out;
    out.reserve (items.size());
    for (const auto& i : items) out.push_back (*i);
    return out;
}

void Engine::finish (const QueueItemPtr& item, QueueItem::State state, const juce::String& stage)
{
    item->state = state;
    if (stage.isNotEmpty()) item->stage = stage;
    sendChangeMessage();
}

void Engine::setStage (const QueueItemPtr& item, const juce::String& stage)
{
    item->stage = stage;
    sendChangeMessage();
}

// MARK: - рабочий поток

void Engine::run()
{
    while (! threadShouldExit())
    {
        QueueItemPtr next;
        {
            const juce::ScopedLock sl (lock);
            // Список показывается в порядке вставки, качаем сверху вниз —
            // берём первое ждущее задание.
            for (auto& i : items)
                if (i->state == QueueItem::State::queued) { next = i; break; }
        }

        if (next == nullptr)
        {
            wake.wait (-1);
            continue;
        }
        processItem (next);
    }
}

void Engine::processItem (const QueueItemPtr& item)
{
    if (item->cancelled())
    {
        finish (item, QueueItem::State::failed, Str::utf8 ("Отменено"));
        return;
    }

    item->state = QueueItem::State::working;
    item->stage = Str::utf8 ("Готовлюсь…");
    sendChangeMessage();

    // Автоматическая папка резолвится в момент старта: в плагине это свежая
    // папка проекта DAW, в приложении — Загрузки; внутри всегда «K DOWNLOADER».
    if (item->dest == juce::File())
        item->dest = DestResolver::defaultFolder();
    item->dest.createDirectory();

    if (item->isPhoto) { startPhotoFallback (item); return; }
    if (Detector::needsResolve (item->service)) { startResolve (item); return; }

    startNative (item);

    // Pinterest часто оказывается фотографией, которую yt-dlp не видит.
    if (item->state == QueueItem::State::failed
        && item->service == Detector::Service::pinterest
        && item->files.isEmpty()
        && ! item->cancelled())
    {
        startPhotoFallback (item);
    }
}

// MARK: - инструменты

juce::File Engine::findToolsDir()
{
    // 1. Указанный вручную (тесты и нестандартные установки).
    if (const auto* env = ::getenv ("K_DOWNLOADER_TOOLS"))
        if (juce::File (juce::String (env)).isDirectory())
            return juce::File (juce::String (env));

    // 2. Установленные инструменты: сначала пользовательские (install-скрипт,
    //    fetch-tools), затем общесистемные из .pkg (кладёт их в /Library).
    auto appSupport = appDataRoot().getChildFile ("K Downloader").getChildFile ("tools");
    if (appSupport.getChildFile ("ffmpeg").existsAsFile()
        || appSupport.getChildFile ("ffmpeg.exe").existsAsFile())
        return appSupport;

#if JUCE_MAC
    auto systemTools = juce::File ("/Library/Application Support/K Downloader/tools");
    if (systemTools.getChildFile ("ffmpeg").existsAsFile())
        return systemTools;
#endif

    const auto hasFfmpeg = [] (const juce::File& dir)
    {
        return dir.getChildFile ("ffmpeg").existsAsFile()
            || dir.getChildFile ("ffmpeg.exe").existsAsFile();
    };

    // 3. Ресурсы собственного бандла — у приложения инструменты лежат здесь.
    //    В плагине currentExecutableFile — это бинарник хоста (DAW), поэтому
    //    ниже ищем и бандл плагина, и установленные копии K DOWNLOADER.
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    if (hasFfmpeg (exe.getParentDirectory().getParentDirectory().getChildFile ("Resources")))
        return exe.getParentDirectory().getParentDirectory().getChildFile ("Resources");

#if JUCE_MAC
    // 4. Бандл плагина: JUCE отдаёт его как currentApplicationFile.
    auto moduleBundle = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    if (moduleBundle != juce::File()
        && hasFfmpeg (moduleBundle.getChildFile ("Contents").getChildFile ("Resources")))
        return moduleBundle.getChildFile ("Contents").getChildFile ("Resources");

    // 5. Установленное приложение K DOWNLOADER: инструменты одни на машину,
    //    плагин в DAW пользуется ими же.
    for (const auto* appPath : { "~/Applications/K DOWNLOADER.app",
                                 "/Applications/K DOWNLOADER.app" })
    {
        auto res = juce::File (appPath).getChildFile ("Contents").getChildFile ("Resources");
        if (hasFfmpeg (res))
            return res;
    }
#endif

    // 6. Рядом с приложением — так кладёт NSIS-установщик на Windows.
    auto besideApp = exe.getParentDirectory().getChildFile ("tools");
    if (hasFfmpeg (besideApp))
        return besideApp;

    // 7. Дерево репозитория — разработка без установки.
    auto dir = exe.getParentDirectory();
    for (int i = 0; i < 8; ++i)
    {
        auto vendor = dir.getChildFile ("vendor");
        if (hasFfmpeg (vendor))
            return vendor;
        dir = dir.getParentDirectory();
    }
    return {};
}

// MARK: - запуск yt-dlp

juce::StringArray Engine::baseArgs (const juce::File& dest, const juce::String& cookie) const
{
    juce::StringArray args {
        "--ignore-config", "--no-warnings", "--newline", "--no-colors",
        "--retries", "5", "--socket-timeout", "20",
        "--concurrent-fragments", "4", "--no-mtime", "--no-overwrites",
        "--print", "before_dl:@T|%(playlist_index|1)s|%(playlist_count|1)s|%(title)s",
        "--print", "after_move:@F|%(filepath)s",
        "-P", dest.getFullPathName()
    };
    const auto tools = findToolsDir();
    if (tools != juce::File())
    {
        args.add ("--ffmpeg-location");
        args.add (tools.getFullPathName());
    }
    // Вход в аккаунт подбирается невидимо: пользователь не видит, чьи
    // cookies взяты, — интерфейс показывает только результат.
    if (cookie.isNotEmpty())
    {
        args.add ("--cookies-from-browser");
        args.add (cookie);
    }
    return args;
}

// Свойство var'а с значением по умолчанию: у DynamicObject getProperty
// умеет дефолт, у var — нет.
static juce::var jget (const juce::var& v, const char* key, juce::var def = juce::var())
{
    if (auto* obj = v.getDynamicObject())
        if (obj->hasProperty (juce::Identifier (key)))
            return obj->getProperty (juce::Identifier (key));
    return def;
}

// Тихий запуск yt-dlp: весь stdout одним куском (для -J разбора).
static bool captureOut (const juce::StringArray& args, juce::String& out, int* code = nullptr)
{
    const auto tools = Engine::findToolsDir();
    if (tools == juce::File()) return false;
#if JUCE_MAC
    const auto tool = tools.getChildFile ("ytdlp").getChildFile (ytdlpBinaryName());
#else
    const auto tool = tools.getChildFile (ytdlpBinaryName());
#endif
    juce::StringArray all;
    all.add (tool.getFullPathName());
    all.addArray (args);

    juce::ChildProcess proc;
    if (! proc.start (all, juce::ChildProcess::wantStdOut)) return false;
    juce::MemoryBlock mb;
    char chunk[16384];
    for (;;)
    {
        const int n = proc.readProcessOutput (chunk, (int) sizeof (chunk));
        if (n > 0) mb.append (chunk, (size_t) n);
        else if (! proc.isRunning()) break;
        else juce::Thread::sleep (30);
    }
    out = juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize());
    if (code != nullptr) *code = (int) proc.getExitCode();
    return true;
}

bool Engine::runYtDlp (const QueueItemPtr& item, const juce::StringArray& args, int* exitCodeOut)
{
    const auto tools = findToolsDir();
    if (tools == juce::File())
    {
        finish (item, QueueItem::State::failed,
                Str::utf8 ("Загрузчик не найден — установите инструменты"));
        return false;
    }
#if JUCE_MAC
    const auto tool = tools.getChildFile ("ytdlp").getChildFile (ytdlpBinaryName());
#else
    const auto tool = tools.getChildFile (ytdlpBinaryName());
#endif

    juce::StringArray all;
    all.add (tool.getFullPathName());
    all.addArray (args);

    engineLog (Str::utf8 ("запуск: ") + all.joinIntoString (" "));
    current = std::make_unique<juce::ChildProcess>();
    errTail = {};
    buffer = {};
    if (! current->start (all))
    {
        engineLog (Str::utf8 ("ошибка: процесс не запустился"));
        current = nullptr;
        finish (item, QueueItem::State::failed, Str::utf8 ("Не удалось запустить загрузчик"));
        return false;
    }

    char chunk[8192];
    bool cancelled = false;
    for (;;)
    {
        if (item->cancelled() || threadShouldExit()) { cancelled = true; break; }

        const int n = current->readProcessOutput (chunk, (int) sizeof (chunk));
        if (n > 0)
        {
            buffer += juce::String::fromUTF8 (chunk, n);
            int nl;
            while ((nl = buffer.indexOfChar ('\n')) >= 0)
            {
                consume (buffer.substring (0, nl), item);
                buffer = buffer.substring (nl + 1);
            }
        }
        else if (! current->isRunning())
        {
            break;
        }
        else
        {
            juce::Thread::sleep (40);
        }
    }

    if (cancelled) current->kill();

    // Хвост без перевода строки — тоже строка вывода.
    if (buffer.isNotEmpty()) consume (buffer, item);
    buffer = {};
    const auto code = (int) current->getExitCode();
    engineLog (Str::utf8 ("завершён: код ") + juce::String (code)
               + (errTail.isEmpty() ? juce::String() : Str::utf8 (" | ") + errTail));
    current = nullptr;

    if (exitCodeOut != nullptr) *exitCodeOut = code;
    return ! cancelled;
}

void Engine::consume (const juce::String& line, const QueueItemPtr& item)
{
    const auto raw = line.trimCharactersAtStart (" \r\t").trimCharactersAtEnd (" \r\t");

    // Склейку yt-dlp объявляет обычной строкой, без нашего префикса.
    if (raw.contains ("[Merger]") || raw.contains ("Merging formats"))
    {
        setStage (item, Str::utf8 ("Склеиваю видео и звук…"));
        return;
    }
    if (raw.contains ("has already been downloaded"))
    {
        item->skipped++;
        return;
    }
    if (raw.contains ("ERROR"))
        errTail = raw;

    // Ход загрузки — обычные строки «[download]  12.3% of 9.78MiB at …».
    // Свой --progress-template вместе с --concurrent-fragments yt-dlp не
    // печатает, поэтому читаем обычные.
    if (raw.startsWith ("[download]"))
    {
        const auto pct = raw.indexOfChar ('%');
        if (pct > 0)
        {
            const auto head = raw.substring (0, pct);
            int start = head.length();
            while (start > 0 && head.substring (start - 1, start).containsOnly ("0123456789."))
                --start;
            const auto value = head.substring (start).getDoubleValue();
            if (value > 0.0)
            {
                item->state = QueueItem::State::working;
                item->progress = (float) juce::jlimit (0.0, 100.0, value) / 100.0f;
                // Не топтать «Качаю 2 из 14»: процент прицепляем отдельным хвостом.
                if (item->stage.containsChar (U'\u00B7'))
                    item->stage = item->stage.upToFirstOccurrenceOf (Str::utf8 ("\u00B7"), false, false).trim();
                item->stage = item->stage.trimEnd() + Str::utf8 (" \u00B7 ")
                            + juce::String (value, 1) + "%";
                sendChangeMessage();
            }
        }
        return;
    }
    if (! raw.startsWith ("@")) return;

    // Наши маркеры: @T|индекс|всего|название и @F|путь файла.
    const auto parts = juce::StringArray::fromTokens (raw, "|", "");
    if (parts[0] == "@T" && parts.size() >= 4)
    {
        item->itemIndex = parts[1].getIntValue();
        if (const auto n = parts[2].getIntValue(); n > 0) item->itemTotal = n;
        if (item->itemTotal > 1) item->title = parts[3];
        item->progress = 0;
        item->state = QueueItem::State::working;
        item->stage = item->itemTotal > 1
            ? Str::utf8 ("Качаю ") + juce::String (item->itemIndex)
              + Str::utf8 (" из ") + juce::String (item->itemTotal)
            : Str::utf8 ("Качаю…");
        sendChangeMessage();
    }
    else if (parts[0] == "@F" && parts.size() >= 2)
    {
        item->files.add (parts[1]);
        item->progress = 1;
        // Заголовок строки — имя файла, пока не пришло настоящее название.
        if (item->title.isEmpty())
            item->title = juce::File (parts[1]).getFileNameWithoutExtension();
    }
}

// MARK: - обычные сервисы

void Engine::startNative (const QueueItemPtr& item)
{
    // Порядок попыток: сначала без входа (открытые материалы качаются и так),
    // затем невидимо через браузеры — браузер по умолчанию, потом остальные.
    juce::StringArray attempts { "" };
    for (const auto& b : item->cookieChain)
        if (! attempts.contains (b))
            attempts.add (b);

    for (int attempt = 0; attempt < attempts.size(); ++attempt)
    {
        if (item->cancelled() || threadShouldExit()) break;
        if (attempt > 0)
            setStage (item, Str::utf8 ("Пробую ещё раз…"));

        juce::StringArray args = baseArgs (item->dest, attempts[attempt]);

        if (item->isAudio)
        {
            args.addTokens ("-f bestaudio/best -x", true);
            args.add ("--audio-format");
            args.add (audioFormatName (item->audioFormat));
            args.addTokens ("--audio-quality 0 --embed-metadata", true);
            // В WAV обложку не вшить: попытка заканчивается ошибкой, а рядом
            // с файлом остаются картинки.
            if (item->audioFormat != AudioFormat::wav)
                args.add ("--embed-thumbnail");
        }
        else if (item->service == Detector::Service::instagram
                 || item->service == Detector::Service::pinterest)
        {
            // По ссылке может лежать картинка — просить у неё высоту бессмысленно.
            args.add ("-f");
            args.add ("bestvideo*+bestaudio/best");
        }
        else if (item->maxHeight > 0)
        {
            // Хвостовое /best обязательно: у TikTok одна нестандартная дорожка,
            // без запасного варианта yt-dlp отвечал «Requested format is not
            // available».
            args.add ("-f");
            args.add ("bestvideo[height<=" + juce::String (item->maxHeight)
                      + "]+bestaudio/best[height<=" + juce::String (item->maxHeight) + "]/best");
            args.addTokens ("--merge-output-format mp4", true);
        }
        else
        {
            args.add ("-f");
            args.add ("bestvideo+bestaudio/best");
            args.addTokens ("--merge-output-format mp4", true);
        }

        if (item->wholePlaylist)
        {
            // Подборка целиком складывается в свою папку, с нумерацией.
            args.add ("--yes-playlist");
            args.add ("-o");
            args.add ("%(playlist_title).80B/%(playlist_index)02d - %(title).100B.%(ext)s");
        }
        else if (item->nameOverride.isNotEmpty())
        {
            // Человек искал трек по названию — файл называется треком, а не
            // «… (Official Video)». Титул в тегах тоже наш, не ютубовский.
            args.add ("--no-playlist");
            args.add ("-o");
            args.add (safeName (item->nameOverride) + ".%(ext)s");
            args.add ("--parse-metadata");
            args.add (safeName (item->nameOverride) + ":%(title)s");
        }
        else
        {
            // Ролик, открытый внутри плейлиста, качаем как ролик.
            args.add ("--no-playlist");
            args.add ("-o");
            args.add ("%(title).120B.%(ext)s");
        }

        args.add (item->link);

        int code = -1;
        const bool ran = runYtDlp (item, args, &code);
        if (item->cancelled())
        {
            finish (item, QueueItem::State::failed, Str::utf8 ("Отменено"));
            return;
        }
        if (! ran) return; // состояние уже выставлено runYtDlp с настоящей причиной

        if (code == 0 || ! item->files.isEmpty())
        {
            const bool already = item->files.isEmpty() && item->skipped > 0;
            juce::String stage = already ? Str::utf8 ("Уже скачано") : Str::utf8 ("Готово");
            if (item->files.size() > 1)
                stage += Str::utf8 (" · файлов: ") + juce::String (item->files.size());
            if (item->skipped > 0)
                stage += Str::utf8 (" · уже было: ") + juce::String (item->skipped);
            item->progress = 1;
            finish (item, QueueItem::State::done, stage);
            return;
        }

        // Ошибка. Открытые материалы качаются с первой попытки; для остальных
        // незаметно для человека пробуем следующий источник входа.
        const bool canRetry = retryWithCookies (errTail) && attempt < attempts.size() - 1;
        if (! canRetry) break;
        engineLog (Str::utf8 ("попытка без входа не удалась, перехожу к следующему источнику"));
    }

    finish (item, QueueItem::State::failed, humanError (errTail));
}

bool Engine::retryWithCookies (const juce::String& raw)
{
    const auto low = raw.toLowerCase();
    return low.contains ("login") || low.contains ("cookie")
        || low.contains ("private") || low.contains ("rate-limit")
        || low.contains ("sign in") || low.contains ("sign-in")
        || (low.contains ("age") && low.contains ("restrict"))
        || low.contains ("403") || low.contains ("forbidden")
        || low.contains ("failed to decrypt") || low.contains ("keyring");
}

juce::String Engine::humanError (const juce::String& raw)
{
    // Из потока ошибок yt-dlp человеку нужна одна понятная строка.
    const auto first = raw.upToFirstOccurrenceOf ("\n", false, false);
    const auto low = first.toLowerCase();

    if (low.contains ("login") || low.contains ("cookies") || low.contains ("private")
        || low.contains ("rate-limit") || low.contains ("sign in"))
    {
        // Источники входа в интерфейсе не называем: качаются только
        // открытые материалы.
        return Str::utf8 ("Не получилось: запись закрыта. Качаются только открытые материалы");
    }
    if (low.contains ("403") || low.contains ("forbidden"))
        return Str::utf8 ("Сайт отказал — попробуйте позже");
    if (low.contains ("drm"))
        return Str::utf8 ("Запись защищена от скачивания");
    if (low.contains ("no video formats"))
        return Str::utf8 ("По ссылке нет видео — возможно, это фото");
    if (low.contains ("unavailable") || low.contains ("404") || low.contains ("not found"))
        return Str::utf8 ("По ссылке ничего нет: запись удалена или скрыта");
    if (low.contains ("unsupported url") || low.contains ("no suitable")
        || low.contains ("not a valid url"))
        return Str::utf8 ("Адрес не опознан — нужна ссылка на саму запись");
    if (low.contains ("requested format is not available"))
        return Str::utf8 ("В этом качестве записи нет — выберите другое");

    auto clean = first.replace ("ERROR: ", juce::String()).trim();
    if (clean.length() > 120) clean = clean.substring (0, 120);
    return clean.isEmpty() ? Str::utf8 ("Загрузка не удалась") : clean;
}

// MARK: - закрытые каталоги: Spotify, Apple, Яндекс, ВК

void Engine::startResolve (const QueueItemPtr& item)
{
    setStage (item, Str::utf8 ("Читаю каталог…"));

    juce::String album;
    juce::StringArray tracks;
    if      (item->service == Detector::Service::spotify)    tracks = resolveSpotify (item->link, album);
    else if (item->service == Detector::Service::appleMusic) tracks = resolveAppleMusic (item->link, album);
    else                                                     tracks = resolveOpenGraph (item->link);

    if (tracks.isEmpty())
    {
        finish (item, QueueItem::State::failed,
            item->service == Detector::Service::vkMusic
                ? Str::utf8 ("ВК Музыка треки наружу не отдаёт — вставьте ссылку на этот трек с YouTube")
                : Str::utf8 ("Не удалось прочитать, что это за трек"));
        return;
    }

    item->itemTotal = tracks.size();

    // Альбом складываем в отдельную папку.
    if (tracks.size() > 1 && album.isNotEmpty())
    {
        const auto folder = item->dest.getChildFile (safeName (album));
        folder.createDirectory();
        item->dest = folder;
    }

    for (int i = 0; i < tracks.size(); ++i)
    {
        if (item->cancelled() || threadShouldExit()) break;
        // Трек «Артист - Название»: художник уйдёт в теги, искать будем
        // по всей строке.
        const auto query = tracks[i];
        const auto sep = query.indexOf (" - ");
        downloadTrack (item, i, Detector::Service::youtube,
                       sep > 0 ? query.substring (0, sep) : juce::String(),
                       sep > 0 ? query.substring (sep + 3) : query);
    }

    if (item->cancelled())
    {
        finish (item, QueueItem::State::failed, Str::utf8 ("Отменено"));
        return;
    }
    if (item->files.isEmpty())
    {
        finish (item, QueueItem::State::failed,
                Str::utf8 ("Ничего не нашлось по названиям треков"));
        return;
    }
    item->progress = 1;
    finish (item, QueueItem::State::done,
            Str::utf8 ("Готово · треков: ") + juce::String (item->files.size()));
}

void Engine::downloadTrack (const QueueItemPtr& item, const int index,
                            const Detector::Service searchSite,
                            const juce::String& artist, const juce::String& track)
{
    const auto query = artist.isEmpty() ? track : artist + " - " + track;
    item->itemIndex = index + 1;
    item->title = query;
    item->progress = 0;
    setStage (item, Str::utf8 ("Качаю ") + juce::String (index + 1)
                  + Str::utf8 (" из ") + juce::String (item->itemTotal));

    juce::StringArray args = baseArgs (item->dest, {});
    args.addTokens ("-f bestaudio/best -x", true);
    args.add ("--audio-format");
    args.add (audioFormatName (item->audioFormat));
    args.addTokens ("--audio-quality 0 --embed-metadata --embed-thumbnail --no-playlist", true);
    const auto name = safeName (query);
    args.add ("-o");
    args.add (name + ".%(ext)s");
    // Теги пишем свои: иначе в файл уедет название ролика с YouTube.
    // Двоеточие делит аргумент пополам, поэтому значения чистим.
    if (artist.isNotEmpty())
    {
        args.add ("--parse-metadata");
        args.add (safeName (artist) + ":%(artist)s");
    }
    args.add ("--parse-metadata");
    args.add (safeName (track) + ":%(title)s");
    args.addTokens ("--ignore-errors --max-downloads 1", true);
    // Пять кандидатов: первый результат бывает защищённым или недоступным.
    args.add (searchSite == Detector::Service::soundcloud
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

juce::String Engine::fetch (const juce::String& url, const int timeoutMs)
{
    auto stream = juce::URL (url).createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withExtraHeaders (
                "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36\r\n"
                "Accept-Language: ru,en;q=0.8\r\n")
            .withConnectionTimeoutMs (timeoutMs));
    if (stream == nullptr) return {};
    return stream->readEntireStreamAsString();
}

juce::String Engine::between (const juce::String& text, const juce::String& start,
                              const juce::String& end)
{
    const auto a = text.indexOf (start);
    if (a < 0) return {};
    const auto body = text.substring (a + start.length());
    const auto b = body.indexOf (end);
    return b < 0 ? juce::String() : body.substring (0, b);
}

juce::var Engine::dig (const juce::var& root, const juce::String& path)
{
    auto cur = root;
    for (const auto& key : juce::StringArray::fromTokens (path, ".", {}))
    {
        auto* obj = cur.getDynamicObject();
        if (obj == nullptr) return {};
        cur = obj->getProperty (key);
    }
    return cur;
}

juce::StringArray Engine::resolveSpotify (const juce::String& link, juce::String& album) const
{
    // .../track/ID, .../album/ID, .../playlist/ID — иногда с /intl-ru/ внутри.
    juce::String kind, value;
    for (const auto* k : { "track", "album", "playlist" })
    {
        const auto marker = juce::String ("/") + k + "/";
        const auto p = link.indexOf (marker);
        if (p >= 0)
        {
            kind = marker.substring (1, marker.length() - 1);
            const auto tail = link.substring (p + marker.length());
            int end = 0;
            while (end < tail.length()
                   && tail.substring (end, end + 1).containsOnly (
                          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"))
                ++end;
            value = tail.substring (0, end);
            break;
        }
    }
    if (kind.isEmpty()) return {};

    // У страницы-вставки есть готовый JSON со всем содержимым.
    const auto html = fetch ("https://open.spotify.com/embed/" + kind + "/" + value);
    const auto open = html.indexOf ("<script id=\"__NEXT_DATA__\"");
    if (open < 0) return {};
    const auto bodyStart = html.indexOf (open, ">") + 1;
    const auto close = html.indexOf (bodyStart, "</script>");
    if (close < 0) return {};

    const auto data = juce::JSON::parse (html.substring (bodyStart, close));
    const auto entity = dig (data, "props.pageProps.state.data.entity");
    auto* obj = entity.getDynamicObject();
    if (obj == nullptr) return {};

    album = obj->getProperty ("name").toString();

    juce::StringArray tracks;
    // Альбом и подборка: список лежит целиком.
    if (auto* list = obj->getProperty ("trackList").getArray())
    {
        for (const auto& t : *list)
            if (auto* to = t.getDynamicObject())
            {
                const auto tTitle = to->getProperty ("title").toString();
                const auto artist = to->getProperty ("subtitle").toString();
                if (tTitle.isNotEmpty())
                    tracks.add (artist.isEmpty() ? tTitle : artist + " - " + tTitle);
            }
        if (! tracks.isEmpty()) return tracks;
    }
    // Одиночный трек: имя лежит в name, исполнители — отдельным полем.
    if (album.isEmpty()) return {};
    juce::StringArray artists;
    if (auto* arr = obj->getProperty ("artists").getArray())
        for (const auto& a : *arr)
            if (auto* ao = a.getDynamicObject())
                artists.add (ao->getProperty ("name").toString());
    return { artists.isEmpty() ? album : artists.joinIntoString (", ") + " - " + album };
}

juce::StringArray Engine::resolveAppleMusic (const juce::String& link, juce::String& album) const
{
    const auto url = juce::URL (link.startsWith ("http") ? link : "https://" + link);
    // ?i=… — конкретный трек внутри альбома, он важнее номера альбома.
    juce::String songId;
    {
        const auto& names = url.getParameterNames();
        const auto& values = url.getParameterValues();
        for (int i = 0; i < names.size(); ++i)
            if (names[i] == "i") { songId = values[i]; break; }
    }
    const bool isSong = songId.isNotEmpty();
    if (songId.isEmpty())
    {
        const auto path = url.getSubPath();
        int end = path.length();
        while (end > 0 && path.substring (end - 1, end).containsOnly ("0123456789")) --end;
        if (end < path.length()) songId = path.substring (end);
    }
    if (songId.isEmpty()) return {};

    // Открытый справочник Apple: по номеру отдаёт исполнителя и название,
    // для альбома — сразу все его треки.
    const auto json = juce::JSON::parse (fetch (
        "https://itunes.apple.com/lookup?id=" + songId
        + (isSong ? "" : "&entity=song") + "&limit=200"));
    auto* arr = dig (json, "results").getArray();
    if (arr == nullptr || arr->isEmpty()) return {};

    juce::StringArray tracks;
    for (const auto& r : *arr)
        if (auto* obj = r.getDynamicObject())
        {
            if (obj->getProperty ("wrapperType").toString() == "collection")
            {
                album = obj->getProperty ("collectionName").toString();
                continue;
            }
            const auto trackName = obj->getProperty ("trackName").toString();
            if (trackName.isEmpty()) continue;
            const auto artist = obj->getProperty ("artistName").toString();
            tracks.add (artist.isEmpty() ? trackName : artist + " - " + trackName);
        }
    return tracks;
}

juce::StringArray Engine::resolveOpenGraph (const juce::String& link) const
{
    // ВК Музыка и Яндекс наружу API не дают. Берём то, что страница сама
    // печатает для предпросмотра ссылки: og:title.
    const auto html = fetch (link);
    if (html.isEmpty()) return {};

    auto title = between (html, "<meta property=\"og:title\" content=\"", "\"");
    if (title.isEmpty()) title = between (html, "<meta name=\"og:title\" content=\"", "\"");
    if (title.isEmpty()) return {};
    title = title.replace ("&amp;", "&").replace ("&quot;", "\"").replace ("&#x27;", "'");

    auto artist = between (html, "<meta property=\"og:description\" content=\"", "\"");
    // Spotify: «Артист · Альбом · Song», Яндекс: «Артист • Трек • 2021».
    const auto sep = artist.indexOfAnyOf (Str::utf8 ("\u00B7\u2022"), 0, false);
    if (sep > 0) artist = artist.substring (0, sep).trim();

    if (artist.isEmpty() || artist == title) return { title };
    return { artist + " - " + title };
}

// MARK: - Pinterest-фото

void Engine::startPhotoFallback (const QueueItemPtr& item)
{
    if (! downloadPinterestPhoto (item))
    {
        if (! item->cancelled())
            finish (item, QueueItem::State::failed,
                    Str::utf8 ("Не удалось забрать фотографию по этой ссылке"));
        else
            finish (item, QueueItem::State::failed, Str::utf8 ("Отменено"));
        return;
    }
    item->progress = 1;
    finish (item, QueueItem::State::done, Str::utf8 ("Готово · фотография"));
}

bool Engine::downloadPinterestPhoto (const QueueItemPtr& item)
{
    setStage (item, Str::utf8 ("Забираю фотографию…"));

    // yt-dlp пин с фотографией не отдаёт («No video formats found»), зато
    // у Pinterest открытый oEmbed с превью; из адреса превью получается
    // адрес оригинала — размер в пути («236x») меняется на «originals».
    const auto api = juce::URL ("https://www.pinterest.com/oembed.json")
                         .withParameter ("url", item->link);
    const auto data = juce::JSON::parse (fetch (api.toString (true)));
    const auto thumb = jget (data, "thumbnail_url").toString();
    const auto title = jget (data, "title").toString();
    if (thumb.isEmpty()) return false;

    const auto thumbUrl = juce::URL (thumb);
    const auto original = "https://" + thumbUrl.getDomain()
                        + "/originals/" + thumbUrl.getSubPath().fromFirstOccurrenceOf ("/", false, false);

    // Если оригинала нет — вернёмся к превью, оно есть всегда.
    juce::String fileUrl = original;
    auto stream = juce::URL (fileUrl).createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (15000));
    if (stream == nullptr)
    {
        fileUrl = thumb;
        stream = juce::URL (fileUrl).createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (15000));
    }
    if (stream == nullptr) return false;

    const auto path = juce::URL (fileUrl).getSubPath().upToFirstOccurrenceOf ("?", false, false);
    auto ext = path.fromLastOccurrenceOf (".", false, false).toLowerCase();
    if (ext.isEmpty() || ext.length() > 4 || ext.containsAnyOf ("/\\")) ext = "jpg";

    const auto name = safeName (title.isEmpty()
                                    ? Str::utf8 ("Фотография Pinterest")
                                    : title) + "." + ext;
    const auto target = item->dest.getChildFile (name);
    item->dest.createDirectory();

    juce::FileOutputStream out (target);
    if (! out.openedOk()) return false;
    out.writeFromInputStream (*stream, -1);
    out.flush();
    if (out.getStatus().failed()) return false;

    item->files.add (target.getFullPathName());
    if (title.isNotEmpty()) item->title = title;
    return true;
}

// MARK: - чистка названий

juce::String Engine::cleanTrackName (const juce::String& raw)
{
    // Убирает ютубовские приписки в скобках: «(Official Video)»,
    // «[Lyrics]». Скобки без этих слов не трогаем — там бывает нужное,
    // вроде «(feat. кто-то)».
    static const juce::StringArray junk { "official video", "official music video",
        "official audio", "official lyric video", "lyric video", "lyrics",
        "audio only", "remaster", "remastered", "hd", "hq", "4k", "8k", "mv",
        "official", "visualizer", "клип", "премьера" };

    auto out = raw;
    for (int pass = 0; pass < 8; ++pass)
    {
        bool changed = false;
        for (const auto& bracket : { std::pair<const char*, const char*> {"(", ")"}, {"[", "]"} })
        {
            const auto a = out.indexOf (bracket.first);
            if (a < 0) continue;
            const auto b = out.indexOf (a + 1, bracket.second);
            if (b < 0) continue;
            const auto inside = out.substring (a + 1, b).toLowerCase();
            for (const auto& j : junk)
                if (inside.contains (j))
                {
                    out = out.substring (0, a) + " " + out.substring (b + 1);
                    changed = true;
                    break;
                }
            if (changed) break;
        }
        if (! changed) break;
    }
    while (out.contains ("  ")) out = out.replace ("  ", " ");
    return out.trim();
}

juce::String Engine::safeName (const juce::String& s)
{
    // Имя файла без символов, на которых спотыкается файловая система, и
    // без двоеточия — оно делит аргумент --parse-metadata пополам.
    auto out = s;
    for (const auto bad : { '/', ':', '%', '"', '\\', '\n', '\r', '\t' })
        out = out.replaceCharacter (bad, ' ');
    out = out.trim();
    return out.isEmpty() ? "track" : out.substring (0, 110);
}

// MARK: - разбор ссылки для карточки

Probe Engine::probe (const juce::String& text) const
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

            juce::String album;
            auto tracks = service == Detector::Service::spotify    ? resolveSpotify (link, album)
                        : service == Detector::Service::appleMusic ? resolveAppleMusic (link, album)
                                                                   : resolveOpenGraph (link);
            if (tracks.isEmpty())
            {
                p.ok = false;
                p.error = service == Detector::Service::vkMusic
                    ? Str::utf8 ("ВК Музыка треки наружу не отдаёт — найдите трек на YouTube")
                    : Str::utf8 ("Не удалось прочитать, что это за трек");
                return p;
            }
            p.title = album.isNotEmpty() ? album : tracks[0];
            p.uploader = tracks.size() > 1
                ? Str::utf8 ("альбом") + " - " + juce::String (tracks.size()) + Str::utf8 (" треков")
                : tracks[0].upToFirstOccurrenceOf (" - ", false, false);
            p.count = tracks.size();
            p.isPlaylist = tracks.size() > 1;
            return p;
        }

        // Pinterest: по пину с фотографией yt-dlp отвечает «нет форматов»,
        // поэтому сначала пробуем его, затем карточку собираем из oEmbed.
        if (service == Detector::Service::pinterest)
        {
            Probe viaYtdlp;
            int code = -1;
            juce::String out;
            juce::StringArray args { "--ignore-config", "--no-warnings", "-J", "--no-playlist" };
            args.add (link);
            if (captureOut (args, out, &code) && code == 0 && out.isNotEmpty())
            {
                const auto data = juce::JSON::parse (out);
                if (! data.isVoid())
                {
                    viaYtdlp.ok = true;
                    viaYtdlp.link = link;
                    viaYtdlp.service = service;
                    viaYtdlp.title = jget (data, "title").toString();
                    viaYtdlp.uploader = jget (data, "uploader").toString();
                    viaYtdlp.thumbnail = jget (data, "thumbnail").toString();
                    viaYtdlp.duration = (int) jget (data, "duration").toString().getDoubleValue();
                }
            }
            if (viaYtdlp.ok) return viaYtdlp;
            return probePinterestPhoto (link);
        }

        // Все остальные: одним -J. Подборка разбирается плоско — большой
        // плейлист не заставит ждать минуту.
        Probe p;
        const bool flat = Detector::isCollection (link);
        juce::StringArray args { "--ignore-config", "--no-warnings", "-J" };
        args.add (flat ? "--flat-playlist" : "--no-playlist");
        args.add (link);
        int code = -1;
        juce::String out;
        if (! captureOut (args, out, &code) || code != 0 || out.isEmpty())
        {
            p.ok = false;
            if (Detector::cookieSensitive (service))
                p.error = Str::utf8 ("Качаются только открытые материалы: запись скрыта или требует входа");
            else
                p.error = Str::utf8 ("По ссылке ничего нет: запись удалена, скрыта или требует входа");
            return p;
        }
        const auto data = juce::JSON::parse (out);
        if (data.isVoid())
        {
            p.ok = false;
            p.error = Str::utf8 ("Не удалось разобрать запись");
            return p;
        }
        p.ok = true;
        p.link = link;
        p.service = service;
        p.hasPlaylist = Detector::hasPlaylist (link);
        p.title = jget (data, "title", Str::utf8 ("Без названия")).toString();
        p.uploader = jget (data, "uploader").toString();
        if (p.uploader.isEmpty())
            p.uploader = jget (data, "channel").toString();
        p.thumbnail = jget (data, "thumbnail").toString();
        p.duration = (int) jget (data, "duration").toString().getDoubleValue();

        if (jget (data, "_type").toString() == "playlist")
        {
            p.isPlaylist = true;
            auto count = jget (data, "playlist_count").toString().getIntValue();
            if (auto* entries = jget (data, "entries").getArray())
                count = juce::jmax (count, entries->size());
            p.count = juce::jmax (count, 1);
            if (p.uploader.isEmpty()) p.uploader = Str::utf8 ("подборка");
        }
        else
        {
            // Высоты кадра — из списка форматов: 4K у ролика без 4K не показываем.
            if (auto* formats = jget (data, "formats").getArray())
                for (const auto& f : *formats)
                    if (auto* fo = f.getDynamicObject())
                    {
                        const auto h = fo->getProperty ("height").toString().getIntValue();
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
    if (result.ok && result.resolved.isEmpty()) result.resolved = result.link;
    return result;
}

Probe Engine::probePinterestPhoto (const juce::String& link) const
{
    Probe p;
    const auto api = juce::URL ("https://www.pinterest.com/oembed.json")
                         .withParameter ("url", link);
    const auto data = juce::JSON::parse (fetch (api.toString (true)));
    const auto thumb = jget (data, "thumbnail_url").toString();
    if (thumb.isEmpty())
    {
        p.ok = false;
        p.error = Str::utf8 ("Не удалось прочитать пин");
        return p;
    }
    p.ok = true;
    p.link = link;
    p.service = Detector::Service::pinterest;
    p.isPhoto = true;
    p.title = jget (data, "title", Str::utf8 ("Фотография")).toString();
    p.thumbnail = thumb;
    return p;
}

// Поиск трека по названию: прямой запрос к странице результатов YouTube —
// одна загрузка вместо запуска целого процесса. yt-dlp остаётся запасным
// путём на случай, если разметка поменяется; если и он пуст — SoundCloud.
Probe Engine::probeSearch (const juce::String& query) const
{
    Probe p;
    p.link = query;
    p.isSearch = true;
    p.service = Detector::Service::youtube;

    const auto html = fetch ("https://www.youtube.com/results?search_query="
                             + juce::URL::addEscapeChars (query, true));
    const juce::String marker = "var ytInitialData = ";
    const auto a = html.indexOf (marker);
    if (a >= 0)
    {
        const auto body = html.substring (a + marker.length());
        const auto b = body.indexOf (";</script>");
        if (b >= 0)
        {
            const auto data = juce::JSON::parse (body.substring (0, b));

            // Первый настоящий ролик в выдаче: обходим дерево до videoRenderer.
            std::function<juce::var (const juce::var&)> findVideo = [&] (const juce::var& v) -> juce::var
            {
                if (auto* obj = v.getDynamicObject())
                {
                    auto vr = obj->getProperty ("videoRenderer");
                    if (! vr.isVoid()) return vr;
                    auto& props = obj->getProperties();
                    for (int i = 0; i < props.size(); ++i)
                        if (auto found = findVideo (props.getValueAt (i)); ! found.isVoid()) return found;
                }
                else if (auto* arr = v.getArray())
                    for (const auto& item : *arr)
                        if (auto found = findVideo (item); ! found.isVoid()) return found;
                return {};
            };

            auto runsText = [] (const juce::var& d) -> juce::String
            {
                if (auto* o = d.getDynamicObject())
                {
                    if (auto* runs = o->getProperty ("runs").getArray(); runs != nullptr && runs->size() > 0)
                        if (auto* r = (*runs)[0].getDynamicObject())
                            return r->getProperty ("text").toString();
                    return o->getProperty ("simpleText").toString();
                }
                return {};
            };

            auto v = findVideo (data);
            if (auto* vo = v.getDynamicObject())
            {
                const auto id = vo->getProperty ("videoId").toString();
                int seconds = 0;
                for (const auto& part : juce::StringArray::fromTokens (runsText (vo->getProperty ("lengthText")), ":", ""))
                    seconds = seconds * 60 + part.getIntValue();

                p.ok = true;
                p.resolved = "https://www.youtube.com/watch?v=" + id;
                p.title = runsText (vo->getProperty ("title"));
                if (p.title.isEmpty()) p.title = Str::utf8 ("Без названия");
                p.uploader = runsText (vo->getProperty ("ownerText"));
                p.duration = seconds;
                if (auto* th = vo->getProperty ("thumbnail").getDynamicObject())
                    if (auto* arr = th->getProperty ("thumbnails").getArray(); arr != nullptr && arr->size() > 0)
                        if (auto* last = (*arr)[arr->size() - 1].getDynamicObject())
                            p.thumbnail = last->getProperty ("url").toString();
                return p;
            }
        }
    }

    // Запасной путь: тот же поиск через сам yt-dlp.
    int code = -1;
    juce::String out;
    juce::StringArray args { "--ignore-config", "--no-warnings", "--flat-playlist", "-J",
                             "ytsearch1:" + query };
    if (captureOut (args, out, &code) && code == 0 && out.isNotEmpty())
    {
        const auto data = juce::JSON::parse (out);
        if (auto* entries = jget (data, "entries").getArray(); entries != nullptr && entries->size() > 0)
            if (auto* e = (*entries)[0].getDynamicObject())
            {
                p.ok = true;
                p.resolved = jget (e, "webpage_url").toString();
                p.title = jget (e, "title", juce::var (query)).toString();
                p.uploader = jget (e, "uploader").toString();
                p.duration = (int) jget (e, "duration").toString().getDoubleValue();
                p.thumbnail = jget (e, "thumbnail").toString();
            }
    }

    // На YouTube пусто — SoundCloud: там находятся ремиксы и малоизвестное.
    if (! p.ok)
    {
        juce::String sc;
        juce::StringArray scArgs { "--ignore-config", "--no-warnings", "--flat-playlist", "-J",
                                   "scsearch1:" + query };
        if (captureOut (scArgs, sc, &code) && code == 0 && sc.isNotEmpty())
        {
            const auto data = juce::JSON::parse (sc);
            if (auto* entries = jget (data, "entries").getArray(); entries != nullptr && entries->size() > 0)
                if (auto* e = (*entries)[0].getDynamicObject())
                {
                    p.ok = true;
                    p.service = Detector::Service::soundcloud;
                    p.resolved = jget (e, "webpage_url").toString();
                    p.title = jget (e, "title", juce::var (query)).toString();
                    p.uploader = jget (e, "uploader").toString();
                    p.duration = (int) jget (e, "duration").toString().getDoubleValue();
                    p.thumbnail = jget (e, "thumbnail").toString();
                }
        }
    }

    if (! p.ok)
        p.error = Str::utf8 ("По этому названию ничего не нашлось ни на YouTube, ни на SoundCloud. Попробуйте добавить исполнителя");
    return p;
}

// MARK: - ProbeRunner

void ProbeRunner::start (const juce::String& text,
                         const std::function<Probe (const juce::String&)>& fn,
                         std::function<void (const Probe&)> onDone)
{
    {
        const juce::ScopedLock sl (lock);
        pending = text.trim();
        probeFn = fn;
        callback = std::move (onDone);
        generation++;
    }
    wake.signal();
}

void ProbeRunner::run()
{
    for (;;)
    {
        if (threadShouldExit()) return;
        wake.wait (-1);
        if (threadShouldExit()) return;

        juce::String text;
        int gen = 0;
        std::function<Probe (const juce::String&)> fn;
        std::function<void (const Probe&)> done;
        {
            const juce::ScopedLock sl (lock);
            text = pending;
            gen = generation;
            fn = probeFn;
            done = callback;
        }
        if (text.isEmpty() || fn == nullptr) continue;

        // Разбор занимает секунды; если за это время человек допечатал
        // (поколение сменилось) — результат выбрасываем.
        const auto probe = fn (text);
        {
            const juce::ScopedLock sl (lock);
            if (gen != generation) continue;
        }
        if (done)
            juce::MessageManager::callAsync ([done, probe] { done (probe); });
    }
}
