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
    // getenv возвращает байты в ANSI: на машинах с кириллическим профилем
    // u8path прочитала бы их как UTF-8, и путь мутировал (U+FFFD) —
    // загрузки падали, журнал молча не писался. Только wide-API.
    const DWORD need = ::GetEnvironmentVariableW (L"USERPROFILE", nullptr, 0);
    if (need > 0 && need <= 4096)
    {
        std::wstring v (need, L'\0');
        ::GetEnvironmentVariableW (L"USERPROFILE", v.data(), need);
        v.resize (::wcslen (v.c_str()));
        if (! v.empty()) return fs::path (v);
    }
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
