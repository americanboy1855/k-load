#pragma once

// Куда складывать по умолчанию в приложении: «Загрузки», а внутри —
// подпапка «K DWNLD», чтобы не сыпать файлы прямо в общую свалку.
//
// (Детектор свежего проекта DAW из прежнего ядра — забота этапа плагина:
// вернём его туда, где он нужен.)

#include "kd_compat.h"

#include <cstdlib>

namespace DestResolver
{

// Подпапка, куда складывается скачанное.
inline Str folderName() { return "K DWNLD"; }

inline fs::path homeDir()
{
    if (const char* home = ::getenv ("HOME"))
        if (*home != '\0') return fs::u8path (home);
    return fs::path ("~");
}

// База (без подпапки). Загрузок нет (нестандартная система) — Документы.
inline fs::path baseFolder()
{
    const auto downloads = homeDir() / "Downloads";
    return kd::isDir (downloads) ? downloads : homeDir() / "Documents";
}

// Папка по умолчанию целиком: база + «K DWNLD».
inline fs::path defaultFolder()
{
    return baseFolder() / folderName();
}

} // namespace DestResolver
