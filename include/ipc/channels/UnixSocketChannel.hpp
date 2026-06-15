// ipc/channels/UnixSocketChannel.hpp
//
// A Unix-domain socket (AF_UNIX). Same sockets API as TCP but it skips the
// TCP/IP stack entirely, so it's markedly faster on loopback while staying
// almost as portable: native on Linux/macOS and supported on Windows 10 1803+
// (the sockaddr_un path is a real filesystem path on all of them). The sweet
// spot — fast and not much more code than TCP.

#pragma once

#include <string>

#include "ipc/Channel.hpp"
#include "ipc/Common.hpp"

namespace ipc {

class UnixSocketChannel : public Channel {
public:
    ~UnixSocketChannel() override {
        close_socket(conn_);
        close_socket(listener_);
#if !defined(_WIN32)
        if (!path_.empty()) ::unlink(path_.c_str());
#endif
    }

    std::string setup_server() override {
        path_ = temp_path("ipc_uds_" + std::to_string(current_pid()) + ".sock");
#if !defined(_WIN32)
        ::unlink(path_.c_str());
#else
        DeleteFileA(path_.c_str());
#endif
        listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener_ == kBadSocket) fail("socket(AF_UNIX)");
        sockaddr_un addr = make_addr(path_);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            fail("bind(AF_UNIX)");
        if (::listen(listener_, 1) != 0) fail("listen");
        return path_;
    }

    void accept_client() override {
        conn_ = ::accept(listener_, nullptr, nullptr);
        if (conn_ == kBadSocket) fail("accept");
    }

    void connect_to(const std::string& endpoint) override {
        path_.clear(); // client does not own the path / shouldn't unlink it
        conn_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (conn_ == kBadSocket) fail("socket(AF_UNIX)");
        sockaddr_un addr = make_addr(endpoint);
        while (::connect(conn_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            // wait for the server to bind/listen
        }
    }

    void send(const void* p, std::size_t n) override { socket_send_all(conn_, p, n); }
    void recv(void* p, std::size_t n) override { socket_recv_all(conn_, p, n); }
    const char* portability() const override { return "Linux/macOS native; Windows 10 1803+"; }

private:
    static sockaddr_un make_addr(const std::string& path) {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
        return addr;
    }
    socket_t listener_ = kBadSocket;
    socket_t conn_ = kBadSocket;
    std::string path_;
};

} // namespace ipc
