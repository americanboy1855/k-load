#pragma once

// Замена juce::ChildProcess: запуск процесса, чтение объединённых stdout и
// stderr, код возврата, принудительное завершение. POSIX (macOS) и Win32
// (Windows) с одним интерфейсом.

#include "kd_compat.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace kd
{

class ChildProcess
{
public:
    ChildProcess() = default;

    ~ChildProcess()
    {
        if (started)
        {
            kill();
            waitExitCode();
        }
        closeOutput();
    }

    ChildProcess (const ChildProcess&) = delete;
    ChildProcess& operator= (const ChildProcess&) = delete;

    // Запуск с объединёнными stdout и stderr — ядро разбирает оба потока
    // как один (ошибки yt-dlp приходят в stderr). pathEnv подменяет PATH
    // в окружении ребёнка (инструменты комплекта + системные пути).
    bool start (const StrVec& args, const Str& pathEnv = {})
    {
        if (args.empty()) return false;

        SECURITY_ATTRIBUTES inherit { sizeof (SECURITY_ATTRIBUTES), nullptr, TRUE };
        if (! ::CreatePipe (&outRead, &outWrite, &inherit, 0)) return false;
        // Читающий конец должен быть только нашим — иначе ребёнок унаследует
        // его и пайп не закроется после его смерти (read() не увидит конец).
        ::SetHandleInformation (outRead, HANDLE_FLAG_INHERIT, 0);

        const auto cmd = buildCommandLine (args);
        std::wstring mutableCmd = cmd;
        const auto envBlock = pathEnv.empty() ? std::wstring()
                                              : buildEnvBlock (toWide (pathEnv));

        STARTUPINFOW si {};
        si.cb = sizeof (si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = outWrite;
        si.hStdError = outWrite;

        DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
        const BOOL ok = ::CreateProcessW (toWide (args[0]).c_str(), mutableCmd.data(),
                                          nullptr, nullptr, TRUE, flags,
                                          envBlock.empty() ? nullptr : envBlock.data(),
                                          nullptr, &si, &pi);
        ::CloseHandle (outWrite);
        outWrite = nullptr;

        if (! ok)
        {
            ::CloseHandle (outRead);
            outRead = nullptr;
            return false;
        }
        started = true;
        return true;
    }

    // Блокирующее чтение с таймаутом. >0 — байты; 0 — поток закрыт
    // (процесс закончил вывод); -1 — таймаут, процесса можно подождать ещё.
    int read (char* buffer, int bytes, int timeoutMs = 40)
    {
        if (! started || outRead == nullptr) return 0;

        const ULONGLONG deadline = ::GetTickCount64() + (ULONGLONG) timeoutMs;
        for (;;)
        {
            DWORD avail = 0;
            if (! ::PeekNamedPipe (outRead, nullptr, 0, nullptr, &avail, nullptr))
                return 0; // пайп закрыт — процесс закончил вывод
            if (avail > 0)
            {
                DWORD got = 0;
                if (! ::ReadFile (outRead, buffer, (DWORD) bytes, &got, nullptr))
                    return 0;
                return got > 0 ? (int) got : 0;
            }
            if (::GetTickCount64() >= deadline) return -1;
            ::Sleep (2);
        }
    }

    bool isRunning()
    {
        if (! started || reaped) return false;
        const DWORD w = ::WaitForSingleObject (pi.hProcess, 0);
        if (w == WAIT_TIMEOUT) return true;
        ::GetExitCodeProcess (pi.hProcess, &exitCode);
        reaped = true;
        return false;
    }

    int waitExitCode()
    {
        if (! started) return -1;
        if (! reaped)
        {
            ::WaitForSingleObject (pi.hProcess, INFINITE);
            ::GetExitCodeProcess (pi.hProcess, &exitCode);
            reaped = true;
        }
        return (int) exitCode;
    }

    void kill()
    {
        // На Windows нет SIGTERM: жёсткое завершение. .part-файл yt-dlp
        // остаётся валидным (ОС закрывает дескрипторы) — после паузы
        // загрузка продолжается с сохранённой позиции, как на macOS.
        if (started && ! reaped)
        {
            ::TerminateProcess (pi.hProcess, (UINT) -1);
            ::WaitForSingleObject (pi.hProcess, 500);
        }
    }

    void closeOutput()
    {
        if (outRead != nullptr) { ::CloseHandle (outRead); outRead = nullptr; }
        if (outWrite != nullptr) { ::CloseHandle (outWrite); outWrite = nullptr; }
        if (started)
        {
            ::CloseHandle (pi.hThread);
            ::CloseHandle (pi.hProcess);
            started = false;
        }
    }

private:
    static std::wstring toWide (const Str& s)
    {
        if (s.empty()) return {};
        const int n = ::MultiByteToWideChar (CP_UTF8, 0, s.c_str(), (int) s.size(),
                                             nullptr, 0);
        std::wstring w ((size_t) n, L'\0');
        ::MultiByteToWideChar (CP_UTF8, 0, s.c_str(), (int) s.size(), w.data(), n);
        return w;
    }

    // Кавычки и обратные слеши — по правилам командной строки Windows.
    static std::wstring quoteArg (const std::wstring& a)
    {
        std::wstring out = L"\"";
        size_t backslashes = 0;
        for (const wchar_t c : a)
        {
            if (c == L'\\') { ++backslashes; continue; }
            if (c == L'"')
            {
                out.append (backslashes * 2 + 2, L'\\');
                out += L'"';
            }
            else
            {
                out.append (backslashes, L'\\');
                out += c;
            }
            backslashes = 0;
        }
        out.append (backslashes * 2, L'\\');
        out += L'"';
        return out;
    }

    static std::wstring buildCommandLine (const StrVec& args)
    {
        std::wstring out;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (i > 0) out += L' ';
            out += quoteArg (toWide (args[i]));
        }
        return out;
    }

    // Копия текущего окружения с подменённым PATH (регистр имени не важен).
    static std::wstring buildEnvBlock (const std::wstring& pathValue)
    {
        std::wstring out;
        bool pathEmitted = false;
        if (LPWCH block = ::GetEnvironmentStringsW())
        {
            for (LPWCH p = block; *p;)
            {
                const std::wstring orig (p);
                const size_t len = orig.size();
                const bool isPath = len >= 5 && _wcsnicmp (orig.c_str(), L"PATH=", 5) == 0;
                out += isPath ? (L"PATH=" + pathValue) : orig;
                out += L'\0';
                pathEmitted |= isPath;
                p += len + 1;
            }
            ::FreeEnvironmentStringsW (block);
        }
        if (! pathEmitted) { out += L"PATH=" + pathValue; out += L'\0'; }
        out += L'\0';
        return out;
    }

    PROCESS_INFORMATION pi {};
    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    DWORD exitCode = (DWORD) -1;
    bool started = false;
    bool reaped = false;
};

} // namespace kd

using kd::ChildProcess;

#else // POSIX — macOS

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
        closeOutput();
    }

    ChildProcess (const ChildProcess&) = delete;
    ChildProcess& operator= (const ChildProcess&) = delete;

    // Запуск с объединёнными stdout и stderr — ядро разбирает оба потока
    // как один (ошибки yt-dlp приходят в stderr).
    bool start (const StrVec& args, const Str& pathEnv = {})
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
        {
            // SIGTERM: yt-dlp успевает корректно закрыть .part-файл —
            // после паузы загрузка продолжится с сохранённой позиции.
            ::kill (pid, SIGTERM);
        }
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

#endif // _WIN32
