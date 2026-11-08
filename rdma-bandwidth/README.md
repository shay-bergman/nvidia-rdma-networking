# RDMA SEND Bandwidth Benchmark

*Unidirectional RDMA SEND throughput between two hosts over InfiniBand verbs.*

## Overview

This benchmark measures sustained RDMA SEND throughput over a Reliable
Connection (RC) queue pair using the native `libibverbs` API. The two peers
bootstrap over TCP, exchanging queue-pair metadata (LID, QPN, PSN, GID) and
transitioning their QPs through `INIT -> RTR -> RTS`. The sender then sweeps
payload sizes from 1 B to 1 MiB (powers of two) and reports the rate at each
size. It is derived from the InfiniBand "pingpong" bandwidth template, split
into verbs, connection-exchange, and driver modules.

## Highlights

- Loopback RDMA path sustains ~4000 MB/s on the fastest hosts.
- Cross-machine transfer reaches ~4000 MB/s, matching the loopback ceiling on
  that fabric.
- Clean three-region curve: per-SEND overhead (1-16 B), DMA ramp-up
  (32 B - 16 KiB), and a saturation plateau (>16 KiB).
- Sends under the QP's inline limit are posted inline to avoid an extra DMA
  fetch of the payload.

## Tech stack

- C (GNU C11), InfiniBand verbs (`libibverbs`)
- POSIX TCP sockets for out-of-band bootstrap
- GNU Make, GCC

## Repository structure

```
rdma-bandwidth/
├── pingpong.h      # shared types, constants, cross-module declarations
├── ib_verbs.c      # verbs resources + post/poll data plane
├── tcp_exchange.c  # out-of-band exchange of connection info
├── bw_bench.c      # argument parsing, bring-up, size sweep, main
└── Makefile
```

## Building

Requires an RDMA-capable NIC (InfiniBand or RoCE) with its driver loaded, plus
the `libibverbs` development headers and library.

```sh
make            # builds ./server and a ./client symlink to it
make clean
```

## Usage

Start the server (receiver) on one host:

```sh
./server
```

Start the client (sender) on the other host:

```sh
./client <server-host>
```

Useful options: `-p <port>`, `-d <ib-dev>`, `-i <ib-port>`, `-m <mtu>`,
`-n <iters>`, and `-g <gid-index>` (required for RoCE).

## Results

Every host shows the same curve shape; the plateau is set by each path's
maximum sustainable RDMA SEND rate.

| Path | Sustained plateau |
| --- | --- |
| Loopback, fastest hosts | ~4000 MB/s |
| Loopback, mid host | ~3100 MB/s |
| Loopback, slowest host | ~1560 MB/s |
| Cross-machine, fast pair | ~4000 MB/s |
| Cross-machine, slower pair | ~1720 MB/s |

The three regions (overhead, ramp-up, saturation) appear in all curves, with
small variations from fabric and driver overhead.

## License

The upstream template's dual GPLv2 / OpenIB.org BSD license applies to this
directory and is reproduced in `pingpong.h`. See the repository
[LICENSE](../LICENSE) for details.
