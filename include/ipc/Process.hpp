// ipc/Process.hpp
//
// Spawn the *same* executable again as the peer process (portable: re-launch
// with role arguments rather than fork(), which Windows lacks), and wait for it.

#pragma once

#include <string>
#include <vector>

#include "ipc/Common.hpp"

#if !defined(_WIN32)
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace ipc {

#if defined(_WIN32)
using ProcHandle = PROCESS_INFORMATION;
#else
using ProcHandle = pid_t;
#endif

inline ProcHandle spawn_self(const std::vector<std::string>& args) {
    const std::string exe = exe_path();
#if defined(_WIN32)
    std::string cmd = "\"" + exe + "\"";
    for (const auto& a : args) cmd += " \"" + a + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi))
        fail("CreateProcess");
    return pi;
#else
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
        fail("posix_spawn");
    return pid;
#endif
}

struct WaitResult {
    int code = 0;
    double child_cpu_ms = 0; // CPU the peer process burned (for the overhead metric)
};

inline WaitResult wait_for(ProcHandle h) {
#if defined(_WIN32)
    WaitForSingleObject(h.hProcess, INFINITE);
    FILETIME c, e, k, u;
    GetProcessTimes(h.hProcess, &c, &e, &k, &u); // read before closing the handle
    DWORD code = 0;
    GetExitCodeProcess(h.hProcess, &code);
    CloseHandle(h.hProcess);
    CloseHandle(h.hThread);
    return {static_cast<int>(code), filetime_ms(k) + filetime_ms(u)};
#else
    int status = 0;
    waitpid(h, &status, 0);
    rusage ru{};
    ::getrusage(RUSAGE_CHILDREN, &ru); // reaped child's CPU
    auto ms = [](timeval v) { return v.tv_sec * 1000.0 + v.tv_usec / 1000.0; };
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, ms(ru.ru_utime) + ms(ru.ru_stime)};
#endif
}

} // namespace ipc
