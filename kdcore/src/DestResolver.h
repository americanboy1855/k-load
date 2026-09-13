#pragma once

// Куда складывать по умолчанию в приложении: «Загрузки», а внутри —
// подпапка «K LOAD», чтобы не сыпать файлы прямо в общую свалку.
//
// (Детектор свежего проекта DAW из прежнего ядра — забота этапа плагина:
// вернём его туда, где он нужен.)

#include "kd_compat.h"

#include <cstdlib>

namespace DestResolver
{

// Подпапка, куда складывается скачанное.
inline Str folderName() { return "K LOAD"; }

inline fs::path homeDir()
{
#ifdef _WIN32
    if (const char* profile = ::getenv ("USERPROFILE"))
        if (*profile != '\0') return kd::u8path (profile);
#else
    if (const char* home = ::getenv ("HOME"))
        if (*home != '\0') return kd::u8path (home);
#endif
    return fs::path (".");
}

// База (без подпапки). Загрузок нет (нестандартная система) — Документы.
inline fs::path baseFolder()
{
    const auto downloads = homeDir() / "Downloads";
    return kd::isDir (downloads) ? downloads : homeDir() / "Documents";
}

// Папка по умолчанию целиком: база + «K LOAD».
inline fs::path defaultFolder()
{
    return baseFolder() / folderName();
}

} // namespace DestResolver
