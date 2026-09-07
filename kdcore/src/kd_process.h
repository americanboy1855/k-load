#pragma once

// Замена juce::ChildProcess: запуск процесса, чтение объединённых stdout и
// stderr, код возврата, принудительное завершение. POSIX, macOS.

#include "kd_compat.h"

#include <csignal>
#include <cstdlib>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace kd
{

class ChildProcess
{
public:
    ChildProcess() = default;

    ~ChildProcess()
    {
        if (pid > 0)
        {
            kill();
            waitExitCode();
        }
    }

    ChildProcess (const ChildProcess&) = delete;
    ChildProcess& operator= (const ChildProcess&) = delete;

    // Запуск с объединёнными stdout и stderr — ядро разбирает оба потока
    // как один (ошибки yt-dlp приходят в stderr).
    bool start (const StrVec& args)
    {
        if (args.empty()) return false;

        int fds[2];
        if (::pipe (fds) != 0) return false;

        std::vector<char*> argv;
        argv.reserve (args.size() + 1);
        for (const auto& a : args)
            argv.push_back (const_cast<char*> (a.c_str()));
        argv.push_back (nullptr);

        const pid_t newPid = ::fork();
        if (newPid < 0)
        {
            ::close (fds[0]);
            ::close (fds[1]);
            return false;
        }
        if (newPid == 0)
        {
            ::dup2 (fds[1], STDOUT_FILENO);
            ::dup2 (fds[1], STDERR_FILENO);
            ::close (fds[0]);
            ::close (fds[1]);
            ::signal (SIGPIPE, SIG_DFL);
            ::execvp (argv[0], argv.data());
            _Exit (127);
        }

        ::close (fds[1]);
        pid = newPid;
        outFd = fds[0];
        reaped = false;
        return true;
    }

    // Блокирующее чтение с таймаутом. >0 — байты; 0 — поток закрыт
    // (процесс закончил вывод); -1 — таймаут, процесса можно подождать ещё.
    int read (char* buffer, int bytes, int timeoutMs = 40)
    {
        if (outFd < 0) return 0;

        pollfd p { outFd, POLLIN, 0 };
        const int ready = ::poll (&p, 1, timeoutMs);
        if (ready <= 0) return -1;
        if (p.revents & (POLLHUP | POLLERR))
        {
            // Перед закрытием могут доехать последние байты.
            const ssize_t n = ::read (outFd, buffer, (size_t) bytes);
            return n > 0 ? (int) n : 0;
        }
        const ssize_t n = ::read (outFd, buffer, (size_t) bytes);
        if (n > 0) return (int) n;
        return 0;
    }

    bool isRunning()
    {
        if (pid <= 0) return false;
        if (reaped) return false;
        const int r = ::waitpid (pid, &status, WNOHANG);
        if (r == pid) { reaped = true; return false; }
        return true;
    }

    int waitExitCode()
    {
        if (pid <= 0) return -1;
        if (! reaped)
        {
            int st = 0;
            ::waitpid (pid, &st, 0);
            status = st;
            reaped = true;
        }
        return WIFEXITED (status) ? WEXITSTATUS (status) : -1;
    }

    void kill()
    {
        if (pid > 0 && ! reaped)
            ::kill (pid, SIGKILL);
    }

    void closeOutput()
    {
        if (outFd >= 0) { ::close (outFd); outFd = -1; }
    }

private:
    pid_t pid = -1;
    int outFd = -1;
    int status = 0;
    bool reaped = false;
};

} // namespace kd

using kd::ChildProcess;
