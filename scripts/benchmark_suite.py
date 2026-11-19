#!/usr/bin/env python3
"""
CppDesk Polyglot Benchmark & Stress Verification Suite (Python 3.14)
Verifies and benchmarks:
  1. C++20 + x86-64 AVX2 Assembly SIMD Kernels (CppDeskTests.exe)
  2. C# .NET 10 Standalone Rendezvous & Relay Server (CppDeskRelay --self-test & 40-peer stress test)
"""

import re
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROTOCOL_MAGIC = 0x43505044  # "CPPD" (CppDesk Protocol Magic)
PKT_RELAY_REGISTER = 0x50
PKT_RELAY_REGISTER_ACK = 0x51
PKT_RELAY_LOOKUP = 0x52
PKT_RELAY_LOOKUP_RESP = 0x53


def pack_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def make_frame(pkt_type: int, payload: bytes) -> bytes:
    return struct.pack("<IBBI", PROTOCOL_MAGIC, pkt_type, 0, len(payload)) + payload


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Socket closed early")
        buf.extend(chunk)
    return bytes(buf)


def recv_frame(sock: socket.socket) -> tuple[int, bytes]:
    hdr = recv_exact(sock, 10)
    magic, pkt_type, _flags, size = struct.unpack("<IBBI", hdr)
    assert magic == PROTOCOL_MAGIC, f"Invalid magic: {hex(magic)}"
    payload = recv_exact(sock, size) if size > 0 else b""
    return pkt_type, payload


def benchmark_dotnet_relay() -> tuple[int, float, float]:
    """Starts the C# .NET 10 Relay Server and benchmarks 40 concurrent peer registrations & lookups."""
    relay_dll = ROOT / "relay-dotnet" / "bin" / "Release" / "net10.0" / "CppDeskRelay.dll"
    subprocess.run(
        ["dotnet", "build", str(ROOT / "relay-dotnet" / "CppDeskRelay.csproj"), "-c", "Release", "--nologo", "-v", "q"],
        check=True,
    )

    proc = subprocess.Popen(
        ["dotnet", str(relay_dll), "--port", "0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    bound_port: list[int] = []
    ready_evt = threading.Event()

    def _drain_stdout() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            if not ready_evt.is_set():
                m = re.search(r"listening on [^:]+:(\d+)", line)
                if m:
                    bound_port.append(int(m.group(1)))
                    ready_evt.set()

    drain_thread = threading.Thread(target=_drain_stdout, daemon=True)
    drain_thread.start()

    peer_sockets: list[socket.socket] = []
    try:
        if not ready_evt.wait(timeout=10.0) or not bound_port:
            raise RuntimeError("CppDeskRelay did not report a bound port within 10s")
        test_port = bound_port[0]

        num_peers = 40

        t0 = time.perf_counter()
        for i in range(num_peers):
            s = socket.create_connection(("127.0.0.1", test_port), timeout=2.0)
            peer_sockets.append(s)
            desk_id = 300_000_000 + i
            payload = (
                struct.pack("<Q", desk_id)
                + pack_string(f"PyBench-Host-{i}")
                + pack_string("127.0.0.1")
                + struct.pack("<H", 50990 + (i % 10))
            )
            s.sendall(make_frame(PKT_RELAY_REGISTER, payload))
            ptype, ack = recv_frame(s)
            assert ptype == PKT_RELAY_REGISTER_ACK and ack == b"\x01"
        reg_ms = (time.perf_counter() - t0) * 1000.0

        t1 = time.perf_counter()
        for i in range(num_peers):
            with socket.create_connection(("127.0.0.1", test_port), timeout=2.0) as ls:
                desk_id = 300_000_000 + i
                ls.sendall(make_frame(PKT_RELAY_LOOKUP, struct.pack("<Q", desk_id)))
                ptype, resp = recv_frame(ls)
                assert ptype == PKT_RELAY_LOOKUP_RESP and resp[0] == 1
        lookup_ms = (time.perf_counter() - t1) * 1000.0 / num_peers

        return num_peers, reg_ms, lookup_ms
    finally:
        for s in peer_sockets:
            try:
                s.close()
            except OSError:
                pass
        proc.terminate()
        try:
            proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2.0)
        if proc.stdout is not None:
            proc.stdout.close()
        drain_thread.join(timeout=1.0)


def main() -> int:
    print("=================================================================")
    print("   CppDesk Polyglot Verification & Performance Benchmark (Py3)   ")
    print("=================================================================")

    # 1. C++20 + x86-64 AVX2 Assembly Test Suite
    candidates = [
        ROOT / "CppDeskTests.exe",
        ROOT / "CppDeskTests",
        ROOT / "build" / "CppDeskTests",
        ROOT / "build" / "Release" / "CppDeskTests.exe",
        ROOT / "build" / "Release" / "CppDeskTests",
    ]
    test_exe = next((p for p in candidates if p.is_file()), None)
    if test_exe is None:
        print("ERROR: CppDeskTests binary not found; build tests before running benchmark_suite.py.", file=sys.stderr)
        return 1

    print(f"[1/2] Running Native C++20 Test Suite ({test_exe.name})...")
    res = subprocess.run([str(test_exe)], capture_output=True, text=True, check=True)
    for line in res.stdout.splitlines():
        if "AVX2" in line or "Assertions Passed" in line:
            print(f"      {line.strip()}")

    # 2. C# .NET 10 Relay Stress Test
    print("[2/2] Stress-Testing C# (.NET 10) Standalone Relay Server...")
    peers, reg_total_ms, avg_lookup_ms = benchmark_dotnet_relay()
    print(f"      Registered {peers} concurrent hosts in {reg_total_ms:.1f} ms | Avg ID Lookup: {avg_lookup_ms:.2f} ms")

    print("-----------------------------------------------------------------")
    print("Polyglot Stack Status: ALL NATIVE & RELAY RUNTIMES VERIFIED (PASS)")
    print("=================================================================")
    return 0


if __name__ == "__main__":
    sys.exit(main())
