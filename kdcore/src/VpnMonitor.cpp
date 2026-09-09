#include "VpnMonitor.h"
#include "kd_process.h"

// VPN виден по таблице маршрутизации: туннельные интерфейсы (utun, ppp,
// ipsec, wg) получают маршрут по умолчанию. Штатные utun-интерфейсы macOS
// (iCloud и пр.) маршрут по умолчанию не забирают — их считаем фоном.

VpnMonitor::VpnMonitor()
{
    thread = std::thread ([this]
    {
        pthread_setname_np ("kd-vpn");
        run();
    });
}

VpnMonitor::~VpnMonitor()
{
    signal (true);
    if (thread.joinable())
        thread.join();
}

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

        wake.waitMs (8000);
    }
}
