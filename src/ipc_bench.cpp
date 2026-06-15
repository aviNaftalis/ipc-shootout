// src/ipc_bench.cpp
//
// One executable, two roles. With no --role it is the SERVER: it creates the
// endpoint, re-launches itself as the CLIENT peer, runs the timed ping-pong as
// the initiator, and prints a CSV row. The child connects and echoes.
//
//   ipc_bench --channel shm                 # run the shared-memory benchmark
//   ipc_bench --channel tcp --small 64 --big 65536
//
// Channels: tcp  uds  pipe  shm

#include <charconv>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ipc/Bench.hpp"
#include "ipc/Channel.hpp"
#include "ipc/Common.hpp"
#include "ipc/Process.hpp"
#include "ipc/channels/NamedPipeChannel.hpp"
#include "ipc/channels/SharedMemChannel.hpp"
#include "ipc/channels/TcpChannel.hpp"
#include "ipc/channels/UnixSocketChannel.hpp"

namespace ipc {
std::unique_ptr<Channel> make_channel(const std::string& name, Role role) {
    if (name == "tcp") return std::make_unique<TcpChannel>();
    if (name == "uds") return std::make_unique<UnixSocketChannel>();
    if (name == "pipe") return std::make_unique<NamedPipeChannel>(role);
    if (name == "shm") return std::make_unique<SharedMemChannel>(role);
    fail("unknown channel: " + name);
}
} // namespace ipc

using namespace ipc;

namespace {
long arg_long(std::string_view v, long fb) {
    long out{};
    auto [pp, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return ec == std::errc{} ? out : fb;
}
} // namespace

int main(int argc, char** argv) {
    std::string role = "server", channel = "tcp", endpoint;
    BenchParams p;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string_view k = argv[i], v = argv[i + 1];
        if (k == "--role") role = v;
        else if (k == "--channel") channel = v;
        else if (k == "--endpoint") endpoint = v;
        else if (k == "--small") p.small = static_cast<std::size_t>(arg_long(v, 64));
        else if (k == "--big") p.big = static_cast<std::size_t>(arg_long(v, 65536));
        else if (k == "--warmup") p.warmup = static_cast<int>(arg_long(v, p.warmup));
        else if (k == "--lat") p.lat_iters = static_cast<int>(arg_long(v, p.lat_iters));
        else if (k == "--tput") p.tput_iters = static_cast<int>(arg_long(v, p.tput_iters));
    }

    try {
        NetInit net;
        if (role == "client") {
            auto ch = make_channel(channel, Role::Client);
            ch->connect_to(endpoint);
            run_echoer(*ch, p);
            return 0;
        }

        // server / orchestrator
        auto ch = make_channel(channel, Role::Server);
        const std::string ep = ch->setup_server();
        ProcHandle child = spawn_self({
            "--role", "client", "--channel", channel, "--endpoint", ep,
            "--small", std::to_string(p.small), "--big", std::to_string(p.big),
            "--warmup", std::to_string(p.warmup), "--lat", std::to_string(p.lat_iters),
            "--tput", std::to_string(p.tput_iters),
        });
        ch->accept_client();

        // Wrap the whole run to measure CPU overhead (both processes / wall).
        const double cpu0 = cpu_ms_self();
        const auto wall0 = steady::now();
        Metrics m = run_initiator(*ch, p);
        const double wall_ms = ns_since(wall0) / 1e6;
        const double self_cpu = cpu_ms_self() - cpu0;
        const WaitResult wr = wait_for(child);
        m.cores = wall_ms > 0 ? (self_cpu + wr.child_cpu_ms) / wall_ms : 0;

        std::fprintf(stderr,
                     "%-5s | RTT %8.0f ns (1-way ~%6.0f) | throughput %7.1f MB/s | "
                     "CPU %4.2f cores | %s\n",
                     channel.c_str(), m.rtt_med_ns, m.rtt_med_ns / 2, m.throughput_MBps,
                     m.cores, ch->portability());
        // CSV: channel,small_bytes,rtt_med_ns,rtt_min_ns,big_bytes,throughput_MBps,cores
        std::printf("CSV,%s,%zu,%.1f,%.1f,%zu,%.1f,%.3f\n", channel.c_str(), p.small,
                    m.rtt_med_ns, m.rtt_min_ns, p.big, m.throughput_MBps, m.cores);
        return wr.code;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[%s] error: %s\n", role.c_str(), e.what());
        return 1;
    }
}
