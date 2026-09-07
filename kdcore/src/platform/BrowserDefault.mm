#include "BrowserDefault.h"

#include <map>

#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>

namespace CookieChain
{

kd::Str defaultBrowser()
{
    // Кого система зовёт открывать https — того и считаем браузером входа.
    @autoreleasepool
    {
        NSURL* url = [NSURL URLWithString:@"https://kload.example/"];
        CFURLRef app = LSCopyDefaultApplicationURLForURL ((__bridge CFURLRef) url,
                                                         kLSRolesAll, nullptr);
        if (app == nullptr) return {};
        NSURL* appURL = CFBridgingRelease (app);
        NSBundle* bundle = [NSBundle bundleWithURL:appURL];
        const kd::Str id = bundle.bundleIdentifier != nil
            ? std::string (bundle.bundleIdentifier.UTF8String) : std::string();

        if (id == "com.apple.Safari")                return "safari";
        if (id == "com.google.Chrome")               return "chrome";
        if (id == "com.microsoft.edgemac")           return "edge";
        if (id == "org.mozilla.firefox")             return "firefox";
        if (id == "com.brave.Browser")               return "brave";
        if (id == "com.operasoftware.Opera")         return "opera";
        if (id == "com.vivaldi.Vivaldi")             return "vivaldi";
        if (id == "com.github.thebrowser.browser"
            || id == "com.thebrowser.Browser")       return "arc";
        // Хром-совместимые сборки: их cookies читает вариант "chrome".
        if (id.find ("chrom") != std::string::npos)          return "chrome";
        return {};
    }
}

bool isInstalled (const kd::Str& ytdlpName)
{
    static const std::map<kd::Str, kd::Str> bundles {
        { "safari",  "com.apple.Safari" },
        { "chrome",  "com.google.Chrome" },
        { "edge",    "com.microsoft.edgemac" },
        { "firefox", "org.mozilla.firefox" },
        { "brave",   "com.brave.Browser" },
        { "opera",   "com.operasoftware.Opera" },
        { "vivaldi", "com.vivaldi.Vivaldi" },
        { "arc",     "com.thebrowser.Browser" },
    };
    if (ytdlpName == "safari") return true; // в macOS есть всегда
    const auto it = bundles.find (ytdlpName);
    if (it == bundles.end()) return false;
    // bundleWithIdentifier видит только загруженные бандлы, поэтому живой
    // поиск идёт через LaunchServices.
    NSURL* app = [[NSWorkspace sharedWorkspace]
                      URLForApplicationWithBundleIdentifier:
                          [NSString stringWithUTF8String: it->second.c_str()]];
    return app != nil;
}

StrVec build()
{
    kd::StrVec chain;
    auto push = [&chain] (const kd::Str& b)
    {
        if (! b.empty() && ! kd::containsVec (chain, b)) chain.push_back (b);
    };

    push (defaultBrowser());
    for (const auto* b : { "safari", "chrome", "edge", "brave", "firefox",
                           "opera", "vivaldi", "arc" })
        if (isInstalled (b))
            push (b);
    return chain;
}

} // namespace CookieChain
