// Контрактные тесты kdcore.
//
// Чистые (детектор, разбор ссылок, имена файлов) гоняются всегда; живые
// (--live) бьют по реальным источникам: probe по ссылкам, поиск по
// названию, сквозные скачивания YouTube/SoundCloud в тестовую папку.
// Живые тесты прогонять после каждого обновления yt-dlp.

#include "Engine.h"
#include "Detector.h"
#include "kd_compat.h"
#include "kd_url.h"
#include "kd_capi.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>

#ifdef _WIN32
static void testSetenv (const char* k, const char* v) { ::_putenv_s (k, v); }
static void testUnsetenv (const char* k) { ::_putenv_s (k, ""); }
#else
static void testSetenv (const char* k, const char* v) { ::setenv (k, v, 1); }
static void testUnsetenv (const char* k) { ::unsetenv (k); }
#endif
#include <string>
#include <thread>

namespace fs = std::filesystem;

static int passed = 0, failed = 0;

static void check (bool ok, const std::string& name, const std::string& detail = {})
{
    if (ok) { ++passed; std::cout << "  ok  " << name << "\n"; }
    else    { ++failed; std::cout << "FAIL  " << name << (detail.empty() ? "" : "  → " + detail) << "\n"; }
}

// ---- чистые ----

static void testDetector()
{
    std::cout << "Detector::serviceFor\n";
    using S = Detector::Service;
    check (Detector::serviceFor ("https://www.youtube.com/watch?v=jNQXAC9IVRw") == S::youtube, "youtube watch");
    check (Detector::serviceFor ("https://youtu.be/jNQXAC9IVRw") == S::youtube, "youtu.be");
    check (Detector::serviceFor ("https://music.youtube.com/watch?v=x") == S::youtubeMusic, "youtube music");
    check (Detector::serviceFor ("https://www.instagram.com/reel/DaikHQxo_Z3/") == S::instagram, "instagram reel");
    check (Detector::serviceFor ("https://www.tiktok.com/@user/video/123") == S::tiktok, "tiktok");
    check (Detector::serviceFor ("https://pin.it/3xYz") == S::pinterest, "pin.it");
    check (Detector::serviceFor ("https://www.pinterest.com/pin/1004795366878354024/") == S::pinterest, "pinterest pin");
    check (Detector::serviceFor ("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC") == S::spotify, "spotify");
    check (Detector::serviceFor ("https://music.apple.com/ru/album/1440833098") == S::appleMusic, "apple music");
    check (Detector::serviceFor ("https://soundcloud.com/forss/flickermood") == S::soundcloud, "soundcloud");
    check (Detector::serviceFor ("https://vk.com/audio1_-123_456") == S::vkMusic, "vk audio");
    check (Detector::serviceFor ("https://vkvideo.ru/video-123_456") == S::vk, "vk video");
    check (Detector::serviceFor ("https://example.com/video") == S::unknown, "unknown host");
}

static void testLinkLogic()
{
    std::cout << "плейлисты и ссылки\n";
    check (Detector::hasPlaylist ("https://www.youtube.com/watch?v=x&list=PL123"), "hasPlaylist: watch+list");
    check (! Detector::isCollection ("https://www.youtube.com/watch?v=x&list=PL123"), "watch+list — не подборка");
    check (Detector::isCollection ("https://www.youtube.com/playlist?list=PL123"), "чистый playlist — подборка");
    check (Detector::isCollection ("https://open.spotify.com/album/4aawyAB9vmq"), "альбом — подборка");
    check (Detector::looksLikeLink ("https://youtube.com/watch?v=1"), "looksLikeLink: да");
    check (! Detector::looksLikeLink ("radiohead creep acoustic"), "looksLikeLink: текст — нет");

    auto links = Engine::splitLinks ("https://www.youtube.com/watch?v=dQw4w9WgXcQ\n"
                                     "мусор без ссылки\n"
                                     "https://www.instagram.com/reel/DaikHQxo_Z3/,"
                                     " https://soundcloud.com/forss/flickermood");
    check (links.size() == 3, "splitLinks: 3 ссылки из свалки", std::to_string (links.size()));

    // upToFirst: без маркера строка целиком — иначе однострочные ошибки
    // yt-dlp терялись и человек видел безликое «не удалось».
    check (kd::upToFirst ("ERROR: drm protected", "\n") == "ERROR: drm protected",
        "upToFirst: маркера нет — строка целиком");
    check (kd::upToFirst ("a\nb", "\n") == "a", "upToFirst: до маркера");
}

static void testNames()
{
    std::cout << "имена файлов\n";
    check (Engine::safeName ("AC/DC: Back in Black") == "AC DC  Back in Black", "safeName чистит / и :");
    check (Engine::safeName ("") == "track", "safeName пустого");
    check (Engine::cleanTrackName ("Song (Official Video) [HD]") == "Song", "cleanTrackName сносит мусор");
    check (Engine::cleanTrackName ("Song (feat. Someone)") == "Song (feat. Someone)", "cleanTrackName бережёт фита");
}

static void testYtFileName()
{
    std::cout << "ytFileName — зеркало sanitize_filename yt-dlp\n";
    // Запрещённые символы становятся полноширинными двойниками (проверено
    // офлайн-печатью имени бинаром 2026.08.19), кавычки-ёлочки живы.
    check (Engine::ytFileName ("a/b:c?d\"e<f>f|g*h") == "a⧸b：c？d＂e＜f＞f｜g＊h",
           "запрещённые символы → полноширинные");
    check (Engine::ytFileName ("«Цитата» — трек") == "«Цитата» — трек", "ёлочки и тире не тронуты");
    check (Engine::ytFileName ("back\\slash") == "back⧹slash", "обратный слэш → ⧹");
    check (Engine::ytFileName ("a\tb") == "ab", "таб удалён");
    check (Engine::ytFileName ("Многострочный\nзаголовок") == "Многострочный заголовок", "\\n → пробел");
    // %(title).NB: сырые байты режутся до санитизации, граница UTF-8 целая.
    std::string y60, y59;
    for (int i = 0; i < 60; ++i) y60 += "\xD0\xAF";
    for (int i = 0; i < 59; ++i) y59 += "\xD0\xAF";
    check (Engine::ytFileName (y60 + "/", 120) == y60, "слэш за 120 байтами отрезан");
    check (Engine::ytFileName (y59 + "/", 120) == y59 + "⧸", "слэш в лимите — санитизируется");
}

static void testPredictFiles()
{
    std::cout << "predictFiles — имена и проверка папки\n";
    std::error_code ec;
    const auto tmp = fs::temp_directory_path() / "kd_predict_test";
    fs::create_directories (tmp, ec);
#ifdef _WIN32
    std::cout << "  (диагностика: папка создана, ec=" << ec.message()
              << ", есть: " << (kd::isDir (tmp) ? "да" : "НЕТ") << ")\n";
#endif
    auto dirJson = tmp.u8string();
#ifdef _WIN32
    std::replace (dirJson.begin(), dirJson.end(), '\\', '/'); // JSON-безопасно
#endif

    // Одиночный ролик: шаблон %(title).120B + расширение контейнера + ХРОН.
    char* out = kd_predict_files (nullptr, ("{\"files\":["
        "{\"kind\":\"template\",\"dir\":\"" + dirJson + "\","
        "\"title\":\"Clip: Первый?\",\"service\":0,\"ext\":\"mp4\",\"sections\":\"0:00-0:10\"}]}").c_str());
    std::string res = out ? out : "";
    kd_string_free (out);
    check (res.find ("Clip： Первый？ [00:00–00:10].mp4") != std::string::npos,
           "template: полноширинные + суффикс ХРОНа", res);
    check (res.find ("\"exists\":false") != std::string::npos, "template: файла нет", res);

    // Такой же файл, созданный на диске, — exists = true: проверка по папке,
    // а не по диспетчеру (работает после очистки и перезапуска).
    {
        std::ofstream f (tmp / "Clip： Первый？ [00:00–00:10].mp4");
        f << "x";
#ifdef _WIN32
        std::cout << "  (диагностика: ofstream открыт: " << (f.good() ? "да" : "НЕТ") << ")\n";
#endif
    }
#ifdef _WIN32
    {
        std::error_code ec2;
        const auto probe = kd::u8path (dirJson) / "Clip： Первый？ [00:00–00:10].mp4";
        std::cout << "  (диагностика: файл виден: "
                  << (fs::exists (probe, ec2) ? "да" : "НЕТ")
                  << ", ec=" << ec2.message() << ")\n";
        std::cout << "  (диагностика: содержимое папки)\n";
        for (const auto& entry : fs::directory_iterator (kd::u8path (dirJson)))
            std::cout << "    [" << kd::pathStr (entry.path()) << "]\n";
    }
#endif
    out = kd_predict_files (nullptr, ("{\"files\":["
        "{\"kind\":\"template\",\"dir\":\"" + dirJson + "\","
        "\"title\":\"Clip: Первый?\",\"service\":0,\"ext\":\"mp4\",\"sections\":\"0:00-0:10\"}]}").c_str());
    res = out ? out : "";
    kd_string_free (out);
    check (res.find ("\"exists\":true") != std::string::npos, "template: файл в папке найден", res);

    // Ролик плейлиста: имя задаёт приложение, расширение — формат аудио.
    out = kd_predict_files (nullptr, ("{\"files\":["
        "{\"kind\":\"literal\",\"dir\":\"" + dirJson + "/Плейлист\","
        "\"name\":\"01 - Песня\",\"ext\":\"mp3\"}]}").c_str());
    res = out ? out : "";
    kd_string_free (out);
    check (res.find ("01 - Песня.mp3") != std::string::npos, "literal: имя плейлистного ролика", res);

    // Плейлист внутри пачки: папка подборки + нумерация, титул до 100 байт.
    out = kd_predict_files (nullptr, ("{\"files\":["
        "{\"kind\":\"flat\",\"dir\":\"" + dirJson + "\","
        "\"playlistTitle\":\"Мой: микс\",\"index\":2,\"title\":\"Трек?\",\"ext\":\"mp4\"}]}").c_str());
    res = out ? out : "";
    kd_string_free (out);
    check (res.find ("Мой  микс/02 - Трек？.mp4") != std::string::npos,
           "flat: папка (safeName) + номер + санитизация титула", res);

    // Каталог (Spotify): файл называется «Артист - Трек» из разбора.
    out = kd_predict_files (nullptr, "{\"files\":["
        "{\"kind\":\"template\",\"dir\":\"\",\"title\":\"Forss - Flickermood\",\"service\":6,\"ext\":\"mp3\"}]}");
    res = out ? out : "";
    kd_string_free (out);
    check (res.find ("Forss - Flickermood.mp3") != std::string::npos, "каталог: safeName от разбора", res);

    fs::remove_all (tmp, ec);
}

static void testCAPIPure()
{
    std::cout << "C-API: базовое\n";
    check (std::string (kd_version()) == std::string ("1.0.0"), "kd_version");

    auto links = kd_split_links ("https://youtu.be/jNQXAC9IVRw текст");
    check (std::string (links) == "[\"https://youtu.be/jNQXAC9IVRw\"]", "kd_split_links JSON", links);
    kd_string_free (links);

    kd_engine* e = kd_engine_create (nullptr);
    check (e != nullptr, "kd_engine_create");

    auto snap = kd_snapshot (e);
    check (std::string (snap) == "[]", "kd_snapshot пустой очереди", snap);
    kd_string_free (snap);

    auto dest = kd_default_dest (e);
    std::string destStr = dest;
    check (destStr.find ("K LOAD") != std::string::npos, "kd_default_dest внутри K LOAD", destStr);
    kd_string_free (dest);

    if (kd::isDir (fs::path ("core/tools")))
    {
        auto tools = kd_tools_status (e);
        std::string toolsStr = tools;
        check (toolsStr.find ("\"found\":true") != std::string::npos,
               "kd_tools_status нашёл core/tools", toolsStr);
        kd_string_free (tools);
    }
    else
    {
        std::cout << "  ok (нет core/tools в CI — проверка инструментов пропущена)\n";
    }

    // VPN: просто проверяем валидность ответа, значение зависит от машины.
    const int vpn = kd_vpn_state (e);
    check (vpn >= 0 && vpn <= 2, "kd_vpn_state в диапазоне", std::to_string (vpn));

    kd_engine_destroy (e);
}

// ---- пауза очереди (фейковый загрузчик, без сети) ----

static std::string waitForState (kd_engine* e, int id, const char* stateA, const char* stateB, int seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (seconds);
    for (;;)
    {
        char* snap = kd_snapshot (e);
        std::string s = snap;
        kd_string_free (snap);
        const auto key = s.find ("\"id\":" + std::to_string (id) + ",");
        if (key != std::string::npos)
        {
            const auto a = s.rfind ('{', key);
            const auto b = s.find ('}', key);
            if (a != std::string::npos && b != std::string::npos)
            {
                const auto item = s.substr (a, b - a + 1);
                if (item.find (std::string ("\"state\":\"") + stateA + "\"") != std::string::npos) return item;
                if (stateB != nullptr && item.find (std::string ("\"state\":\"") + stateB + "\"") != std::string::npos) return item;
            }
        }
        if (std::chrono::steady_clock::now() > deadline) return s;
        std::this_thread::sleep_for (std::chrono::milliseconds (500));
    }
}

static void testPauseResume()
{
#ifdef _WIN32
    std::cout << "  ok (posix-only: пауза-стаб на /bin/sh)\n";
    return;
#endif
    std::cout << "C-API: пауза очереди\n";
    const auto tools = fs::temp_directory_path() / "kdcore-stub-tools";
    fs::remove_all (tools);
    fs::create_directories (tools / "ytdlp");
    { std::ofstream f (tools / "ffmpeg"); f << "# приманка для поиска инструментов\n"; }

    // Фейковый загрузчик: до маркера висит 30 с (ловим паузу), после —
    // завершается мгновенно. Реальный yt-dlp по SIGTERM так же бросает
    // .part, который потом докачивается с той же позиции.
    const char* script = R"sh(#!/bin/sh
if [ -f "$KD_STUB_DONE" ]; then
    echo "[download] 100.0% of 1.00MiB"
    exit 0
fi
echo "[download] 5.0% of 1.00MiB"
sleep 30
)sh";
    for (const char* name : { "yt-dlp_macos", "yt-dlp" })
    {
        const auto stub = tools / "ytdlp" / name;
        { std::ofstream f (stub); f << script; }
        fs::permissions (stub, fs::perms::owner_exec, fs::perm_options::add);
    }

    const auto out = fs::temp_directory_path() / "kdcore-stub-out";
    fs::remove_all (out);
    fs::create_directories (out);
    const std::string dest = kd::pathStr (out);
    const std::string opts = "{\"dest\":\"" + dest + "\",\"mode\":\"video\"}";

    kd_engine* e = kd_engine_create (kd::pathStr (tools).c_str());
    check (e != nullptr, "движок на фейковых инструментах");

    check (kd_enqueue_batch (e,
        "[\"https://example.com/video1\",\"https://example.com/video2\"]",
        opts.c_str()) == 2, "enqueue двух заданий");

    // Параллельность: два воркера забирают оба задания сразу.
    const auto w1 = waitForState (e, 1, "working", nullptr, 15);
    const auto w2 = waitForState (e, 2, "working", nullptr, 15);
    check (w1.find ("\"state\":\"working\"") != std::string::npos, "воркер 1 взял задание");
    check (w2.find ("\"state\":\"working\"") != std::string::npos,
        "воркер 2 взял второе задание не дожидаясь первого");

    kd_set_paused (e, 1);
    check (kd_is_paused (e) == 1, "kd_is_paused на паузе");
    const auto p1 = waitForState (e, 1, "paused", nullptr, 15);
    const auto p2 = waitForState (e, 2, "paused", nullptr, 15);
    check (p1.find ("\"state\":\"paused\"") != std::string::npos
        && p1.find ("ПРИОСТАНОВИЛ") != std::string::npos,
        "задание 1 приостановлено", p1.substr (0, 300));
    check (p2.find ("\"state\":\"paused\"") != std::string::npos,
        "задание 2 приостановлено", p2.substr (0, 300));

    // [ОЧИСТИТЬ] историю не должен трогать приостановленное.
    kd_clear_finished (e);
    {
        char* snap = kd_snapshot (e);
        const std::string s = snap;
        check (s.find ("\"state\":\"paused\"") != std::string::npos,
            "clearFinished бережёт приостановленные", s);
        kd_string_free (snap);
    }

    // Отмена приостановленного срабатывает сразу, а не после возобновления.
    kd_cancel (e, 1);
    const auto c1 = waitForState (e, 1, "failed", nullptr, 5);
    check (c1.find ("\"state\":\"failed\"") != std::string::npos
        && c1.find ("Отменено") != std::string::npos,
        "отмена приостановленного — сразу", c1.substr (0, 300));

    // Возобновление: маркер-файл заставит приманку завершиться мгновенно.
    { const std::ofstream f (out / "done.marker"); }
    testSetenv ("KD_STUB_DONE", (fs::path (out) / "done.marker").string().c_str());
    kd_set_paused (e, 0);
    check (kd_is_paused (e) == 0, "kd_is_paused после возобновления");
    const auto r2 = waitForState (e, 2, "done", "failed", 30);
    check (r2.find ("\"state\":\"done\"") != std::string::npos,
        "возобновлённое задание докачалось", r2.substr (0, 300));
    testUnsetenv ("KD_STUB_DONE");

    // Выход приложения на паузе не должен зависать: воркеры спят в паузе,
    // деструктор должен разбудить каждого.
    {
        kd_engine* e2 = kd_engine_create (kd::pathStr (tools).c_str());
        kd_enqueue_batch (e2, "[\"https://example.com/v\"]", opts.c_str());
        (void) waitForState (e2, 1, "working", nullptr, 15);
        kd_set_paused (e2, 1);
        (void) waitForState (e2, 1, "paused", nullptr, 15);
        auto destroyed = std::async (std::launch::async,
            [e2] { kd_engine_destroy (e2); });
        check (destroyed.wait_for (std::chrono::seconds (5)) == std::future_status::ready,
            "destroy на паузе не зависает");
    }

    kd_engine_destroy (e);
    testUnsetenv ("K_LOAD_TOOLS"); // живые тесты ниже ищут инструменты сами
    fs::remove_all (tools);
    fs::remove_all (out);
}

// ---- живые источники ----

static void testLiveProbes (kd_engine* e)
{
    std::cout << "живой probe\n";

    auto p = kd_probe_blocking (e, "https://www.youtube.com/watch?v=jNQXAC9IVRw");
    std::string ps = p;
    check (ps.find ("\"ok\":true") != std::string::npos, "probe YouTube ok", ps.substr (0, 200));
    check (ps.find ("\"title\":\"Me at the zoo\"") != std::string::npos, "probe YouTube title", ps.substr (0, 200));
    // «Макс. качество» честно: показываем только реально доступные высоты
    // (у «Me at the zoo» их две — 240p и 360p, никакого 1080).
    {
        const auto h = ps.find ("\"heights\":[");
        const bool real = h != std::string::npos
            && ps.substr (h, 40).find ("2") != std::string::npos;
        check (real, "probe YouTube высоты (реальные, без выдуманных)");
    }
    kd_string_free (p);

    p = kd_probe_blocking (e, "https://soundcloud.com/forss/flickermood");
    ps = p;
    check (ps.find ("\"ok\":true") != std::string::npos, "probe SoundCloud ok", ps.substr (0, 200));
    check (ps.find ("Flickermood") != std::string::npos, "probe SoundCloud title");
    kd_string_free (p);

    p = kd_probe_blocking (e, "a-ha take on me");
    ps = p;
    check (ps.find ("\"isSearch\":true") != std::string::npos, "probe поиск: isSearch", ps.substr (0, 200));
    check (ps.find ("\"resolved\":\"https://www.youtube.com/watch?v=djV11Xbc914\"") != std::string::npos
        || ps.find ("\"resolved\":\"https://www.youtube.com/watch?") != std::string::npos,
        "probe поиск нашёл ролик", ps.substr (0, 200));
    kd_string_free (p);

    p = kd_probe_blocking (e, "https://www.pinterest.com/pin/1004795366878354024/");
    ps = p;
    check (ps.find ("\"ok\":true") != std::string::npos, "probe Pinterest ok", ps.substr (0, 200));
    check (ps.find ("\"isPhoto\":true") != std::string::npos || ps.find ("\"thumbnail\":\"http") != std::string::npos,
        "probe Pinterest фото/превью", ps.substr (0, 200));
    kd_string_free (p);
}

static void testLiveDownload (kd_engine* e)
{
    std::cout << "живые скачивания\n";
    const auto tmp = fs::temp_directory_path() / "kdcore-live" / "K LOAD";
    fs::remove_all (fs::temp_directory_path() / "kdcore-live");
    fs::create_directories (tmp);
    const std::string dest = kd::pathStr (tmp);

    // Видео YouTube (коротчайший «Me at the zoo»).
    std::string links = "[\"https://www.youtube.com/watch?v=jNQXAC9IVRw\"]";
    const std::string opts = "{\"dest\":\"" + dest + "\",\"mode\":\"video\",\"quality\":\"1080\"}";
    check (kd_enqueue_batch (e, links.c_str(), opts.c_str()) == 1, "enqueue youtube");

    const auto item1 = waitForState (e, 1, "done", "failed", 180);
    check (item1.find ("\"state\":\"done\"") != std::string::npos, "youtube скачан", item1.substr (0, 300));
    check (item1.find ("\"files\":[\"/") != std::string::npos, "youtube путь файла выдан", item1.substr (0, 300));

    // Аудио SoundCloud → mp3.
    links = "[\"https://soundcloud.com/forss/flickermood\"]";
    const std::string optsAudio = "{\"dest\":\"" + dest + "\",\"mode\":\"audio\",\"audioFormat\":\"mp3\"}";
    check (kd_enqueue_batch (e, links.c_str(), optsAudio.c_str()) == 1, "enqueue soundcloud");

    const auto item2 = waitForState (e, 2, "done", "failed", 180);
    check (item2.find ("\"state\":\"done\"") != std::string::npos, "soundcloud скачан", item2.substr (0, 300));
    check (item2.find (".mp3\"") != std::string::npos, "soundcloud отдал mp3", item2.substr (0, 300));

    // Пачка из двух ссылок.
    links = "[\"https://www.youtube.com/watch?v=jNQXAC9IVRw\",\"https://youtu.be/jNQXAC9IVRw\"]";
    check (kd_enqueue_batch (e, links.c_str(), opts.c_str()) == 2, "enqueue пачка из 2");

    const auto item3 = waitForState (e, 3, "done", "failed", 180);
    const auto item4 = waitForState (e, 4, "done", "failed", 180);
    check (item3.find ("\"state\":\"done\"") != std::string::npos && item3.find ("\"batchTotal\":2") != std::string::npos
        && item3.find ("\"batchIndex\":1") != std::string::npos, "пачка: первое задание 1/2", item3.substr (0, 300));
    check (item4.find ("\"state\":\"done\"") != std::string::npos && item4.find ("\"batchIndex\":2") != std::string::npos,
        "пачка: второе задание 2/2", item4.substr (0, 300));

    // Ролик в пачке тот же, что и первое задание: повтор не дублируется
    // («уже скачано»), поэтому ждём mp4 от первого задания и mp3 от аудио.
    bool sawMp4 = false, sawMp3 = false;
    for (const auto& entry : fs::recursive_directory_iterator (tmp))
        if (entry.is_regular_file())
        {
            const auto ext = entry.path().extension().string();
            if (ext == ".mp4") sawMp4 = true;
            if (ext == ".mp3") sawMp3 = true;
        }
    check (sawMp4 && sawMp3, "в папке назначения mp4 и mp3");

    kd_clear_finished (e);
    char* snap = kd_snapshot (e);
    check (std::string (snap) == "[]", "clearFinished опустошил очередь");
    kd_string_free (snap);

    // ХРОН: отрезок 0–2 секунды того же короткого ролика. Секции доезжают
    // в задание и файл получается (многократно меньше полного).
    links = "[\"https://www.youtube.com/watch?v=jNQXAC9IVRw\"]";
    const std::string optsChron = "{\"dest\":\"" + dest
        + "\",\"mode\":\"video\",\"sections\":\"0:00-0:02\"}";
    check (kd_enqueue_batch (e, links.c_str(), optsChron.c_str()) == 1, "enqueue с sections");

    char* snapChron = kd_snapshot (e);
    check (std::string (snapChron).find ("\"sections\":\"0:00-0:02\"") != std::string::npos,
        "sections видны в снапшоте", snapChron);
    kd_string_free (snapChron);

    const auto item5 = waitForState (e, 5, "done", "failed", 180);
    check (item5.find ("\"state\":\"done\"") != std::string::npos, "хрон-отрезок скачан", item5.substr (0, 300));
    std::uintmax_t fullSize = 0, cutSize = 0;
    for (const auto& entry : fs::recursive_directory_iterator (tmp))
        if (entry.is_regular_file() && entry.path().extension().string() == ".mp4")
        {
            const auto size = entry.file_size();
            if (size > fullSize) { cutSize = fullSize; fullSize = size; }
            else cutSize = std::max (cutSize, size);
        }
    check (cutSize > 0 && cutSize < fullSize, "отрезок меньше целого ролика",
        std::to_string (cutSize) + " < " + std::to_string (fullSize));
}

// Инъекции в маркерах @T/@F (аудит KL-002/005/007): чужие и старые пути
// не попадают в files[], «|» в названии и пути не рвут данные.
static void testConsumeMarkers()
{
#ifdef _WIN32
    // Стаб-заглушка — скрипт /bin/sh: на Windows не исполняется. Эти потоки
    // (запуск/пауза/маркеры) на Windows покрывают integration-тесты и
    // ручная приёмка на реальном yt-dlp.
    std::cout << "  ok (posix-only: consume-стаб на /bin/sh)\n";
    return;
#endif
    const auto tools = fs::temp_directory_path() / "kd-consume-tools";
    const auto dest = fs::temp_directory_path() / "kd-consume-out";
    const auto outside = fs::temp_directory_path() / "kd-consume-outside";
    std::error_code ec;
    fs::remove_all (tools, ec); fs::remove_all (dest, ec); fs::remove_all (outside, ec);
    fs::create_directories (tools / "ytdlp");
    fs::create_directories (outside);
    { std::ofstream f (tools / "ffmpeg"); f << "# приманка\n"; }

    const auto stub = tools / "ytdlp" / "yt-dlp_macos";
    {
        std::ofstream f (stub);
        f << "#!/bin/sh\n"
          << "prev=\"\"\n"
          << "for a in \"$@\"; do if [ \"$prev\" = \"-P\" ]; then d=\"$a\"; fi; prev=\"$a\"; done\n"
          << "mkdir -p \"$d\"\n"
          << "printf x > \"" << outside.u8string() << "/evil.mp4\"\n"
          << "printf x > \"$d/Old.mp4\"\n"
          << "touch -t 202001010000 \"$d/Old.mp4\"\n"
          << "printf x > \"$d/a|b Fresh.mp4\"\n"
          << "echo \"@T|1|1|Название с | пайпом\"\n"
          << "echo \"@F|" << outside.u8string() << "/evil.mp4\"\n"
          << "echo \"@F|$d/Old.mp4\"\n"
          << "echo \"@F|$d/a|b Fresh.mp4\"\n"
          << "echo \"[download] 50.0% of 1.00MiB\"\n"
          << "exit 0\n";
    }
    fs::permissions (stub, fs::perms::owner_exec, fs::perm_options::add);

    const std::string opts = "{\"dest\":\"" + dest.u8string() + "\",\"mode\":\"video\"}";
    kd_engine* e = kd_engine_create (kd::pathStr (tools).c_str());
    check (kd_enqueue_batch (e, "[\"https://example.com/inject\"]", opts.c_str()) == 1,
        "consume: задание поставлено");

    const auto snap = waitForState (e, 1, "done", "failed", 30);
    check (snap.find ("\"state\":\"done\"") != std::string::npos,
        "consume: задание завершилось", snap.substr (0, 200));
    check (snap.find ("evil.mp4") == std::string::npos,
        "consume: путь вне папки назначения отклонён", snap.substr (0, 400));
    check (snap.find ("Old.mp4") == std::string::npos,
        "consume: файл со старым временем отклонён", snap.substr (0, 400));
    check (snap.find ("a|b Fresh.mp4") != std::string::npos,
        "consume: «|» в названии и пути не рвут данные", snap.substr (0, 400));
    check (snap.find ("Название с ") == std::string::npos,
        "consume: заголовок с «|» не обрезал данные до первой черты", snap.substr (0, 400));
    kd_engine_destroy (e);
    fs::remove_all (tools, ec); fs::remove_all (dest, ec); fs::remove_all (outside, ec);
}

int main (int argc, char** argv)
{
    // Небуферизованный вывод: при крэше видно, на каком тесте.
    setvbuf (stdout, nullptr, _IONBF, 0);
    const bool live = argc > 1 && std::string (argv[1]) == "--live";

    try
    {
        testDetector();
        testLinkLogic();
        testNames();
        testYtFileName();
        testPredictFiles();
        testConsumeMarkers();
        testCAPIPure();
        testPauseResume();

        if (live)
        {
            kd_engine* e = kd_engine_create (nullptr);
            testLiveProbes (e);
            testLiveDownload (e);
            kd_engine_destroy (e);
        }
    }
    catch (const std::exception& e)
    {
        std::cout << "EXCEPTION: " << e.what() << "\n";
        return 2;
    }
    catch (...)
    {
        std::cout << "EXCEPTION: неизвестная\n";
        return 2;
    }

    std::cout << "\nитого: " << passed << " ok, " << failed << " fail\n";
    return failed == 0 ? 0 : 1;
}
