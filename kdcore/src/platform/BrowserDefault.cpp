#include "BrowserDefault.h"

// Windows-вариант. Подбор cookies из браузеров на Windows не работает:
// Chrome/Edge шифруют cookie-базу DPAPI/App-Bound Encryption. На macOS
// эта цепочка фактически тоже не используется (ядро делает анонимную
// попытку) — здесь честно возвращаем пустоту, поведение совпадает.

namespace CookieChain
{

kd::Str defaultBrowser() { return {}; }

kd::StrVec build() { return {}; }

bool isInstalled (const kd::Str&) { return false; }

} // namespace CookieChain
