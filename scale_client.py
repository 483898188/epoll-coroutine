#!/usr/bin/env python3
"""Scaling client with marker-file coordination so perf can be attached to the
server exactly during the request rounds.

Usage: scale_client.py <n_conns> <port> <batch> <tag>

Marker files: /tmp/scale_{ready,go,done}_<tag>

Protocol is the bare line protocol of test_server.cc: send "apple\\n",
expect "A fruit that is round and red or green.".
"""
import asyncio
import os
import socket as pysock
import struct
import sys
import time

HOST = "127.0.0.1"


def fds(pid):
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return -1


def rss_mb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS"):
                    return int(line.split()[1]) // 1024
    except OSError:
        pass
    return -1


async def main():
    n = int(sys.argv[1])
    port = int(sys.argv[2])
    batch = int(sys.argv[3])
    tag = sys.argv[4]

    ready = f"/tmp/scale_ready_{tag}"
    go = f"/tmp/scale_go_{tag}"
    done = f"/tmp/scale_done_{tag}"
    for p in (ready, go, done):
        if os.path.exists(p):
            os.unlink(p)

    spid = int(os.popen("pgrep -x test_server").read().split()[0])
    out = []

    def log(s):
        print(s, flush=True)
        out.append(s)

    # ---------- phase 1: establish ----------
    conns, failed = [], 0
    t0 = time.perf_counter()
    for start in range(0, n, batch):
        chunk = range(start, min(start + batch, n))
        res = await asyncio.gather(
            *[asyncio.open_connection(HOST, port) for _ in chunk],
            return_exceptions=True,
        )
        for r in res:
            if isinstance(r, Exception):
                failed += 1
            else:
                conns.append(r)
    t_conn = time.perf_counter() - t0
    log(f"established {len(conns)}/{n} in {t_conn:.2f}s "
        f"({len(conns)/t_conn:.0f}/s) failed={failed}")
    log(f"server fds={fds(spid)} rss={rss_mb(spid)}MB")

    # RST on close -> no TIME_WAIT, so scale points stay independent
    for (_, w) in conns:
        s = w.get_extra_info("socket")
        if s is not None:
            try:
                s.setsockopt(pysock.SOL_SOCKET, pysock.SO_LINGER,
                             struct.pack("ii", 1, 0))
            except OSError:
                pass

    await asyncio.sleep(1.0)

    # ---------- phase 2: ready, wait for GO ----------
    open(ready, "w").close()
    while not os.path.exists(go):
        await asyncio.sleep(0.05)

    # ---------- phase 3: request rounds (perf attached) ----------
    expect = b"A fruit that is round and red or green."
    rounds = max(5, 50000 // n)
    ok = bad = 0
    lat = []
    t_start = time.perf_counter()

    async def one(reader, writer):
        t = time.perf_counter()
        writer.write(b"apple\n")
        await writer.drain()
        data = await asyncio.wait_for(reader.readuntil(b"."), timeout=20)
        lat.append(time.perf_counter() - t)
        return data == expect

    for r in range(rounds):
        res = await asyncio.gather(*[one(rd, wr) for (rd, wr) in conns],
                                   return_exceptions=True)
        for x in res:
            if x is True:
                ok += 1
            else:
                bad += 1
    t_rounds = time.perf_counter() - t_start
    lat.sort()

    open(done, "w").close()

    log(f"rounds={rounds} wall={t_rounds:.2f}s ok={ok} bad={bad} "
        f"-> {ok/t_rounds:.0f} req/s")
    if lat:
        p = lambda q: lat[min(len(lat) - 1, int(len(lat) * q))] * 1e6
        log(f"latency_us p50={p(.5):.0f} p90={p(.9):.0f} p99={p(.99):.0f} "
            f"max={lat[-1]*1e6:.0f}")
    log(f"server fds={fds(spid)} rss={rss_mb(spid)}MB")

    # ---------- phase 4: close ----------
    # Hold before closing so the perf window can end while nothing but idle
    # epoll_wait is happening; teardown syscalls would otherwise be counted.
    await asyncio.sleep(25)
    for (_, w) in conns:
        try:
            w.close()
        except OSError:
            pass
    await asyncio.sleep(2.0)
    log(f"after close: server fds={fds(spid)} rss={rss_mb(spid)}MB")

    with open(f"/tmp/scale_result_{tag}.txt", "w") as f:
        f.write("\n".join(out) + "\n")


asyncio.run(main())
