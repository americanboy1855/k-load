#include "VpnMonitor.h"
#include "kd_process.h"

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
    MIB_IPFORWARDROW row {};
    if (::GetBestRoute (0, 0, &row) != NO_ERROR) return false;
    const ULONG ifIdx = row.dwForwardIfIndex;

    std::vector<char> buf (16 * 1024);
    for (;;)
    {
        auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*> (buf.data());
        ULONG size = (ULONG) buf.size();
        const DWORD r = ::GetAdaptersAddresses (AF_UNSPEC, 0, nullptr, aa, &size);
        if (r == ERROR_BUFFER_OVERFLOW) { buf.resize (size); continue; }
        if (r != NO_ERROR) return false;

        for (auto* a = aa; a != nullptr; a = a->Next)
        {
            if (a->IfIndex != ifIdx) continue;
            if (a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP) return true;

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
            return kd::containsAny (name, { "tun", "tap", "wg", "vpn", "ppp", "ipsec" });
        }
        return false;
    }
}
#else
bool VpnMonitor::checkOnce()
{
    kd::ChildProcess route;
    if (! route.start ({ "route", "-n", "get", "default" }))
        return false;

    Str out;
    char chunk[4096];
    for (;;)
    {
        const int n = route.read (chunk, (int) sizeof (chunk), 100);
        if (n > 0) out.append (chunk, (size_t) n);
        else if (n == 0) break;
    }
    route.waitExitCode();

    if (kd::contains (out, "not in table")) return false;

    for (const auto& line : kd::splitLines (out))
    {
        const auto t = kd::trim (line);
        if (kd::startsWith (t, "interface:"))
        {
            const auto iface = kd::trim (kd::fromFirst (t, "interface:"));
            return kd::startsWith (iface, "utun") || kd::startsWith (iface, "tun")
                || kd::startsWith (iface, "tap") || kd::startsWith (iface, "ppp")
                || kd::startsWith (iface, "ipsec") || kd::startsWith (iface, "wg");
        }
    }
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
                if (auto cb = copyOnChange())
                    cb (next);
        }

        // Проверка раз в секунду: две подряд одинаковые — состояние
        // устоялось. Реакция на отключение VPN — в пределах пары секунд.
        wake.waitMs (1000);
    }
}
