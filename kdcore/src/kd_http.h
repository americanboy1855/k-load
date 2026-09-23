#pragma once

// Замена juce::URL-запросов: GET до строки и до файла через системный
// libcurl. Ядро тянет им открытые данные сервисов (Spotify embed, iTunes
// lookup, Pinterest oEmbed, страница поиска YouTube) и фотографии пинов.

#include "kd_compat.h"

#include <map>
#include <fstream>
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
// часть сервисов сразу отдаёт заглушку. Плюс системный прокси WinINET:
// libcurl сам реестр не читает, а без прокси часть сервисов из РФ недоступна.
static Str systemProxy ()
{
    static Str cached;
    static bool read = false;
    if (! read)
    {
        read = true;
#ifdef _WIN32
        // WinINET-реестр есть только на Windows; на macOS libcurl берёт
        // прокси из окружения сам, дополнительно читать нечего.
        HKEY key = nullptr;
        if (::RegOpenKeyExW (HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                0, KEY_READ, &key) == ERROR_SUCCESS)
        {
            DWORD enabled = 0, size = sizeof (enabled);
            if (::RegQueryValueExW (key, L"ProxyEnable", nullptr, nullptr,
                    reinterpret_cast<LPBYTE> (&enabled), &size) == ERROR_SUCCESS
                && enabled != 0)
            {
                wchar_t buf[512] = {};
                size = sizeof (buf) - sizeof (wchar_t);
                if (::RegQueryValueExW (key, L"ProxyServer", nullptr, nullptr,
                        reinterpret_cast<LPBYTE> (buf), &size) == ERROR_SUCCESS
                    && buf[0] != L'\0')
                {
                    // Форматы: "host:port" или "http=…;https=…" — берём http.
                    std::wstring w (buf);
                    std::wstring pick = w;
                    if (w.find (L'=') != std::wstring::npos)
                    {
                        pick.clear ();
                        size_t start = 0;
                        while (start <= w.size ())
                        {
                            const size_t semi = w.find (L';', start);
                            const auto part = w.substr (start,
                                semi == std::wstring::npos ? std::wstring::npos
                                                           : semi - start);
                            if (part.rfind (L"http=", 0) == 0)
                            {
                                pick = part.substr (5);
                                break;
                            }
                            if (semi == std::wstring::npos) break;
                            start = semi + 1;
                        }
                        if (pick.empty ()) pick = w.substr (0, w.find (L';'));
                    }
                    if (! pick.empty () && pick.find (L"://") == std::wstring::npos)
                        pick = L"http://" + pick;
                    const int n = ::WideCharToMultiByte (CP_UTF8, 0, pick.c_str (),
                        (int) pick.size (), nullptr, 0, nullptr, nullptr);
                    std::string u ((size_t) n, '\0');
                    ::WideCharToMultiByte (CP_UTF8, 0, pick.c_str (),
                        (int) pick.size (), u.data (), n, nullptr, nullptr);
                    cached = u;
                }
            }
            ::RegCloseKey (key);
        }
#endif // _WIN32
    }
    return cached;
}

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
    const Str proxy = systemProxy ();
    if (! proxy.empty ())
        curl_easy_setopt (c, CURLOPT_PROXY, proxy.c_str ());
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
    if (f == nullptr)
    {
        std::ofstream dbg ("C:\\Temp\\kd-curl.log", std::ios::app);
        if (dbg.is_open())
            dbg << "fopen FAILED target=" << kd::pathStr (target) << "\n";
        curl_easy_cleanup (c);
        return false;
    }
    Sink sink { nullptr, f }; // text обязан быть нулевым: мусорный указатель — UB
    curl_easy_setopt (c, CURLOPT_URL, url.c_str());
    curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt (c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt (c, CURLOPT_FAILONERROR, 1L);
    const CURLcode rc = curl_easy_perform (c);
    if (rc != CURLE_OK) // диагностика: без этого «нет превью» не расследовать
    {
        std::ofstream dbg ("C:\\Temp\\kd-curl.log", std::ios::app);
        if (dbg.is_open())
            dbg << "downloadToFile rc=" << rc << " (" << curl_easy_strerror (rc)
                << ") url=" << url << " target=" << kd::pathStr (target) << "\n";
    }
    const bool ok = rc == CURLE_OK;
    curl_easy_cleanup (c);
    fclose (f);
    if (! ok) fs::remove (target);
    return ok;
}

} // namespace http

} // namespace kd
