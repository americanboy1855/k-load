#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include "kd_compat.h"
#include "kd_thread.h"

// Наблюдение за VPN: активен, если маршрут по умолчанию идёт через туннель
// (utun/ppp/ipsec/wg). Работает в фоновом потоке; о смене состояния
// сообщается через onChange. Плашка в интерфейсе — подсказка, не блокировка.
class VpnMonitor
{
public:
    enum class State { unknown, on, off };

    VpnMonitor();
    ~VpnMonitor();

    // Поток наблюдения стартует в конструкторе, поэтому колбэк
    // подменяется под мьютексом — без гонки на первом срабатывании.
    // Состояние передаётся аргументом: читать члены из колбэка нельзя —
    // к моменту join в деструкторе владелец мог их уже обнулить.
    void setOnChange (std::function<void (State)> cb)
    {
        const std::lock_guard<std::mutex> g (callbackMutex);
        onChange = std::move (cb);
    }

    std::function<void (State)> copyOnChange()
    {
        const std::lock_guard<std::mutex> g (callbackMutex);
        return onChange;
    }

    State state() const { return currentState.load (std::memory_order_relaxed); }

    // Единичная проверка (вызывается потоком наблюдения). lastReason —
    // почему включён: «туннель utun5», «системный прокси», «vpn-сервис».
    bool checkOnce();

private:
    void signal (bool quitNow)
    {
        quitFlag.store (quitNow, std::memory_order_relaxed);
        wake.signal();
    }

    void run();

    std::atomic<State> currentState { State::unknown };
    Str lastReason; // пишется в checkOnce, читается в run() (один поток)
    std::atomic<bool> quitFlag { false };
    std::mutex callbackMutex;
    std::function<void (State)> onChange;
    kd::Wake wake;
    std::thread thread;
};
