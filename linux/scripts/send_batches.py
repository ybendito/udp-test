#!/usr/bin/env python3
"""Minimal BDUP sender for Linux server tests (matches packet.hpp layout)."""
import argparse
import socket
import struct

MAGIC = 0x42554450
HEADER_SIZE = 20


def _crc32_update(crc: int, byte: int) -> int:
    crc ^= byte
    for _ in range(8):
        mask = -(crc & 1) & 0xFFFFFFFF
        crc = ((crc >> 1) ^ (0xEDB88320 & mask)) & 0xFFFFFFFF
    return crc


def crc32_compute(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc = _crc32_update(crc, b)
    return crc ^ 0xFFFFFFFF


def batch_crc32(data: bytes) -> int:
    buf = bytearray(data)
    struct.pack_into("<I", buf, 16, 0)
    return crc32_compute(bytes(buf))


def build_batch(seq: int, batch_total: int, size: int, seed: int) -> bytes:
    if size < HEADER_SIZE:
        raise ValueError("size too small")
    payload = bytearray(size)
    state = seed + seq * 10007
    for i in range(HEADER_SIZE, size):
        state = (state * 1103515245 + 12345) & 0xFFFFFFFF
        payload[i] = state & 0xFF
    struct.pack_into("<IIIII", payload, 0, MAGIC, seq, batch_total, size, 0)
    crc = batch_crc32(bytes(payload))
    struct.pack_into("<I", payload, 16, crc)
    return bytes(payload)


def send_uso_like(sock: socket.socket, dest, data: bytes, mss: int) -> int:
    segs = 0
    off = 0
    while off < len(data):
        chunk = data[off : off + mss]
        sock.sendto(chunk, dest)
        segs += 1
        off += len(chunk)
    return segs


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--dest", default="127.0.0.1")
    p.add_argument("--port", type=int, default=9000)
    p.add_argument("--size", type=int, default=4000)
    p.add_argument("--count", type=int, default=1)
    p.add_argument("--mss", type=int, default=1408)
    p.add_argument("--seed", type=int, default=42)
    args = p.parse_args()
    dest = (args.dest, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    total_segs = 0
    for seq in range(args.count):
        batch = build_batch(seq, args.count, args.size, args.seed)
        total_segs += send_uso_like(sock, dest, batch, args.mss)
        print(f"batch {seq}: {args.size} bytes")
    print(f"sent {args.count} batch(es), {total_segs} datagram(s)")


if __name__ == "__main__":
    main()
