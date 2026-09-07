#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

// Наблюдение за VPN: активен, если маршрут по умолчанию идёт через туннель
// (utun/ppp/ipsec/wg). Работает в фоновом потоке, результат приезжает на
// message thread. Плашка в интерфейсе — подсказка, не блокировка.
class VpnMonitor : public juce::ChangeBroadcaster, private juce::Thread
{
public:
    enum class State { unknown, on, off };

    VpnMonitor() : juce::Thread ("K Downloader vpn") { startThread(); }
    ~VpnMonitor() override { signal (true); stopThread (4000); }

    State state() const { return currentState.load (std::memory_order_relaxed); }

    // Единичная проверка прямо сейчас (можно звать из любого потока).
    static bool checkOnce();

private:
    void signal (bool quit)
    {
        quitFlag.store (quit, std::memory_order_relaxed);
        wake.signal();
    }

    void run() override;

    std::atomic<State> currentState { State::unknown };
    std::atomic<bool> quitFlag { false };
    juce::WaitableEvent wake;
};
