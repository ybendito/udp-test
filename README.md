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
- **Git** (to fetch vendored dependencies)

## Dependencies

The project vendors **Boost.Asio** (header-only subset via Boost superproject submodules). A standalone **Asio** tree is also fetched for reference; the build uses **Boost’s** `libs/asio`.

| Dependency | Repository | Commit (verified) |
|------------|------------|-------------------|
| Boost superproject | https://github.com/boostorg/boost | `8c3ca159ca9e5ac4b56ced6a6f146d5fef3650bc` |
| Standalone Asio (optional) | https://github.com/chriskohlhoff/asio | `bd500f0a018db9a845ebaaed5c0318343ae9f497` |

After checking out the Boost commit above, initialize only the submodules required by this repo (see script below). Boost’s bundled `libs/asio` at that revision is commit `7b56b644c9819be94b7b601823a65087a7c29f2e`.

### Fetch dependencies (recommended)

From the repo root in PowerShell:

```powershell
.\scripts\fetch-deps.ps1
```

The script clones into `deps/`, checks out the pinned commits, and runs `git submodule update --init` for the Boost libraries listed in `cmake/BoostDeps.cmake`.

If `deps/` already exists, the script skips cloning; delete `deps/boost` or `deps/asio` to re-fetch.

### Fetch dependencies (manual, pinned)

```powershell
mkdir deps
cd deps

git clone https://github.com/boostorg/boost.git boost
cd boost
git checkout 8c3ca159ca9e5ac4b56ced6a6f146d5fef3650bc
git submodule update --init `
  libs/align libs/asio libs/assert libs/config libs/core `
  libs/integer libs/io libs/mpl libs/optional libs/predef `
  libs/preprocessor libs/smart_ptr libs/static_assert libs/system `
  libs/throw_exception libs/type_traits libs/utility libs/winapi
cd ..

git clone https://github.com/chriskohlhoff/asio.git asio
cd asio
git checkout bd500f0a018db9a845ebaaed5c0318343ae9f497
cd ..\..
```

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

Optional receive coalescing (compare `frags=` vs default `recvfrom`):

```text
build\Release\boostudp_server.exe --port 5000 --count 10 -v --uro
```

Without `--count`, the server exits after an idle timeout once traffic stops.

Verbose output:

- **Single datagram per batch** (`--mss 0` on sender): `ok seq=N 33792b`
- **Reassembled from multiple UDP segments**: `ok seq=N 33792b mss=1408 segs=24`

Summary line: `batches=… frags=… bad=… lost=… dup=… ooo=…`

### 2. Send from the PC (or guest if built there)

**USO segmented** (many UDP payloads, one logical batch):

```text
build\Release\boostudp_client.exe --dest 10.0.0.15 --port 5000 --size 33792 --mss 1408 --count 10
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
| `-v` | Verbose per-batch / reassembly logs |
| `--uro` | `WSARecvMsg` + `UDP_RECV_MAX_COALESCED_SIZE` (64 KiB) |

**Client** (`--help`):

| Option | Description |
|--------|-------------|
| `--source <addr>` | Local bind (default `0.0.0.0`) |
| `--dest <addr>` | Destination host (required) |
| `--port <port>` | Destination port (required) |
| `--size <bytes>` | Logical batch size, 16..65536 (required; includes 16-byte BDUP header) |
| `--count <n>` | Batches to send (default `10`) |
| `--mss <bytes>` | USO segment size (default `1400`; `0` = plain `sendto`, no USO) |

### Wireshark

Example filter between guest and PC:

```text
ip.addr == 10.0.0.7 && ip.addr == 10.0.0.15 && udp
```

With `--mss 1408` and `--size 33792`, expect many ~1408-byte UDP frames. With `--mss 0`, expect one UDP payload per batch (possibly multiple IP fragments).

## Protocol sketch

Each batch is one buffer with a fixed header (`magic`, `seq`, `length`, `crc32`) and random payload. `length` equals `--size`. The sender CRCs the full buffer; the receiver reassembles fragments until `length` bytes arrive, then validates.

## Limits

| Constant | Value | Meaning |
|----------|-------|---------|
| Max batch / buffer | 64 KiB | `--size`, recv buffer, USO/URO caps |
| Default MSS | 1400 | USO segment hint |
| Max MSS | 1472 | Per-segment UDP payload cap |
| Socket SNDBUF/RCVBUF | 1 MiB | Set in USO socket setup |

## Related references

- Sunshine `send_batch()` — `src/platform/windows/misc.cpp` (USO send pattern)
- Windows USO / coalescing: `UDP_SEND_MSG_SIZE`, `UDP_RECV_MAX_COALESCED_SIZE`, `WSASendMsg` / `WSARecvMsg`
