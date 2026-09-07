#pragma once

// Минимальный разбор адреса — ровно то, что ядро спрашивало у juce::URL:
// схема, домен (в нижнем регистре), путь (в нижнем регистре), параметры.

#include "kd_compat.h"

namespace kd
{

struct UrlParts
{
    Str scheme, host, path, query;   // host и path — в нижнем регистре
};

inline UrlParts parseUrl (Str raw)
{
    raw = trim (raw);
    if (raw.empty()) return {};

    if (! startsWith (lower (raw), "http://") && ! startsWith (lower (raw), "https://"))
        raw = "https://" + raw;

    UrlParts u;
    auto rest = raw;
    {
        const auto sep = rest.find ("://");
        if (sep != Str::npos)
        {
            u.scheme = lower (rest.substr (0, sep));
            rest = rest.substr (sep + 3);
        }
    }
    {
        const auto slash = rest.find ('/');
        u.host = lower (slash == Str::npos ? rest : rest.substr (0, slash));
        const auto addrSlash = raw.find ("://") + 3;
        const auto full = raw.substr (addrSlash);
        const auto q = full.find ('?');
        if (q != Str::npos)
        {
            u.path = lower (full.substr (0, q));
            u.query = full.substr (q + 1);
        }
        else
        {
            u.path = lower (full);
        }
    }
    // www. и m. не меняют, какой это сервис.
    if (startsWith (u.host, "www.")) u.host = u.host.substr (4);
    if (startsWith (u.host, "m."))   u.host = u.host.substr (2);
    return u;
}

// Значение параметра из query (?i=123&x=y). Пусто — параметра нет.
inline Str queryParam (const Str& query, const Str& name)
{
    for (const auto& pair : splitTokens (query, "&"))
    {
        const auto eq = pair.find ('=');
        if (eq == Str::npos) continue;
        if (pair.substr (0, eq) == name)
            return pair.substr (eq + 1);
    }
    return {};
}

} // namespace kd

using kd::parseUrl;
