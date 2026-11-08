# TCP Throughput Benchmark

*A client/server tool that measures sustained TCP throughput across a payload-size sweep.*

## Overview

The client streams fixed-size messages to the server for each payload size in a
geometric sweep (1 B, 2 B, 4 B, ... , 1 MiB) and reports the achieved rate in
MB/s per size. Each size is warmed up before timing, and a one-byte ACK from the
server bounds the timed window so the measurement reflects data fully drained by
the receiver. The design isolates socket plumbing in a shared module so the
client and server hold only benchmark logic.

## Highlights

- Loopback peaks at ~5.34 GB/s at 1 MiB (711 MB/s already at 4 KiB).
- Saturates a 10 GbE link at ~1.36 GB/s (256 KiB payloads).
- Plateaus at ~112 MB/s (896 Mb/s) over a 1 Gb/s path, matching the link limit.
- Small payloads (<16 B) are overhead-bound at ~1-1.5 MB/s across all paths.

## Tech stack

- C (C11), POSIX sockets (`AF_INET`, `SOCK_STREAM`)
- GNU Make, GCC

## Repository structure

```
tcp-throughput/
├── client.c    # sending side: size sweep, timing, throughput output
├── server.c    # receiving side: drain messages, send ACK per size
├── shared.c    # send_all / receive_all / buffer / error helpers
├── shared.h    # shared constants and helper declarations
└── Makefile
```

## Building

```sh
make            # builds ./client and ./server
make clean
```

## Usage

Start the server on the receiving host:

```sh
./server
```

Run the client on the sending host, passing the server's IPv4 address
(use `127.0.0.1` for a loopback test):

```sh
./client <server-ip>
```

The client prints one tab-separated line per size: `size <TAB> throughput <TAB> MB/s`.

## Results

All paths share the same curve: tiny payloads are dominated by per-message
syscall and TCP overhead, throughput climbs steeply with size, and each path
plateaus at its link bandwidth.

| Path | Small payloads (<16 B) | Large-payload plateau |
| --- | --- | --- |
| Loopback (same host) | ~1.5 MB/s | ~5.34 GB/s at 1 MiB |
| 10 GbE LAN | ~1 MB/s | ~1.36 GB/s at 256 KiB |
| 1 Gb/s backbone | tens of MB/s | ~112 MB/s / 896 Mb/s |

Throughput is latency/overhead-bound for small messages and bandwidth-bound for
large ones, with the crossover and ceiling set by the underlying link.

## License

MIT. See the [LICENSE](../LICENSE) at the repository root.
