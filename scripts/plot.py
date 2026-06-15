#!/usr/bin/env python3
"""Render the IPC comparison chart from results/ipc.csv into docs/img/summary.png.

Three panels: round-trip latency, throughput, and code complexity (lines of
code + number of OS branches, read straight from the channel headers).

Usage: python3 scripts/plot.py [results/ipc.csv]   (needs matplotlib + numpy)
"""
import csv
import os
import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "results", "ipc.csv")
IMG = os.path.join(ROOT, "docs", "img")
os.makedirs(IMG, exist_ok=True)

ORDER = ["shm", "uds", "pipe", "tcp"]  # fastest -> slowest, by latency
NICE = {"shm": "shared memory", "uds": "unix socket", "pipe": "named pipe", "tcp": "tcp loopback"}
COLOR = {"shm": "#2ca02c", "uds": "#1f77b4", "pipe": "#ff7f0e", "tcp": "#d62728"}
HEADER = {"shm": "SharedMemChannel.hpp", "uds": "UnixSocketChannel.hpp",
          "pipe": "NamedPipeChannel.hpp", "tcp": "TcpChannel.hpp"}


def complexity(channel):
    """(lines of code, number of _WIN32 OS branches) for a channel's header."""
    path = os.path.join(ROOT, "include", "ipc", "channels", HEADER[channel])
    loc, branches = 0, 0
    in_block = False
    for line in open(path):
        s = line.strip()
        if in_block:
            if "*/" in s:
                in_block = False
            continue
        if s.startswith("/*"):
            in_block = "*/" not in s
            continue
        if s and not s.startswith("//"):
            loc += 1
        branches += len(re.findall(r"_WIN32", line))
    return loc, branches


def load():
    rows = {}
    with open(CSV, newline="") as f:
        for r in csv.DictReader(f):
            rows[r["channel"]] = r
    return rows


def plot_cpus():
    """RTT and throughput vs number of available CPUs — the key 'use case' knob.
    Busy-wait shared memory collapses at 1 core; blocking transports don't."""
    path = os.path.join(os.path.dirname(CSV), "cpus.csv")
    if not os.path.exists(path):
        return
    data = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            data.setdefault(r["channel"], []).append(
                (int(r["cpus"]), float(r["rtt_med_ns"]), float(r["throughput_MBps"])))
    fig, (a_rtt, a_tp) = plt.subplots(1, 2, figsize=(13, 5))
    for c in ORDER:
        if c not in data:
            continue
        pts = sorted(data[c])
        xs = [p[0] for p in pts]
        a_rtt.plot(xs, [p[1] for p in pts], "-o", color=COLOR[c], label=NICE[c], lw=2)
        a_tp.plot(xs, [p[2] for p in pts], "-o", color=COLOR[c], label=NICE[c], lw=2)
    for a in (a_rtt, a_tp):
        a.set_xscale("log", base=2)
        a.set_xlabel("CPUs allowed (both processes pinned)")
        a.grid(True, which="both", alpha=0.3)
    a_rtt.set_yscale("log")
    a_rtt.set_ylabel("round-trip latency (ns, log) — lower is better")
    a_rtt.set_title("Latency vs available CPUs")
    a_tp.set_yscale("log")
    a_tp.set_ylabel("throughput (MB/s, log) — higher is better")
    a_tp.set_title("Throughput vs available CPUs")
    a_rtt.legend(fontsize=9)
    fig.suptitle("Squeeze the CPUs: shared memory's busy-wait collapses at 1 core "
                 "(~8 ms/round-trip); blocking transports get faster (same-core handoff)",
                 fontsize=12)
    fig.tight_layout()
    fig.savefig(os.path.join(IMG, "cpus.png"), dpi=130)
    plt.close(fig)
    print(f"Wrote {IMG}/cpus.png")


def main():
    plot_cpus()
    rows = load()
    chans = [c for c in ORDER if c in rows]
    labels = [NICE[c] for c in chans]
    colors = [COLOR[c] for c in chans]
    rtt = [float(rows[c]["rtt_med_ns"]) for c in chans]
    tput = [float(rows[c]["throughput_MBps"]) for c in chans]
    cores = [float(rows[c].get("cores", 0)) for c in chans]
    loc = [complexity(c)[0] for c in chans]
    branches = [complexity(c)[1] for c in chans]

    fig, axes = plt.subplots(2, 2, figsize=(13, 9))

    a = axes[0][0]
    a.bar(labels, rtt, color=colors)
    a.set_yscale("log")
    a.set_ylabel("round-trip latency (ns, log) — lower is better")
    a.set_title("Latency (RTT = send + echo back)")
    for i, v in enumerate(rtt):
        a.text(i, v, f"{v:,.0f} ns", ha="center", va="bottom", fontsize=9)

    a = axes[0][1]
    a.bar(labels, tput, color=colors)
    a.set_ylabel("ping-pong throughput (MB/s) — higher is better")
    a.set_title("Throughput")
    for i, v in enumerate(tput):
        a.text(i, v, f"{v:,.0f}", ha="center", va="bottom", fontsize=9)

    a = axes[1][0]
    a.bar(labels, cores, color=colors)
    a.set_ylabel("CPU cores busied (both processes) — lower is cheaper")
    a.set_title("CPU overhead — what throughput hides")
    for i, v in enumerate(cores):
        a.text(i, v, f"{v:.2f}", ha="center", va="bottom", fontsize=9)

    a = axes[1][1]
    a.bar(labels, loc, color=colors)
    a.set_ylabel("lines of code — lower is simpler")
    a.set_title("Code complexity (impl size + OS branches)")
    for i, (l, b) in enumerate(zip(loc, branches)):
        a.text(i, l, f"{l} LOC\n{b} OS branches", ha="center", va="bottom", fontsize=9)

    for row in axes:
        for a in row:
            a.tick_params(axis="x", rotation=12)

    fig.suptitle("IPC shootout: shared memory wins latency & throughput — but its busy-wait "
                 "burns ~2 cores; sockets/pipes are cheap CPU but slower", fontsize=13)
    fig.tight_layout()
    fig.savefig(os.path.join(IMG, "summary.png"), dpi=130)
    plt.close(fig)
    print(f"Wrote {IMG}/summary.png")


if __name__ == "__main__":
    main()
