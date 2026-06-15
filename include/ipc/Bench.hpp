// ipc/Bench.hpp
//
// The measurement, identical for every channel: a ping-pong. The initiator
// sends a frame and waits for the echo; the echoer bounces it straight back.
//   * latency   — round-trip time of a small frame (report median; one-way ~ rtt/2)
//   * throughput — same ping-pong with a large frame; bytes moved / time
// Both sides run the exact same schedule (warmup, then latency, then throughput)
// so the counts always line up.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ipc/Channel.hpp"
#include "ipc/Common.hpp"

namespace ipc {

struct BenchParams {
    std::size_t small = 64;        // latency frame
    std::size_t big = 64 * 1024;   // throughput frame
    int warmup = 2000;
    int lat_iters = 50000;
    int tput_iters = 20000;
};

struct Metrics {
    double rtt_med_ns = 0;
    double rtt_min_ns = 0;
    double throughput_MBps = 0;
    double cores = 0; // CPU overhead: avg cores busied (both processes) during the run
};

inline Metrics run_initiator(Channel& ch, const BenchParams& p) {
    std::vector<char> buf(p.big, 'x');

    auto pingpong = [&](std::size_t sz) { ch.send(buf.data(), sz); ch.recv(buf.data(), sz); };

    for (int i = 0; i < p.warmup; ++i) pingpong(p.small);

    std::vector<double> rtt;
    rtt.reserve(static_cast<std::size_t>(p.lat_iters));
    for (int i = 0; i < p.lat_iters; ++i) {
        auto t0 = steady::now();
        pingpong(p.small);
        rtt.push_back(ns_since(t0));
    }
    std::ranges::sort(rtt);

    const auto t0 = steady::now();
    for (int i = 0; i < p.tput_iters; ++i) pingpong(p.big);
    const double secs = ns_since(t0) / 1e9;

    Metrics m;
    m.rtt_med_ns = rtt[rtt.size() / 2];
    m.rtt_min_ns = rtt.front();
    // one direction's worth of bytes per round trip
    m.throughput_MBps = (static_cast<double>(p.tput_iters) * p.big) / secs / 1e6;
    return m;
}

inline void run_echoer(Channel& ch, const BenchParams& p) {
    std::vector<char> buf(p.big);
    auto echo = [&](std::size_t sz) { ch.recv(buf.data(), sz); ch.send(buf.data(), sz); };
    for (int i = 0; i < p.warmup; ++i) echo(p.small);
    for (int i = 0; i < p.lat_iters; ++i) echo(p.small);
    for (int i = 0; i < p.tput_iters; ++i) echo(p.big);
}

} // namespace ipc
