#pragma once

// Замена juce::URL-запросов: GET до строки и до файла через системный
// libcurl. Ядро тянет им открытые данные сервисов (Spotify embed, iTunes
// lookup, Pinterest oEmbed, страница поиска YouTube) и фотографии пинов.

#include "kd_compat.h"

#include <map>
#include <curl/curl.h>

namespace kd
{

namespace http
{

struct Sink
{
    std::string* text = nullptr;
    FILE* file = nullptr;
};

static size_t writeCb (char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* sink = (Sink*) userdata;
    const size_t bytes = size * nmemb;
    if (sink->text != nullptr)
        sink->text->append (ptr, bytes);
    else if (sink->file != nullptr)
        return fwrite (ptr, 1, bytes, sink->file);
    return bytes;
}

// Общие настройки запроса: пользовательский агент как у браузера — без него
// часть сервисов сразу отдаёт заглушку.
static CURL* makeEasy (int timeoutMs)
{
    CURL* c = curl_easy_init();
    if (c == nullptr) return nullptr;
    curl_easy_setopt (c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt (c, CURLOPT_TIMEOUT_MS, (long) timeoutMs);
    curl_easy_setopt (c, CURLOPT_CONNECTTIMEOUT_MS, (long) timeoutMs);
    curl_easy_setopt (c, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36");
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append (headers, "Accept-Language: ru,en;q=0.8");
    curl_easy_setopt (c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt (c, CURLOPT_SSL_VERIFYPEER, 1L);
    return c;
}

// То же с дополнительными заголовками (Bearer для публичных API).
inline Str fetch (const Str& url, int timeoutMs,
                  const std::map<Str, Str>& extraHeaders)
{
    CURL* c = makeEasy (timeoutMs);
    if (c == nullptr) return {};
    Sink sink;
    Str out;
    sink.text = &out;
    curl_easy_setopt (c, CURLOPT_URL, url.c_str());
    curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt (c, CURLOPT_WRITEDATA, &sink);
    struct curl_slist* headers = nullptr;
    for (const auto& h : extraHeaders)
        headers = curl_slist_append (headers, (h.first + ": " + h.second).c_str());
    if (headers != nullptr)
        curl_easy_setopt (c, CURLOPT_HTTPHEADER, headers);
    curl_easy_perform (c);
    curl_easy_cleanup (c);
    if (headers != nullptr) curl_slist_free_all (headers);
    return out;
}

// GET страницы целиком. Пусто — сеть, сервер или таймаут.
inline Str fetch (const Str& url, int timeoutMs = 15000)
{
    return fetch (url, timeoutMs, {});
}

// GET в файл (фотография Pinterest). false — не скачалось.
inline bool downloadToFile (const Str& url, const fs::path& target, int timeoutMs = 15000)
{
    CURL* c = makeEasy (timeoutMs);
    if (c == nullptr) return false;
#ifdef _WIN32
    FILE* f = _wfopen (target.c_str(), L"wb");
#else
    FILE* f = fopen (target.string().c_str(), "wb");
#endif
    if (f == nullptr) { curl_easy_cleanup (c); return false; }
    Sink sink;
    sink.file = f;
    curl_easy_setopt (c, CURLOPT_URL, url.c_str());
    curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt (c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt (c, CURLOPT_FAILONERROR, 1L);
    const bool ok = curl_easy_perform (c) == CURLE_OK;
    curl_easy_cleanup (c);
    fclose (f);
    if (! ok) fs::remove (target);
    return ok;
}

} // namespace http

} // namespace kd
