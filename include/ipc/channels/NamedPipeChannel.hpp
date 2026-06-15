// ipc/channels/NamedPipeChannel.hpp
//
// Named pipe. This is where the two OSes diverge the most:
//   POSIX   — a FIFO is half-duplex, so a bidirectional channel needs TWO of
//             them (mkfifo), opened in a careful order to avoid blocking.
//   Windows — a single full-duplex Named Pipe object (CreateNamedPipe /
//             CreateFile), read/written with ReadFile/WriteFile.
// Same concept, almost no shared code — the cautionary tale for the
// "code complexity / portability" axis.

#pragma once

#include <string>

#include "ipc/Channel.hpp"
#include "ipc/Common.hpp"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace ipc {

class NamedPipeChannel : public Channel {
public:
    explicit NamedPipeChannel(Role role) : role_(role) {}

    ~NamedPipeChannel() override {
#if defined(_WIN32)
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
#else
        if (rd_ >= 0) ::close(rd_);
        if (wr_ >= 0) ::close(wr_);
        if (role_ == Role::Server && !base_.empty()) {
            ::unlink((base_ + "_s2c").c_str());
            ::unlink((base_ + "_c2s").c_str());
        }
#endif
    }

    std::string setup_server() override {
        const std::string id = std::to_string(current_pid());
#if defined(_WIN32)
        name_ = "\\\\.\\pipe\\ipc_" + id;
        pipe_ = CreateNamedPipeA(name_.c_str(), PIPE_ACCESS_DUPLEX,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                                 1 << 20, 1 << 20, 0, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) fail("CreateNamedPipe");
        return id;
#else
        base_ = temp_path("ipc_fifo_" + id);
        ::unlink((base_ + "_s2c").c_str());
        ::unlink((base_ + "_c2s").c_str());
        if (mkfifo((base_ + "_s2c").c_str(), 0600) != 0) fail("mkfifo s2c");
        if (mkfifo((base_ + "_c2s").c_str(), 0600) != 0) fail("mkfifo c2s");
        return base_;
#endif
    }

    void accept_client() override {
#if defined(_WIN32)
        if (!ConnectNamedPipe(pipe_, nullptr) &&
            GetLastError() != ERROR_PIPE_CONNECTED)
            fail("ConnectNamedPipe");
#else
        // Open order mirrors the client's so neither blocks forever.
        wr_ = open_fifo(base_ + "_s2c", O_WRONLY);
        rd_ = open_fifo(base_ + "_c2s", O_RDONLY);
#endif
    }

    void connect_to(const std::string& endpoint) override {
#if defined(_WIN32)
        name_ = "\\\\.\\pipe\\ipc_" + endpoint;
        for (;;) {
            pipe_ = CreateFileA(name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) break;
            if (GetLastError() == ERROR_PIPE_BUSY) { WaitNamedPipeA(name_.c_str(), 2000); continue; }
            Sleep(1); // server may not have created the pipe yet
        }
        DWORD mode = PIPE_READMODE_BYTE;
        SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr);
#else
        base_ = endpoint;
        rd_ = open_fifo(base_ + "_s2c", O_RDONLY);
        wr_ = open_fifo(base_ + "_c2s", O_WRONLY);
#endif
    }

    void send(const void* p, std::size_t n) override {
#if defined(_WIN32)
        const char* c = static_cast<const char*>(p);
        DWORD done = 0;
        for (std::size_t s = 0; s < n; s += done)
            if (!WriteFile(pipe_, c + s, static_cast<DWORD>(n - s), &done, nullptr))
                fail("WriteFile");
#else
        write_all(wr_, p, n);
#endif
    }

    void recv(void* p, std::size_t n) override {
#if defined(_WIN32)
        char* c = static_cast<char*>(p);
        DWORD done = 0;
        for (std::size_t g = 0; g < n; g += done)
            if (!ReadFile(pipe_, c + g, static_cast<DWORD>(n - g), &done, nullptr) || done == 0)
                fail("ReadFile");
#else
        read_all(rd_, p, n);
#endif
    }

    const char* portability() const override { return "both, but two different APIs (FIFO vs Named Pipe)"; }

private:
    Role role_;
#if defined(_WIN32)
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::string name_;
#else
    int rd_ = -1, wr_ = -1;
    std::string base_;
    static int open_fifo(const std::string& path, int flags) {
        int fd;
        while ((fd = ::open(path.c_str(), flags)) < 0) {
            if (errno == EINTR || errno == ENOENT) continue; // peer not ready yet
            fail("open fifo");
        }
        return fd;
    }
    static void write_all(int fd, const void* p, std::size_t n) {
        const char* c = static_cast<const char*>(p);
        for (std::size_t s = 0; s < n;) {
            ssize_t r = ::write(fd, c + s, n - s);
            if (r < 0) { if (errno == EINTR) continue; fail("write"); }
            s += static_cast<std::size_t>(r);
        }
    }
    static void read_all(int fd, void* p, std::size_t n) {
        char* c = static_cast<char*>(p);
        for (std::size_t g = 0; g < n;) {
            ssize_t r = ::read(fd, c + g, n - g);
            if (r < 0) { if (errno == EINTR) continue; fail("read"); }
            if (r == 0) fail("fifo: peer closed");
            g += static_cast<std::size_t>(r);
        }
    }
#endif
};

} // namespace ipc
