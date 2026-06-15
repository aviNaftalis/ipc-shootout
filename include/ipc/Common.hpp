// ipc/Common.hpp
//
// The cross-platform floor every channel stands on: socket types, error
// handling, blocking full send/recv, a temp-path helper, a steady clock, and a
// CPU-relax hint. Everything Windows-vs-POSIX that ISN'T specific to one IPC
// mechanism lives here, so the channel files stay focused on their own API.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>
namespace ipc {
using socket_t = SOCKET;
inline constexpr socket_t kBadSocket = INVALID_SOCKET;
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif
namespace ipc {
using socket_t = int;
inline constexpr socket_t kBadSocket = -1;
}
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#define IPC_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define IPC_CPU_RELAX() asm volatile("yield" ::: "memory")
#else
#define IPC_CPU_RELAX() ((void)0)
#endif

namespace ipc {

[[noreturn]] inline void fail(const std::string& what) {
#if defined(_WIN32)
    throw std::runtime_error(what + " (error " + std::to_string(GetLastError()) + ")");
#else
    throw std::runtime_error(what + " (errno " + std::to_string(errno) + ": " +
                             std::strerror(errno) + ")");
#endif
}

// RAII Winsock startup (no-op on POSIX).
struct NetInit {
    NetInit() {
#if defined(_WIN32)
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) fail("WSAStartup");
#endif
    }
    ~NetInit() {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
};

inline void close_socket(socket_t s) {
    if (s == kBadSocket) return;
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

// Blocking, exactly-n transfers over a stream socket (used by TCP and AF_UNIX).
inline void socket_send_all(socket_t s, const void* p, std::size_t n) {
    const char* c = static_cast<const char*>(p);
    std::size_t sent = 0;
    while (sent < n) {
#if defined(_WIN32)
        int r = ::send(s, c + sent, static_cast<int>(n - sent), 0);
        if (r == SOCKET_ERROR) fail("send");
#else
        ssize_t r = ::send(s, c + sent, n - sent, 0);
        if (r < 0) { if (errno == EINTR) continue; fail("send"); }
#endif
        sent += static_cast<std::size_t>(r);
    }
}

inline void socket_recv_all(socket_t s, void* p, std::size_t n) {
    char* c = static_cast<char*>(p);
    std::size_t got = 0;
    while (got < n) {
#if defined(_WIN32)
        int r = ::recv(s, c + got, static_cast<int>(n - got), 0);
        if (r == SOCKET_ERROR) fail("recv");
#else
        ssize_t r = ::recv(s, c + got, n - got, 0);
        if (r < 0) { if (errno == EINTR) continue; fail("recv"); }
#endif
        if (r == 0) fail("recv: peer closed");
        got += static_cast<std::size_t>(r);
    }
}

inline void set_tcp_nodelay(socket_t s) {
    int one = 1;
#if defined(_WIN32)
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

using steady = std::chrono::steady_clock;
inline double ns_since(steady::time_point t0) {
    return std::chrono::duration<double, std::nano>(steady::now() - t0).count();
}

#if defined(_WIN32)
inline double filetime_ms(FILETIME f) {
    ULARGE_INTEGER x;
    x.LowPart = f.dwLowDateTime;
    x.HighPart = f.dwHighDateTime;
    return static_cast<double>(x.QuadPart) / 10000.0; // 100ns units -> ms
}
#endif

// CPU time (user+system) this process has consumed so far, in milliseconds.
// Differenced across the run, this is the "overhead" a mechanism costs — e.g. a
// busy-waiting spin shows up as ~1 core of CPU even while it "waits".
inline double cpu_ms_self() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return filetime_ms(k) + filetime_ms(u);
#else
    rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    auto ms = [](timeval v) { return v.tv_sec * 1000.0 + v.tv_usec / 1000.0; };
    return ms(ru.ru_utime) + ms(ru.ru_stime);
#endif
}

inline unsigned long current_pid() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Restrict the calling process to the first `n` logical CPUs (n<=0 = no limit).
// Set on both peers, it lets you watch how each mechanism behaves when cores are
// scarce — e.g. a busy-wait spinlock collapses once it can't have its own core.
inline void pin_to_cpus(int n) {
    if (n <= 0) return;
#if defined(_WIN32)
    DWORD_PTR mask = (n >= 64) ? ~static_cast<DWORD_PTR>(0)
                               : ((static_cast<DWORD_PTR>(1) << n) - 1);
    SetProcessAffinityMask(GetCurrentProcess(), mask);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < n; ++i) CPU_SET(i, &set);
    sched_setaffinity(0, sizeof(set), &set);
#else
    (void)n; // no portable affinity API (e.g. macOS) — knob is a no-op
#endif
}

// A filesystem path in the system temp dir — used for AF_UNIX sockets and FIFOs.
inline std::string temp_path(const std::string& name) {
#if defined(_WIN32)
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, dir);
    return std::string(dir, n) + name;
#else
    return "/tmp/" + name;
#endif
}

// Path to this executable, so a process can re-launch itself as the peer.
inline std::string exe_path() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf, n);
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf));
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string("./ipc_bench");
#endif
}

} // namespace ipc
