# Linux server build

Build **only** `boostudp_server` here. Sources and headers stay in the repo root; this tree holds scripts and the out-of-tree build directory.

**Client** (USO send) remains Windows-only.

## Requirements

- GCC or Clang with C++17
- CMake 3.16+
- Git

Fedora example: `sudo dnf install cmake gcc-c++ git`

## Build (no install)

From the repo root:

```bash
linux/scripts/fetch-deps.sh   # once: vendored Boost into deps/
linux/scripts/build.sh
```

Binary:

```text
linux/build/boostudp_server
```

Run in place:

```bash
linux/build/boostudp_server --port 9000 -v
```

`--uro` is not supported on Linux (Windows `WSARecvMsg` coalescing only). Receive uses `recvfrom` per datagram with the same batch reassembly logic as the default Windows server.

**Exit behaviour**

| Server flags | When it exits |
|--------------|----------------|
| (none) | After **3 s** with no UDP traffic *after the first datagram* (one session), then exits |
| `--count N` | After **N valid batches**; blocks until the first datagram, then **3 s** idle between batches (set **server** `--count` to match client `--count`) |
| `--loop` | Never exits; starts a new idle session after each 3 s quiet period |

Client `--count` sets `batch_total` on the wire. With **no server `--count`** (typical): wait forever for the **first** datagram, then **3 s** with no further UDP after the last datagram (`-v` logs `idle timeout (3s), ending session`), then print `batches=…` and exit. Do not use `--loop` if you want the process to stop. Server `--count` is optional and only needed if you want a fixed receive cap independent of the wire.

## Troubleshooting “nothing received”

1. **Destination IP** — Point the Windows client at an address that belongs to this host, not an old Windows server IP. Check with `hostname -I` or `ip -br addr` (e.g. `10.0.0.3` on `enp0s31f6`, `10.0.0.6` on `br0` for VM/LAN traffic).

   ```text
   boostudp_client.exe --dest <f2-ip> --port 9000 --size 34000 --count 5 --mss 1408
   ```

2. **Server listening** — In another shell:

   ```bash
   ss -ulnp | grep 9000
   ```

   Expect `0.0.0.0:9000` and `boostudp_server`.

3. **First line is immediate; traffic is blocking** — With `-v` you should see `listen 0.0.0.0:9000` right away. Until the first UDP datagram arrives, the server waits (no timeout yet). Wrong `--dest` looks like “hangs with no batches”.

4. **Capture on Linux** (while sending from Windows):

   ```bash
   sudo tcpdump -ni any udp port 9000
   ```

   No packets → routing/firewall/wrong IP. Packets but no `ok` lines → port/process or reassembly (use full client batches, not random UDP).

5. **Line-buffered logs** when redirecting: `stdbuf -oL linux/build/boostudp_server ... > server.log 2>&1`

6. **Client `--count` > 1** — Idle timeout is 3 s after the *last* datagram in a burst (timer resets on every recv). While reassembling, recv blocks with no timeout. If an older server ended the session after batch 2 of 3, rebuild and restart.
