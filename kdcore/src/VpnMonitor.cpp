#include "VpnMonitor.h"
#include "kd_process.h"
#include "Engine.h"

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>
#endif

// VPN виден по таблице маршрутизации: туннельные интерфейсы (utun, ppp,
// ipsec, wg) получают маршрут по умолчанию. Штатные utun-интерфейсы macOS
// (iCloud и пр.) маршрут по умолчанию не забирают — их считаем фоном.
//
// Windows: маршрут по умолчанию → GetBestRoute, интерфейс маршрута →
// GetAdaptersAddresses. VPN — туннельный/PPP тип или туннельное имя
// (tun/tap/wg/vpn/ppp/ipsec).

VpnMonitor::VpnMonitor()
{
    thread = std::thread ([this]
    {
#if defined(__APPLE__)
        pthread_setname_np ("kd-vpn");
#endif
        run();
    });
}

VpnMonitor::~VpnMonitor()
{
    signal (true);
    if (thread.joinable())
        thread.join();
}

#ifdef _WIN32
bool VpnMonitor::checkOnce()
{
    // Метод 1 — туннельный интерфейс в маршруте по умолчанию (tun/tap/wg/
    // ipsec/PPP и клиенты с TUN-режимом: Happ, Amnezia, Clash, sing-box…).
    // Ошибка проверки ≠ «выключен»: при сбое методы ниже всё равно смотрим,
    // а итоговое состояние сглаживается debounce'ом в run().
    bool routeChecked = false;
    MIB_IPFORWARDROW row {};
    if (::GetBestRoute (0, 0, &row) == NO_ERROR)
    {
        routeChecked = true;
        const ULONG ifIdx = row.dwForwardIfIndex;

        std::vector<char> buf (16 * 1024);
        for (;;)
        {
            auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*> (buf.data());
            ULONG size = (ULONG) buf.size();
            const DWORD r = ::GetAdaptersAddresses (AF_UNSPEC, 0, nullptr, aa, &size);
            if (r == ERROR_BUFFER_OVERFLOW) { buf.resize (size); continue; }
            if (r != NO_ERROR) break;

            for (auto* a = aa; a != nullptr; a = a->Next)
            {
                if (a->IfIndex != ifIdx) continue;
                if (a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP)
                {
                    lastReason = "адаптер (тип туннель)";
                    return true;
                }

                Str name;
                if (a->FriendlyName != nullptr)
                {
                    const int n = ::WideCharToMultiByte (CP_UTF8, 0, a->FriendlyName, -1,
                                                         nullptr, 0, nullptr, nullptr);
                    std::vector<char> raw ((size_t) n, '\0');
                    ::WideCharToMultiByte (CP_UTF8, 0, a->FriendlyName, -1,
                                           raw.data(), n, nullptr, nullptr);
                    name = kd::lower (Str (raw.data()));
                }
                // Имена адаптеров VPN-клиентов: "tun" покрывает tun2socks,
                // "sing" — sing-box, "awg" — Amnezia WireGuard (W-1).
                static const char* kVpnNames[] = {
                    "tun", "tap", "wg", "vpn", "ppp", "ipsec",
                    "happ", "amnezia", "awg", "clash", "mihomo",
                    "sing", "shadowsocks", "v2ray", "xray", "hysteria",
                    "tor", "psiphon", "warp", "tailscale", "zerotier", "outline"
                };
                for (const char* marker : kVpnNames)
                {
                    if (kd::contains (name, marker))
                    {
                        lastReason = Str ("адаптер ") + marker;
                        return true;
                    }
                }
            }
            break;
        }
    }
    (void) routeChecked;

    // Метод 2 — системный прокси WinINET (ProxyEnable): Clash/V2Ray/Happ
    // в режиме «без TUN» туннель не создают, трафик идёт через локальный
    // прокси. Владелец подтвердил: системный прокси = VPN включён.
    HKEY key = nullptr;
    if (::RegOpenKeyExW (HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
            0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        DWORD enabled = 0, size = sizeof (enabled);
        const bool proxyOn =
            ::RegQueryValueExW (key, L"ProxyEnable", nullptr, nullptr,
                reinterpret_cast<LPBYTE> (&enabled), &size) == ERROR_SUCCESS
            && enabled != 0;
        ::RegCloseKey (key);
        if (proxyOn)
        {
            lastReason = "прокси";
            return true;
        }
    }

    // Метод 3 — WinHTTP-дефолт-прокси: некоторые клиенты прописывают его,
    // не трогая WinINET (W-1). Реестр DefaultConnectionSettings → ищем
    // флаг прокси-включён в бинарном блобе (байт со смещением 8 — флаги:
    // бит 2 (0x04) = прокси включён, как в WINHTTP_PROXY_INFO).
    HKEY winhttp = nullptr;
    if (::RegOpenKeyExW (HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\Connections",
            0, KEY_READ, &winhttp) == ERROR_SUCCESS)
    {
        BYTE blob[512] = {};
        DWORD blobSize = sizeof (blob);
        const bool ok =
            ::RegQueryValueExW (winhttp, L"DefaultConnectionSettings", nullptr,
                nullptr, blob, &blobSize) == ERROR_SUCCESS && blobSize >= 12;
        ::RegCloseKey (winhttp);
        if (ok && (blob[8] & 0x04) != 0)
        {
            lastReason = "прокси winhttp";
            return true;
        }
    }

    lastReason.clear();
    return false;
}
#else
// Вывод утилиты целиком (короткий запуск, вывод маленький).
static Str captureCommand (const StrVec& args)
{
    kd::ChildProcess proc;
    if (! proc.start (args)) return {};
    Str out;
    char chunk[4096];
    for (;;)
    {
        const int n = proc.read (chunk, (int) sizeof (chunk), 120);
        if (n > 0) out.append (chunk, (size_t) n);
        else if (n == 0) break;
    }
    proc.waitExitCode();
    return out;
}

bool VpnMonitor::checkOnce()
{
    // Метод 1 — туннельный интерфейс в маршруте по умолчанию (utun/tun/tap/
    // ppp/ipsec/wg; Happ и WireGuard в TUN-режиме попадают сюда же).
    const auto route = captureCommand ({ "route", "-n", "get", "default" });
    if (! kd::contains (route, "not in table"))
    {
        for (const auto& line : kd::splitLines (route))
        {
            const auto t = kd::trim (line);
            if (kd::startsWith (t, "interface:"))
            {
                const auto iface = kd::lower (kd::trim (kd::fromFirst (t, "interface:")));
                const bool tunnel = kd::startsWith (iface, "utun")
                    || kd::startsWith (iface, "tun")
                    || kd::startsWith (iface, "tap")
                    || kd::startsWith (iface, "ppp")
                    || kd::startsWith (iface, "ipsec")
                    || kd::startsWith (iface, "wg");
                if (tunnel)
                {
                    lastReason = "туннель " + iface;
                    return true;
                }
            }
        }
    }

    // Метод 2 — системный прокси: Clash/V2Ray/Happ в режиме «без TUN»
    // туннель не создают, трафик идёт через SOCKS/HTTP-прокси (владелец
    // подтвердил: системный прокси = VPN включён).
    const auto proxy = captureCommand ({ "scutil", "--proxy" });
    const bool proxyOn = kd::contains (proxy, "HTTPEnable : 1")
        || kd::contains (proxy, "HTTPSEnable : 1")
        || kd::contains (proxy, "SOCKSEnable : 1");
    if (proxyOn)
    {
        lastReason = "системный прокси";
        return true;
    }

    // Метод 3 — сетевые VPN-сервисы (встроенные IKEv2/L2TP-конфигурации):
    // scutil --nc list помечает подключённые как "(Connected)".
    const auto nc = captureCommand ({ "scutil", "--nc", "list" });
    if (kd::contains (nc, "(Connected)"))
    {
        lastReason = "vpn-сервис";
        return true;
    }

    lastReason.clear();
    return false;
}
#endif

void VpnMonitor::run()
{
    // Debounce: состояние меняется только после двух подряд одинаковых
    // проверок — кратковременный сбой сети не мигает плашкой.
    int same = 0;
    bool last = false;

    while (! quitFlag.load (std::memory_order_relaxed))
    {
        const bool on = checkOnce();
        if (same > 0 && on == last)
        {
            ++same;
        }
        else
        {
            last = on;
            same = 1;
        }

        // Две подряд одинаковые проверки — считаем состояние устоявшимся.
        if (same >= 2)
        {
            const auto next = on ? State::on : State::off;
            const auto prev = currentState.exchange (next, std::memory_order_relaxed);
            if (prev != next)
            {
                Engine::engineLog (on ? "vpn: включён (" + reason() + ")"
                                      : "vpn: выключен");
                if (auto cb = copyOnChange())
                    cb (next);
            }
        }

        // Проверка раз в секунду: две подряд одинаковые — состояние
        // устоялось. Реакция на отключение VPN — в пределах пары секунд.
        wake.waitMs (1000);
    }
}
