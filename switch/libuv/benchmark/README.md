# libuv TCP echo benchmark server (Nintendo Switch)

A small libuv TCP echo server used to validate and benchmark the `switch-libuv`
port on real hardware. It listens on `0.0.0.0:1234`, echoes everything it
receives, and prints live metrics (active/peak connections, total accepted,
accept errors, bytes echoed, per-second throughput) to the console and to
`sdmc:/uvbench.log` (one line per second, flushed each write so it can be read
over FTP while running). Press **+** to stop.

It exercises the full libuv TCP server path on the posix-poll backend:
`uv_listen` → `on_new_connection` → `uv_accept` → `uv_read_start` → `uv_write`,
under concurrent load.

## Build

Requires the `switch-libuv` package (and `switch-pkg-config`) installed:

```sh
dkp-pacman -S switch-libuv switch-pkg-config
make
```

The Makefile pulls all libuv flags from `aarch64-none-elf-pkg-config --cflags
--libs libuv`, which encodes the include path, the Horizon shim dir, the
force-included prelude, the required defines, and the `libuv-horizon-port.o`
support object. Copy `uvbench.nro` to `sdmc:/switch/` and launch it from hbmenu.

## Socket configuration

The server installs a custom `SocketInitConfig` (1 MB / 4 MB TCP buffers,
`sb_efficiency = 8`, `bsd_service_type = BsdServiceType_Auto`) matching nx.js's
known-good settings. Overly small buffers break accept/poll readiness on the
firmware `bsd:` service, so don't shrink these.

## Benchmarking from a PC

Use [`tcpkali`](https://github.com/satori-com/tcpkali) (`brew install tcpkali`):

```sh
# Throughput (echo, both directions), 20 connections, 15s, 4 KB messages:
tcpkali -c 20 --connect-rate 50 -T 15s \
    -m "$(printf 'C%.0s' $(seq 1 4096))" --latency-connect <switch-ip>:1234
```

A simple echo check with netcat:

```sh
printf 'hello\n' | nc -G 5 <switch-ip> 1234   # prints "hello" back
```

## Measured results (Switch FW 18.1.0, LAN)

| Metric | Value |
|---|---|
| Peak echo throughput (server-side) | ~28 MB/s each direction (~230 Mbps RX) |
| Aggregate bandwidth (tcpkali) | up to 201 Mbps ↑ / 85 Mbps ↓ |
| Max concurrent connections | ~25 (firmware `bsd:` session-pool limit) |
| Connections handled | 102, zero accept errors, byte-symmetric echo |

The ~25 concurrent-connection ceiling is a firmware limit, not a libuv one:
connections beyond it don't complete their handshake (no accept error is
raised). `werr` (write errors) under saturation are normal backpressure — when
the client can't drain echoes fast enough the send buffer fills and `uv_write`
fails; a production server would apply write backpressure on `EAGAIN`.

This benchmark is what surfaced the `uv__cloexec` bug fixed in the port: libnx's
`fcntl(F_SETFD)` returns `EOPNOTSUPP` as a positive value, which stock libuv
misread as failure and closed every accepted socket after the first. See
`../BUILD-NOTES.md`.
