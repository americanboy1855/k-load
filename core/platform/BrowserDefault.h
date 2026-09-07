#pragma once

#include <juce_core/juce_core.h>

// Браузеры для невидимого входа. Пользователь источник не видит: интерфейс
// не показывает, чьи cookies взяты, — им важно только «получилось / нет».
namespace CookieChain
{

// Браузер по умолчанию в системе (имя для yt-dlp: "safari", "chrome"…).
// Пусто — определить не удалось.
juce::String defaultBrowser();

// Порядок подбора: сначала браузер по умолчанию, затем остальные
// установленные. Дубликаты убираются; имена — как их понимает yt-dlp.
juce::StringArray build();

// Установлен ли браузер с таким именем (по бандлу приложения).
bool isInstalled (const juce::String& ytdlpName);

} // namespace CookieChain
