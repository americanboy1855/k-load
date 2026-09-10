#pragma once

// Что за сервис по ссылке и как его качать. Порт core/engine/Detector.h
// 1:1, juce-типы заменены на std.

#include "kd_compat.h"
#include "kd_url.h"

namespace Detector
{

using namespace kd; // строковые хелперы ядра без префикса внутри Detector

enum class Service
{
    youtube, youtubeMusic, instagram, tiktok, pinterest, vk,
    spotify, appleMusic, vkMusic, soundcloud, unknown
};

enum class Route
{
    native,            // yt-dlp справится сам
    resolveThenSearch, // каталог закрыт: читаем название, ищем на YouTube/SoundCloud
    photo              // Pinterest: yt-dlp бессилен, пробуем oEmbed и качаем картинку
};

inline Service serviceFor (Str raw)
{
    raw = trim (raw);
    if (raw.empty()) return Service::unknown;

    const auto url = parseUrl (raw);
    const auto& host = url.host;
    const auto& path = url.path;

    if (endsWith (host, "music.youtube.com"))                        return Service::youtubeMusic;
    if (endsWith (host, "youtube.com") || endsWith (host, "youtu.be")
        || endsWith (host, "youtube-nocookie.com"))                  return Service::youtube;
    if (endsWith (host, "instagram.com") || endsWith (host, "instagr.am")) return Service::instagram;
    if (endsWith (host, "tiktok.com"))                               return Service::tiktok;
    if (endsWith (host, "pinterest.com") || endsWith (host, "pin.it")
        || contains (host, "pinterest."))                            return Service::pinterest;
    if (endsWith (host, "spotify.com") || endsWith (host, "spotify.link")) return Service::spotify;
    if (endsWith (host, "music.apple.com") || endsWith (host, "itunes.apple.com")) return Service::appleMusic;
    if (endsWith (host, "soundcloud.com"))                           return Service::soundcloud;
    // ВК: музыка и видео на одном домене, разводим по адресу страницы.
    if (endsWith (host, "vk.com") || endsWith (host, "vkvideo.ru")
        || endsWith (host, "vk.ru") || endsWith (host, "vkontakte.ru"))
        return (contains (path, "/audio") || contains (path, "/music"))
                   ? Service::vkMusic : Service::vk;
    return Service::unknown;
}

// Сервисы, где звук напрямую не отдаётся никому: работаем через название трека.
inline bool needsResolve (Service s)
{
    return s == Service::spotify || s == Service::appleMusic
        || s == Service::vkMusic;
}

// Сервисы, где без входа в аккаунт видны только открытые материалы.
// По заданию качаем именно их: сначала без входа, затем невидимо с входом
// из браузеров, и только то, что доступно без приватности.
inline bool cookieSensitive (Service s)
{
    return s == Service::instagram || s == Service::tiktok
        || s == Service::vk || s == Service::youtube;
}

inline Str title (Service s)
{
    switch (s)
    {
        case Service::youtube:      return "YouTube";
        case Service::youtubeMusic: return "YouTube Music";
        case Service::instagram:    return "Instagram";
        case Service::tiktok:       return "TikTok";
        case Service::pinterest:    return "Pinterest";
        case Service::vk:           return "ВКонтакте";
        case Service::spotify:      return "Spotify";
        case Service::appleMusic:   return "Apple Music";
        case Service::vkMusic:      return "ВК Музыка";
        case Service::soundcloud:   return "SoundCloud";
        case Service::unknown:      return "Сайт";
    }
    return {};
}

// У ссылки есть плейлист вообще (включая ролик внутри плейлиста).
inline bool hasPlaylist (Str raw)
{
    const auto low = lower (raw);
    return contains (low, "list=") || contains (low, "/playlist")
        || contains (low, "/album/") || contains (low, "/sets/");
}

// Ссылка ведёт на подборку целиком. Ролик, открытый внутри плейлиста
// (watch?v=…&list=…), подборкой НЕ считается — качаем именно его.
inline bool isCollection (Str raw)
{
    const auto low = lower (raw);
    if (contains (low, "watch?v=") || contains (low, "youtu.be/")) return false;
    return contains (low, "/playlist") || contains (low, "list=")
        || contains (low, "/album/") || contains (low, "/sets/");
}

// Похоже ли на ссылку. Мусор и названия треков батчем не качаем.
inline bool looksLikeLink (Str raw)
{
    raw = trim (raw);
    return contains (raw, ".") && raw.find ('/') != Str::npos;
}

} // namespace Detector
