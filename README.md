# boostudp

Small Windows-focused UDP harness for testing **USO** (UDP send segmentation) and optional **URO**-style receive coalescing (`UDP_RECV_MAX_COALESCED_SIZE` + `WSARecvMsg`). Useful when comparing guest vNIC / Apollo behavior to a controlled sender/receiver on the PC.

Binaries:

| Target | Role |
|--------|------|
| `boostudp_client` | Sender (USO via `WSASendMsg`, or plain `sendto` with `--mss 0`) |
| `boostudp_server` | Receiver (app-level batch reassembly + CRC; optional `--uro`) |

## Requirements

- **Windows 10 2004+** (build 19041+) for USO / software receive coalescing APIs
- **Visual Studio 2019** (or newer) with C++ desktop workload and **CMake** (3.16+)
- **Git** (to fetch vendored Boost)

USO behavior varies by OS build, NIC driver, and installed filters. The client prints `windows build N` at startup. If `WSASendMsg` fails, the client reports the error and exits — there is no silent fallback to manual multi-datagram send.

Reported in testing:

| Build | Behavior |
|-------|----------|
| **26080** | `WSASendMsg` USO works; multi-homed OK with default `--source` (egress + `IP_PKTINFO` ifindex toward `--dest`). |
| **26100 / Server 2025** | `WSASendMsg` USO works; **hardware USO** observed in recent testing (driver/filters may still matter). Default `IP_PKTINFO` path OK. |

If routing is ambiguous, set `--source` to the IP on the subnet that reaches `--dest`.

## Dependencies

Vendored **Boost.Asio** (header-only subset via Boost superproject submodules).

| Dependency | Repository | Commit (verified) |
|------------|------------|-------------------|
| Boost superproject | https://github.com/boostorg/boost | `8c3ca159ca9e5ac4b56ced6a6f146d5fef3650bc` |

After checking out the Boost commit above, initialize only the submodules required by this repo (see script below).

### Fetch dependencies

From the repo root in PowerShell:

```powershell
.\scripts\fetch-deps.ps1
```

The script clones into `deps/` and runs `git submodule update --init` for the Boost libraries listed in `cmake/BoostDeps.cmake`.

If `deps/boost` already exists, the script skips cloning; delete it to re-fetch.

## Build

### Windows (Visual Studio 2019)

Adjust the `VsDevCmd.bat` path in `scripts/build-vs2019.bat` if your VS install differs, then:

```bat
scripts\build-vs2019.bat
```

Outputs:

```text
build\Release\boostudp_client.exe
build\Release\boostudp_server.exe
```

Release builds link the **static MSVC runtime** (`/MT`).

### Manual CMake

```bat
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## Run

Replace addresses/ports for your lab (example: guest `10.0.0.7`, PC `10.0.0.15`, port `5000`).

### 1. Start the receiver (PC)

```text
build\Release\boostudp_server.exe --port 5000 --count 10 -v
```

Optional receive coalescing:

```text
build\Release\boostudp_server.exe --port 5000 --count 10 -v --uro
```

Without `--count`, the server exits after an idle timeout once traffic stops. Use **`--loop`** to start another idle session after each summary (not combinable with `--count`). With `--loop`, stdout is flushed after each session so `> outfile` shows progress as rounds complete.

```text
build\Release\boostudp_server.exe --port 5000 -v --loop
build\Release\boostudp_server.exe --port 5000 -v --loop 5
```

Verbose output:

- **Single datagram per batch** (`--mss 0` on sender): `ok seq=N 33792b`
- **Reassembled from multiple UDP segments**: `ok seq=N 33792b mss=1408 segs=24` (`mss` matches sender `--mss`, not the first post-header slice)

Summary line: `batches=… frags=… bad=… lost=… dup=… ooo=…` (`bad` = failed or incomplete logical batches, not resync bytes skipped)

### 2. Send from the PC (or guest if built there)

**USO segmented** (many UDP payloads, one logical batch):

```text
build\Release\boostudp_client.exe --dest 10.0.0.15 --port 5000 --size 33792 --mss 1408 --count 10
```

**Guest vNIC / hardware USO** (trace large send NBL): bind to the NIC that routes to `--dest`.

```text
build\Release\boostudp_client.exe --source 10.0.0.7 --dest 10.0.0.15 --port 5000 --size 5120 --mss 1408 --count 10
build\Release\boostudp_client.exe --dest 10.0.0.15 --port 5000 --size 5120 --mss 1408 --count 10
```

**Plain UDP** (one datagram per batch; IP may fragment on the wire):

```text
build\Release\boostudp_client.exe --dest 10.0.0.15 --port 5000 --size 33792 --mss 0 --count 10
```

### CLI reference

**Server** (`--help`):

| Option | Description |
|--------|-------------|
| `--bind <addr>` | Bind address (default `0.0.0.0`) |
| `--port <port>` | Listen port (required) |
| `--count <n>` | Exit after `n` valid batches |
| `--loop [<n>]` | Repeat idle sessions (requires no `--count`); `n` optional (default: forever) |
| `-v` | Verbose per-batch / reassembly logs |
| `--uro` | `WSARecvMsg` + `UDP_RECV_MAX_COALESCED_SIZE` (64 KiB) |

**Client** (`--help`):

| Option | Description |
|--------|-------------|
| `--source <addr>` | Local bind (default `0.0.0.0` = auto egress toward `--dest`) |
| `--dest <addr>` | Destination host (required) |
| `--port <port>` | Destination port (required) |
| `--size <bytes>` | Logical batch size, 16..65536 (required; includes 16-byte BDUP header) |
| `--count <n>` | Batches to send (default `10`) |
| `--mss <bytes>` | USO segment size (default `1400`; `0` = plain `sendto`, no USO) |
| `--no-pktinfo` | `WSASendMsg` without `IP_PKTINFO` (bind-only; logged once at startup) |
| `--completion-routine` | `WSASendMsg` with completion routine + alertable `SleepEx` |

### Wireshark

Example filter between guest and PC:

```text
ip.addr == 10.0.0.7 && ip.addr == 10.0.0.15 && udp
```

With `--mss 1408` and `--size 33792`, expect many ~1408-byte UDP frames. With `--mss 0`, expect one UDP payload per batch (possibly multiple IP fragments).

## Protocol sketch

Each batch is one buffer with a fixed **20-byte** header and random payload. `length` equals `--size` (includes the header). The sender CRCs the full buffer; the receiver reassembles fragments until `length` bytes arrive, then validates.

```text
struct PacketHeader {   // packed, little-endian on x86
    uint32_t magic;       // 0x42554450 ("BDUP")
    uint32_t seq;         // 0 .. batch_total-1
    uint32_t batch_total; // sender --count (same on every batch in a run)
    uint32_t length;      // total bytes == --size
    uint32_t crc32;       // CRC over full batch with crc32=0
};
```

`batch_total` helps the receiver resync after a bad segment: rescan only accepts headers where `seq < batch_total`, and `lost` can use `batch_total` from the wire when the server has no `--count`.

## Limits

| Constant | Value | Meaning |
|----------|-------|---------|
| Max batch / buffer | 64 KiB | `--size`, recv buffer, USO/URO caps |
| Default MSS | 1400 | USO segment hint |
| Max MSS | 1472 | Per-segment UDP payload cap |
| Server SO_RCVBUF | 1 MiB | Always after bind (USO/URO burst); `--uro` adds coalesced recv |

## Related references

- Sunshine `send_batch()` — `src/platform/windows/misc.cpp` (USO send pattern)
- Windows: `UDP_SEND_MSG_SIZE`, `UDP_RECV_MAX_COALESCED_SIZE`, `WSASendMsg` / `WSARecvMsg`
