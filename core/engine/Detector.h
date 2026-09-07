#pragma once

#include <juce_core/juce_core.h>
#include "../KDText.h"

// Что за сервис по ссылке и как его качать.
namespace Detector
{
enum class Service
{
    youtube, youtubeMusic, instagram, tiktok, pinterest, vk,
    spotify, appleMusic, yandexMusic, vkMusic, soundcloud, unknown
};

enum class Route
{
    native,            // yt-dlp справится сам
    resolveThenSearch, // каталог закрыт: читаем название, ищем на YouTube/SoundCloud
    photo              // Pinterest: yt-dlp бессилен, пробуем oEmbed и качаем картинку
};

inline Service serviceFor (juce::String raw)
{
    raw = raw.trim();
    if (raw.isEmpty()) return Service::unknown;

    auto host = [&]() -> juce::String
    {
        auto normalized = raw.startsWith ("http") ? raw : "https://" + raw;
        auto h = juce::URL (normalized).getDomain().toLowerCase();
        h = h.startsWith ("www.") ? h.fromFirstOccurrenceOf ("www.", false, false) : h;
        h = h.startsWith ("m.")   ? h.fromFirstOccurrenceOf ("m.",   false, false) : h;
        return h;
    }();

    const auto path = juce::URL (raw.startsWith ("http") ? raw : "https://" + raw)
                          .getSubPath().toLowerCase();

    if (host.endsWith ("music.youtube.com"))                        return Service::youtubeMusic;
    if (host.endsWith ("youtube.com") || host.endsWith ("youtu.be")
        || host.endsWith ("youtube-nocookie.com"))                  return Service::youtube;
    if (host.endsWith ("instagram.com") || host.endsWith ("instagr.am")) return Service::instagram;
    if (host.endsWith ("tiktok.com"))                               return Service::tiktok;
    if (host.endsWith ("pinterest.com") || host.endsWith ("pin.it")
        || host.contains ("pinterest."))                            return Service::pinterest;
    if (host.endsWith ("spotify.com") || host.endsWith ("spotify.link")) return Service::spotify;
    if (host.endsWith ("music.apple.com") || host.endsWith ("itunes.apple.com")) return Service::appleMusic;
    if (host.endsWith ("music.yandex.ru") || host.endsWith ("music.yandex.com")
        || host.endsWith ("music.yandex.by") || host.endsWith ("music.yandex.kz")) return Service::yandexMusic;
    if (host.endsWith ("soundcloud.com"))                           return Service::soundcloud;
    // ВК: музыка и видео на одном домене, разводим по адресу страницы.
    if (host.endsWith ("vk.com") || host.endsWith ("vkvideo.ru")
        || host.endsWith ("vk.ru") || host.endsWith ("vkontakte.ru"))
        return (path.contains ("/audio") || path.contains ("/music"))
                   ? Service::vkMusic : Service::vk;
    return Service::unknown;
}

// Сервисы, где звук напрямую не отдаётся никому: работаем через название трека.
inline bool needsResolve (Service s)
{
    return s == Service::spotify || s == Service::appleMusic
        || s == Service::yandexMusic || s == Service::vkMusic;
}

// Сервисы, где без входа в аккаунт видны только открытые материалы.
// По заданию качаем именно их: сначала без входа, затем невидимо с входом
// из браузеров, и только то, что доступно без приватности.
inline bool cookieSensitive (Service s)
{
    return s == Service::instagram || s == Service::tiktok
        || s == Service::vk || s == Service::youtube;
}

inline juce::String title (Service s)
{
    switch (s)
    {
        case Service::youtube:      return "YouTube";
        case Service::youtubeMusic: return "YouTube Music";
        case Service::instagram:    return "Instagram";
        case Service::tiktok:       return "TikTok";
        case Service::pinterest:    return "Pinterest";
        case Service::vk:           return Str::utf8 ("ВКонтакте");
        case Service::spotify:      return "Spotify";
        case Service::appleMusic:   return "Apple Music";
        case Service::yandexMusic:  return Str::utf8 ("Яндекс Музыка");
        case Service::vkMusic:      return Str::utf8 ("ВК Музыка");
        case Service::soundcloud:   return "SoundCloud";
        case Service::unknown:      return Str::utf8 ("Сайт");
    }
    return {};
}

// У ссылки есть плейлист вообще (включая ролик внутри плейлиста).
inline bool hasPlaylist (juce::String raw)
{
    auto low = raw.toLowerCase();
    return low.contains ("list=") || low.contains ("/playlist")
        || low.contains ("/album/") || low.contains ("/sets/");
}

// Ссылка ведёт на подборку целиком. Ролик, открытый внутри плейлиста
// (watch?v=…&list=…), подборкой НЕ считается — качаем именно его.
inline bool isCollection (juce::String raw)
{
    auto low = raw.toLowerCase();
    if (low.contains ("watch?v=") || low.contains ("youtu.be/")) return false;
    return low.contains ("/playlist") || low.contains ("list=")
        || low.contains ("/album/") || low.contains ("/sets/");
}

// Похоже ли на ссылку. Мусор и названия треков батчем не качаем.
inline bool looksLikeLink (juce::String raw)
{
    raw = raw.trim();
    return raw.contains (".") && raw.containsChar ('/');
}
} // namespace Detector
