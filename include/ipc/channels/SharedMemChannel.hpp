// ipc/channels/SharedMemChannel.hpp
//
// Shared memory: the fastest IPC there is — both processes map the same pages
// and the data never goes through the kernel on the hot path. The cost is the
// most code (mapping + your own synchronization + framing) and the least
// portability polish (POSIX shm_open+mmap vs Win32 CreateFileMapping).
//
// Two single-slot mailboxes (one per direction); the reader/writer hand off with
// an atomic flag spun on in user space. Spinning = lowest latency but it burns a
// core while waiting (the classic shared-memory trade-off).

#pragma once

#include <atomic>
#include <cstring>
#include <new>
#include <string>

#include "ipc/Channel.hpp"
#include "ipc/Common.hpp"

#if !defined(_WIN32)
#include <sys/mman.h>
#endif

namespace ipc {

class SharedMemChannel : public Channel {
public:
    static constexpr std::size_t kCap = 1u << 18; // 256 KiB max frame

    explicit SharedMemChannel(Role role) : role_(role) {}
    ~SharedMemChannel() override { unmap(); }

    std::string setup_server() override {
        id_ = std::to_string(current_pid());
        map(/*create=*/true);
        new (region_) Region{}; // zero-init the atomics/flags (server only)
        wire_up();
        return id_;
    }
    void accept_client() override {} // no handshake: peers rendezvous on the flags

    void connect_to(const std::string& endpoint) override {
        id_ = endpoint;
        map(/*create=*/false);
        wire_up();
    }

    void send(const void* p, std::size_t n) override {
        while (tx_->ready.load(std::memory_order_acquire) != 0) IPC_CPU_RELAX();
        std::memcpy(tx_->buf, p, n);
        tx_->len = static_cast<std::uint32_t>(n);
        tx_->ready.store(1, std::memory_order_release);
    }
    void recv(void* p, std::size_t n) override {
        while (rx_->ready.load(std::memory_order_acquire) != 1) IPC_CPU_RELAX();
        std::memcpy(p, rx_->buf, n);
        rx_->ready.store(0, std::memory_order_release);
    }
    const char* portability() const override { return "both, but separate POSIX / Win32 mapping APIs"; }

private:
    struct Mailbox {
        std::atomic<std::uint32_t> ready;
        std::uint32_t len;
        char buf[kCap];
    };
    struct Region {
        Mailbox s2c; // server -> client
        Mailbox c2s; // client -> server
    };

    void wire_up() {
        if (role_ == Role::Server) { tx_ = &region_->s2c; rx_ = &region_->c2s; }
        else                       { tx_ = &region_->c2s; rx_ = &region_->s2c; }
    }

#if defined(_WIN32)
    void map(bool create) {
        const std::string name = "Local\\ipc_shm_" + id_;
        if (create) {
            handle_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         sizeof(Region), name.c_str());
            if (!handle_) fail("CreateFileMapping");
        } else {
            while (!(handle_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str())))
                Sleep(1); // wait for the server to create it
        }
        region_ = static_cast<Region*>(
            MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Region)));
        if (!region_) fail("MapViewOfFile");
    }
    void unmap() {
        if (region_) UnmapViewOfFile(region_);
        if (handle_) CloseHandle(handle_);
    }
    HANDLE handle_ = nullptr;
#else
    void map(bool create) {
        name_ = "/ipc_shm_" + id_;
        int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
        int fd;
        while ((fd = ::shm_open(name_.c_str(), flags, 0600)) < 0) {
            if (!create && errno == ENOENT) continue; // wait for the server
            fail("shm_open");
        }
        if (create && ::ftruncate(fd, sizeof(Region)) != 0) fail("ftruncate");
        region_ = static_cast<Region*>(::mmap(nullptr, sizeof(Region),
                                              PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        ::close(fd);
        if (region_ == MAP_FAILED) { region_ = nullptr; fail("mmap"); }
    }
    void unmap() {
        if (region_) ::munmap(region_, sizeof(Region));
        if (role_ == Role::Server && !name_.empty()) ::shm_unlink(name_.c_str());
    }
    std::string name_;
#endif

    Role role_;
    std::string id_;
    Region* region_ = nullptr;
    Mailbox* tx_ = nullptr;
    Mailbox* rx_ = nullptr;
};

} // namespace ipc
