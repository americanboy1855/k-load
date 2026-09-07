#include "BrowserDefault.h"

#include <map>

#if JUCE_MAC
  #import <Foundation/Foundation.h>
  #import <AppKit/AppKit.h>
  #import <CoreServices/CoreServices.h>
#endif

namespace CookieChain
{

juce::String defaultBrowser()
{
   #if JUCE_MAC
    // Кого система зовёт открывать https — того и считаем браузером входа.
    @autoreleasepool
    {
        NSURL* url = [NSURL URLWithString:@"https://kdownloader.example/"];
        CFURLRef app = LSCopyDefaultApplicationURLForURL ((__bridge CFURLRef) url,
                                                         kLSRolesAll, nullptr);
        if (app == nullptr) return {};
        NSURL* appURL = CFBridgingRelease (app);
        NSBundle* bundle = [NSBundle bundleWithURL:appURL];
        const juce::String id = juce::String::fromUTF8 (bundle.bundleIdentifier.UTF8String);

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
        if (id.contains ("chrom"))                   return "chrome";
        return {};
    }
   #else
    return {}; // Windows: определение появится на этапе Windows-версии
   #endif
}

bool isInstalled (const juce::String& ytdlpName)
{
   #if JUCE_MAC
    static const std::map<juce::String, juce::String> bundles {
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
                          [NSString stringWithUTF8String: it->second.toRawUTF8()]];
    return app != nil;
   #else
    return false;
   #endif
}

juce::StringArray build()
{
    juce::StringArray chain;
    auto push = [&chain] (const juce::String& b)
    {
        if (b.isNotEmpty() && ! chain.contains (b)) chain.add (b);
    };

    push (defaultBrowser());
    for (const auto* b : { "safari", "chrome", "edge", "brave", "firefox",
                           "opera", "vivaldi", "arc" })
        if (isInstalled (b))
            push (b);
    return chain;
}

} // namespace CookieChain
