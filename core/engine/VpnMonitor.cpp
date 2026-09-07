#include "VpnMonitor.h"

// VPN виден по таблице маршрутизации: туннельные интерфейсы (utun, ppp,
// ipsec, wg) получают маршрут по умолчанию. Штатные utun-интерфейсы macOS
// (iCloud и пр.) маршрут по умолчанию не забирают — их считаем фоном.
bool VpnMonitor::checkOnce()
{
   #if JUCE_WINDOWS
    // Windows-адаптация появится на своём этапе.
    return true;
   #else
    juce::ChildProcess route;
    if (! route.start ("route -n get default", juce::ChildProcess::wantStdOut))
        return false;

    juce::MemoryBlock mb;
    char chunk[4096];
    for (;;)
    {
        const int n = route.readProcessOutput (chunk, (int) sizeof (chunk));
        if (n > 0) mb.append (chunk, (size_t) n);
        else if (! route.isRunning()) break;
        else juce::Thread::sleep (10);
    }

    const auto out = juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize());
    if (out.contains ("not in table")) return false;

    for (const auto& line : juce::StringArray::fromLines (out))
    {
        const auto t = line.trim();
        if (t.startsWith ("interface:"))
        {
            const auto iface = t.fromFirstOccurrenceOf ("interface:", false, false).trim();
            return iface.startsWith ("utun") || iface.startsWith ("tun")
                || iface.startsWith ("tap") || iface.startsWith ("ppp")
                || iface.startsWith ("ipsec") || iface.startsWith ("wg");
        }
    }
    return false;
   #endif
}

void VpnMonitor::run()
{
    while (! quitFlag.load (std::memory_order_relaxed) && ! threadShouldExit())
    {
        const bool on = checkOnce();
        const auto next = on ? State::on : State::off;
        const auto prev = currentState.exchange (next, std::memory_order_relaxed);
        if (prev != next) sendChangeMessage();

        wake.wait (15000);
    }
}
