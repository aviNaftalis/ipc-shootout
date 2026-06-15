# ipc-shootout

[![ci](https://github.com/aviNaftalis/ipc-shootout/actions/workflows/ci.yml/badge.svg)](https://github.com/aviNaftalis/ipc-shootout/actions/workflows/ci.yml)

A **cross-platform C++23 benchmark of inter-process communication mechanisms.**
Same two-process ping-pong over four transports — **shared memory, Unix-domain
socket, named pipe, and TCP loopback** — measuring **latency, throughput, and
code complexity**, with notes on the **pros, cons, and portability** of each.
Builds and runs on **Linux and Windows** (proven in CI on every push).

![IPC shootout summary](docs/img/summary.png)

## What the metrics mean

- **RTT (round-trip time)** — the headline latency number: the initiator sends a
  small message, the peer echoes it straight back, and we time the full loop.
  Both timestamps are taken on the *same* clock, so no cross-process clock sync is
  needed. **One-way latency ≈ RTT / 2.**
- **Throughput** — bytes/second moved by ping-ponging a large (64 KiB) frame.
- **CPU overhead** — CPU time of *both* processes ÷ wall-clock = average cores
  kept busy. This is the cost throughput hides: shared memory is fast because it
  *busy-waits*, which pins ~2 whole cores even while "idle"; sockets and pipes
  block in the kernel, so they cost far less CPU.
- **Code complexity** — lines of code in the mechanism + how often it must branch
  on `_WIN32`.

## Results

12-core machine (WSL2 / g++ 15.2). *Median* RTT is inflated by scheduler jitter
on this host; *best* is closer to the mechanism's true floor. Windows numbers
come from CI (Actions tab).

| mechanism | RTT median | RTT best | throughput | CPU overhead | code | portability |
|---|--:|--:|--:|--:|--:|---|
| **shared memory** | **0.3 µs** | **0.2 µs** | **3500 MB/s** | 2.0 cores | 103 LOC, 2 br | both (separate mapping APIs) |
| **unix socket** | 81 µs | 8.5 µs | 546 MB/s | 1.0 cores | 56 LOC, 2 br | Linux/macOS native, Win 10 1803+ |
| **named pipe** | 86 µs | 7.4 µs | 497 MB/s | 0.9 cores | 128 LOC, 8 br | both — two different APIs |
| **tcp loopback** | 109 µs | 36 µs | 448 MB/s | 1.0 cores | 50 LOC, 0 br | universal |

What throughput alone hides: **shared memory's speed costs ~2 full cores** of
busy-wait spinning, while the kernel-mediated transports cost ~1 core total and
sleep when truly idle. Spare cores + need the latency → shared memory; CPU is
precious → the spin is a real tax.

And the same benchmark on **Windows** (from CI, `windows-latest`):

| mechanism | RTT median | throughput |
|---|--:|--:|
| **shared memory** | **0.2 µs** | **7270 MB/s** |
| **named pipe** | 28.5 µs | 1510 MB/s |
| **unix socket** | 31.5 µs | 1424 MB/s |
| **tcp loopback** | 41.2 µs | 1130 MB/s |

Shared memory is **~250× lower latency** because the data never crosses the
kernel on the hot path — the others pay two context switches per round trip. It
pays for it in **code** (you write your own sync + framing) and **CPU** (the
busy-wait spin pins a core). TCP is the slowest but the simplest and the only
one with *zero* OS-specific code.

**The OS ranking flips:** on Windows the **named pipe is the fastest
kernel-mediated transport** (Windows Named Pipes are heavily optimized),
beating AF_UNIX and TCP; on Linux the Unix socket edges out the pipe. Shared
memory wins on both. (The Windows runner is a real VM with less scheduling
jitter than this WSL2 host, so its socket latencies are also lower — compare
ranks across an OS, not absolute numbers across machines.)

## The four mechanisms — pros & cons

### 🟢 Shared memory — fastest, most work
Both processes `mmap` the same pages; a frame is a `memcpy` + an atomic flag flip.
- **+** Lowest latency, highest throughput; ideal for high-frequency or bulk data.
- **−** You implement synchronization, framing, and flow control yourself.
- **−** The busy-wait spin trades a whole CPU core for latency (or add a futex/sema → more latency).
- **Use when:** ultra-low latency on one machine, you control both ends, and a spare core is fine.

### 🔵 Unix-domain socket (AF_UNIX) — the sweet spot
Ordinary sockets, but skipping the TCP/IP stack.
- **+** Fast, simple, and portable (Linux/macOS native; Windows 10 1803+).
- **+** On Linux it can also pass file descriptors / peer credentials (not on Windows).
- **−** Still a syscall + copy per message, so far behind shared memory.
- **Use when:** the sensible default for local IPC — fast and low-effort.

> **Wait — Unix sockets on Windows?** Yes. Since **Windows 10 1803 / Server 2019**,
> Winsock ships a real `AF_UNIX` provider (the `afunix.sys` driver, `afunix.h`
> header). You call the *same* `socket()/bind()/connect()/send()/recv()` as on
> Linux, and `sockaddr_un.sun_path` is a genuine **filesystem path** (an NTFS
> file). Differences from Linux: stream-only (no `SOCK_DGRAM`), **no** abstract
> namespace (the leading-NUL trick), and **no** FD/credential passing. For a
> plain local byte stream the code is identical — which is exactly why one header
> here compiles unchanged on both OSes.

### 🟠 Named pipe / FIFO — the portability cautionary tale
- **+** Simple, named, discoverable stream.
- **−** The APIs diverge the most across OSes (POSIX FIFO vs Windows Named Pipe) — **the most code and OS branches here**; POSIX FIFOs are half-duplex, so you need two.
- **Use when:** a stream between unrelated processes without sockets — though a Unix socket usually beats it on both speed and code.

### 🔴 TCP loopback — slowest, simplest, most portable
- **+** Identical BSD-sockets API everywhere (0 OS branches); built-in framing/flow-control; trivially moves across machines later.
- **−** Pays for the whole TCP/IP stack on a local hop; needs `TCP_NODELAY` or Nagle ruins latency.
- **Use when:** portability/uniformity matters most, or the peers might not stay on one host.

## Where RPC frameworks fit

gRPC, Cap'n Proto, and protobuf-based RPC are **layers on top of these
transports** (usually TCP or a Unix socket): they add a schema, serialization,
and stub generation — convenience and cross-language interop in exchange for
extra latency on top of the numbers above. They're a different axis (encoding +
transport), not a transport themselves. A real gRPC/Cap'n Proto data point is a
natural next addition (it pulls in heavy dependencies, hence not in the core set).

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build                 # per-channel smoke test (runs on Win + Linux)

./scripts/sweep.sh                      # benchmark all channels -> results/ipc.csv
python3 scripts/plot.py                 # render docs/img/summary.png

# one mechanism by hand:
./build/ipc_bench --channel shm --lat 50000 --tput 10000
./build/ipc_bench --channel uds --small 64 --big 65536
```

## How it works

| piece | where |
|---|---|
| Common `Channel` interface (server/client, send/recv) | `include/ipc/Channel.hpp` |
| One header per mechanism | `include/ipc/channels/*.hpp` |
| Cross-platform floor (sockets, errors, temp paths, clock) | `include/ipc/Common.hpp` |
| Spawn the peer process portably (no `fork`) | `include/ipc/Process.hpp` |
| Ping-pong latency + throughput | `include/ipc/Bench.hpp` |
| Driver: one binary, server re-launches itself as the client | `src/ipc_bench.cpp` |
| CI matrix: build + test + benchmark on Ubuntu **and** Windows | `.github/workflows/ci.yml` |

Adding a mechanism is one header implementing `Channel` plus a line in the
factory in `src/ipc_bench.cpp`.

## Requirements & caveats

- CMake ≥ 3.20, a C++23 compiler (g++ 15 / MSVC 2022); Python 3 + matplotlib + numpy for the chart.
- Numbers are one host's; **the ordering and ~250× shared-memory gap are portable, absolute values are not** — re-run `sweep.sh` (and check CI for Windows).
- Throughput is measured by ping-pong (round-trip), not one-way streaming, so it's a conservative, apples-to-apples figure.
- A learning/benchmarking tool, not a production IPC library — the channels favor clarity.
