// ipc/Channel.hpp
//
// The common interface every IPC mechanism implements, so the benchmark driver
// is mechanism-agnostic. A run has two processes: the *server* sets up an
// endpoint and the *client* connects to it; then either side can send/recv
// fixed-size frames. The polymorphic call is nanoseconds — negligible next to
// even shared-memory IPC latency.

#pragma once

#include <memory>
#include <string>

namespace ipc {

enum class Role { Server, Client };

struct Channel {
    virtual ~Channel() = default;

    // Server: create the endpoint; returns an id (port / path / name) the client
    // needs. Client: ignore and use connect().
    virtual std::string setup_server() = 0;
    // Server: block until the client has connected.
    virtual void accept_client() = 0;
    // Client: connect to the endpoint id produced by the server.
    virtual void connect_to(const std::string& endpoint) = 0;

    virtual void send(const void* p, std::size_t n) = 0;
    virtual void recv(void* p, std::size_t n) = 0;

    // One-line portability note for the README table.
    virtual const char* portability() const = 0;
};

// role matters for mechanisms whose two directions are asymmetric (FIFO, shm).
std::unique_ptr<Channel> make_channel(const std::string& name, Role role);

} // namespace ipc
