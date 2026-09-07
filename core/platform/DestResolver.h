#pragma once

#include <juce_core/juce_core.h>

// Куда складывать по умолчанию.
//
// В standalone это «Загрузки/K DOWNLOADER». В плагине человек ждёт, что
// файлы окажутся рядом с проектом, открытым в DAW: стандартного способа
// спросить у хоста этот путь не существует, поэтому ищем сами — по свежим
// файлам проектов в стандартных местах. Свежий проект почти всегда прав:
// открыли тот, что вчера — его и найдём. Не нашли — Загрузки.
//
// В обоих случаях создаётся подпапка «K DOWNLOADER», чтобы не сыпать файлы
// впрямую в проект или в общую свалку Загрузок.
namespace DestResolver
{

inline bool isPlugin()
{
#if JUCE_STANDALONE_APPLICATION
    return false;
#else
    return true;
#endif
}

// Подпапка, куда складывается скачанное.
inline juce::String folderName() { return "K DOWNLOADER"; }

static const juce::StringArray& projectExtensions()
{
    static const juce::StringArray ext { "als", "logicx", "flp", "cpr", "song",
                                         "rpp", "ptx", "bwproject", "band",
                                         "drp", "aup3", "reason" };
    return ext;
}

// Ограниченная по глубине и по времени прогулка: findChildFiles умеет
// только «всё подряд», а это лишний обход целых медиатек. Дедлайн нужен,
// чтобы редактор плагина в DAW не ждал обхода больших папок.
inline void walk (const juce::File& dir, int depth,
                  const juce::StringArray& ext, juce::File& newest, juce::Time& newestTime,
                  juce::int64 deadlineMs = 0)
{
    if (depth <= 0 || ! dir.isDirectory()) return;
    if (deadlineMs > 0 && juce::Time::getMillisecondCounter() > deadlineMs) return;

    for (const auto& f : dir.findChildFiles (juce::File::findFilesAndDirectories
                                                 | juce::File::ignoreHiddenFiles, false))
    {
        const auto name = f.getFileName();
        if (name.startsWith (".") || name == "Library") continue;

        const auto e = f.getFileExtension().toLowerCase().fromFirstOccurrenceOf (".", false, false);
        if (ext.contains (e))
        {
            // .logicx и иже с ними — папки-пакеты: сами в них файлы не кладём,
            // берём папку выше.
            const auto target = f.getParentDirectory();
            const auto t = f.getLastModificationTime();
            if (newest == juce::File() || t > newestTime) { newest = target; newestTime = t; }
        }
        if (f.isDirectory())
        {
            walk (f, depth - 1, ext, newest, newestTime, deadlineMs);
            if (deadlineMs > 0 && juce::Time::getMillisecondCounter() > deadlineMs) return;
        }
    }
}

// Свежая папка проекта по всем известным DAW сразу. Пустой результат —
// проекта не нашли. Не звать из потоков, от которых нельзя ждать: на
// больших библиотеках занимает до пары секунд.
inline juce::File detectDawProjectFolder()
{
    juce::File newest;
    juce::Time newestTime;
    const auto music   = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    const auto docs    = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    const auto desktop = juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
    const auto deadline = juce::Time::getMillisecondCounter() + 4000;

    walk (music,   3, projectExtensions(), newest, newestTime, deadline);
    walk (docs,    3, projectExtensions(), newest, newestTime, deadline);
    walk (desktop, 2, projectExtensions(), newest, newestTime, deadline);

    // Проект старше месяца папкой считать не стоит: вероятно, это архив.
    if (newest != juce::File()
        && newestTime < juce::Time::getCurrentTime() - juce::RelativeTime::days (30))
        return {};
    return newest;
}

// База (без подпапки): у плагина — проект DAW, у приложения — Загрузки.
inline juce::File baseFolder()
{
    if (isPlugin())
        if (auto project = detectDawProjectFolder(); project != juce::File())
            return project;
    const auto downloads = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                               .getChildFile ("Downloads");
    return downloads.isDirectory() ? downloads
        : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
}

// Папка по умолчанию целиком: база + «K DOWNLOADER».
inline juce::File defaultFolder()
{
    return baseFolder().getChildFile (folderName());
}

} // namespace DestResolver
