#pragma once

// Замена juce-типов на чистый C++17. Сюда сходят все строковые и файловые
// операции, которыми пользовалось ядро: сами правила скачивания от этого
// слоя не зависят.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace kd
{

using Str = std::string;
using StrVec = std::vector<Str>;

// ---- строки ----

inline Str trim (const Str& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr (a, b - a);
}

inline Str trimStart (const Str& s, const char* chars = " \r\t")
{
    const size_t a = s.find_first_not_of (chars);
    return a == Str::npos ? Str() : s.substr (a);
}

inline Str trimEnd (const Str& s, const char* chars = " \r\t")
{
    const size_t b = s.find_last_not_of (chars);
    return b == Str::npos ? Str() : s.substr (0, b + 1);
}

inline Str lower (const Str& s)
{
    Str out = s;
    for (auto& c : out)
        c = (char) std::tolower ((unsigned char) c);
    return out;
}

inline bool contains (const Str& s, const Str& needle) { return s.find (needle) != Str::npos; }

inline bool containsAny (const Str& s, std::initializer_list<const char*> needles)
{
    for (const auto* n : needles)
        if (contains (s, n)) return true;
    return false;
}

inline bool startsWith (const Str& s, const Str& p)
{
    return s.size() >= p.size() && s.compare (0, p.size(), p) == 0;
}

inline bool endsWith (const Str& s, const Str& p)
{
    return s.size() >= p.size() && s.compare (s.size() - p.size(), p.size(), p) == 0;
}

// Хвост после первого вхождения marker (вхождение не включается).
inline Str fromFirst (const Str& s, const Str& marker)
{
    const auto p = s.find (marker);
    return p == Str::npos ? Str() : s.substr (p + marker.size());
}

// Хвост после последнего вхождения marker.
inline Str fromLast (const Str& s, const Str& marker)
{
    const auto p = s.rfind (marker);
    return p == Str::npos ? Str() : s.substr (p + marker.size());
}

// Голова до первого вхождения marker (вхождение не включается).
// Маркера нет — строка целиком: однострочная ошибка yt-dlp не должна
// превращаться в пустоту (именно так «DRM protected» стал «не удалось»).
inline Str upToFirst (const Str& s, const Str& marker)
{
    const auto p = s.find (marker);
    return p == Str::npos ? s : s.substr (0, p);
}

// Первое совпадение регулярного выражения (std::regex search), иначе пусто.
inline Str searchRegex (const Str& s, const Str& pattern)
{
    try
    {
        std::regex re (pattern);
        std::smatch m;
        if (std::regex_search (s, m, re) && m.size() > 0)
            return m[0].str();
    }
    catch (...) { /* битый паттерн — пусто */ }
    return {};
}

inline int indexOf (const Str& s, const Str& needle, size_t from = 0)
{
    const auto p = s.find (needle, from);
    return p == Str::npos ? -1 : (int) p;
}

inline int indexOfChar (const Str& s, char c, size_t from = 0)
{
    const auto p = s.find (c, from);
    return p == Str::npos ? -1 : (int) p;
}

// Первый символ из набора chars; pos — позиция, иначе -1.
inline int indexOfAnyOf (const Str& s, const char* chars, size_t from = 0)
{
    const auto p = s.find_first_of (chars, from);
    return p == Str::npos ? -1 : (int) p;
}

inline bool containsOnly (const Str& s, const char* charset)
{
    if (s.empty()) return false;
    return s.find_first_not_of (charset) == Str::npos;
}

inline Str replaceAll (Str s, const Str& from, const Str& to)
{
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find (from, pos)) != Str::npos)
    {
        s.replace (pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

inline Str replaceChar (Str s, char from, char to)
{
    std::replace (s.begin(), s.end(), from, to);
    return s;
}

inline int getInt (const Str& s)
{
    return std::atoi (trim (s).c_str());
}

inline double getDouble (const Str& s)
{
    return std::atof (trim (s).c_str());
}

// Разбиение по любым из breakChars; кавычки не отслеживает — ссылкам в
// пачке это не нужно.
inline StrVec splitTokens (const Str& s, const char* breakChars)
{
    StrVec out;
    size_t pos = 0;
    while (pos < s.size())
    {
        const auto next = s.find_first_of (breakChars, pos);
        if (next != pos)
            out.push_back (s.substr (pos, next - pos));
        if (next == Str::npos) break;
        pos = next + 1;
    }
    return out;
}

inline StrVec splitWhitespace (const Str& s)
{
    return splitTokens (s, " \t\r\n");
}

inline StrVec splitLines (const Str& s)
{
    StrVec out;
    size_t pos = 0;
    while (pos <= s.size())
    {
        const auto next = s.find ('\n', pos);
        auto line = s.substr (pos, next == Str::npos ? Str::npos : next - pos);
        while (! line.empty() && (line.back() == '\r')) line.pop_back();
        out.push_back (line);
        if (next == Str::npos) break;
        pos = next + 1;
    }
    return out;
}

inline Str join (const StrVec& v, const Str& sep)
{
    Str out;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i > 0) out += sep;
        out += v[i];
    }
    return out;
}

inline bool containsVec (const StrVec& v, const Str& s)
{
    return std::find (v.begin(), v.end(), s) != v.end();
}

// Процентное кодирование для подстановки в адрес страницы (как
// juce::URL::addEscapeChars): небезопасные байты превращаются в %XX.
inline Str urlEscape (const Str& s)
{
    static const char* hex = "0123456789ABCDEF";
    Str out;
    for (const unsigned char c : s)
    {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                       || (c >= '0' && c <= '9')
                       || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) out += (char) c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// ---- файлы ----

// UTF-8 везде: .string() на Windows конвертирует в системную ANSI-кодировку
// и кириллица в путях (названия треков!) бросает исключение или портится.
inline Str pathStr (const fs::path& p)
{
#ifdef _WIN32
    return p.u8string();
#else
    return p.string();
#endif
}

inline fs::path childFile (const fs::path& dir, const Str& name) { return dir / fs::u8path (name); }

inline bool isFile (const fs::path& p)
{
    std::error_code ec;
    return fs::is_regular_file (p, ec);
}

inline bool isDir (const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory (p, ec);
}

inline void ensureDir (const fs::path& p)
{
    std::error_code ec;
    fs::create_directories (p, ec);
}

inline Str fileName (const fs::path& p)
{
#ifdef _WIN32
    return p.filename().u8string();
#else
    return p.filename().string();
#endif
}

inline Str stem (const fs::path& p)
{
#ifdef _WIN32
    return p.stem().u8string();
#else
    return p.stem().string();
#endif
}

} // namespace kd

// Типы строки и вектора строк нужны всем внутренним единицам ядра без
// префикса. BrowserDefault.mm глобальный Str не использует: там он занят
// MacTypes, и файл пишет kd:: по-честному.
using kd::Str;
using kd::StrVec;
