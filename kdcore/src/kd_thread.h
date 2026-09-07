#pragma once

// Потоковые примитивы вместо juce::Thread / juce::WaitableEvent.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace kd
{

// Одноразовый семафор: signal будит ждущего, wait сбрасывает флаг.
class Wake
{
public:
    void signal()
    {
        {
            std::lock_guard<std::mutex> g (mutex);
            flag = true;
        }
        cv.notify_all();
    }

    void waitForever()
    {
        std::unique_lock<std::mutex> lk (mutex);
        cv.wait (lk, [this] { return flag; });
        flag = false;
    }

    void waitMs (int ms)
    {
        std::unique_lock<std::mutex> lk (mutex);
        cv.wait_for (lk, std::chrono::milliseconds (ms), [this] { return flag; });
        flag = false;
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    bool flag = false;
};

} // namespace kd

using kd::Wake;
