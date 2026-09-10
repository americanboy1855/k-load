#pragma once

// Потоковые примитивы вместо juce::Thread / juce::WaitableEvent.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace kd
{

// Считающий семафор: каждый signal выдаёт один токен, каждый wait один
// потребляет. С двумя воркерами очереди одноразовый флаг терял пробуждения
// (notify_all будил обоих, а флаг съедал первый — второй засыпал навсегда).
class Wake
{
public:
    void signal()
    {
        {
            std::lock_guard<std::mutex> g (mutex);
            ++count;
        }
        cv.notify_all();
    }

    void waitForever()
    {
        std::unique_lock<std::mutex> lk (mutex);
        cv.wait (lk, [this] { return count > 0; });
        --count;
    }

    void waitMs (int ms)
    {
        std::unique_lock<std::mutex> lk (mutex);
        cv.wait_for (lk, std::chrono::milliseconds (ms), [this] { return count > 0; });
        if (count > 0) --count;
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    int count = 0;
};

} // namespace kd

using kd::Wake;
