// ipc/channels/TcpChannel.hpp
//
// TCP over the loopback interface. The most portable mechanism (the same BSD
// sockets API on every OS, give or take a Winsock shim) and the simplest mental
// model — but it pays for the full TCP/IP stack even though it never leaves the
// machine, so it's the latency baseline. TCP_NODELAY is essential or Nagle's
// algorithm wrecks ping-pong latency.

#pragma once

#include <string>

#include "ipc/Channel.hpp"
#include "ipc/Common.hpp"

namespace ipc {

class TcpChannel : public Channel {
public:
    ~TcpChannel() override {
        close_socket(conn_);
        close_socket(listener_);
    }

    std::string setup_server() override {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ == kBadSocket) fail("socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // let the OS pick a free port
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            fail("bind");
        if (::listen(listener_, 1) != 0) fail("listen");
        socklen_t len = sizeof(addr);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            fail("getsockname");
        return std::to_string(ntohs(addr.sin_port));
    }

    void accept_client() override {
        conn_ = ::accept(listener_, nullptr, nullptr);
        if (conn_ == kBadSocket) fail("accept");
        set_tcp_nodelay(conn_);
    }

    void connect_to(const std::string& endpoint) override {
        conn_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (conn_ == kBadSocket) fail("socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<unsigned short>(std::stoi(endpoint)));
        while (::connect(conn_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            // server may not have called accept() yet; retry briefly
        }
        set_tcp_nodelay(conn_);
    }

    void send(const void* p, std::size_t n) override { socket_send_all(conn_, p, n); }
    void recv(void* p, std::size_t n) override { socket_recv_all(conn_, p, n); }
    const char* portability() const override { return "universal (BSD sockets / Winsock)"; }

private:
    socket_t listener_ = kBadSocket;
    socket_t conn_ = kBadSocket;
};

} // namespace ipc
