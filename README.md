# RDMA & High-Performance Networking in C

*Three low-level networking projects in C: a TCP throughput benchmark, an RDMA SEND bandwidth benchmark, and a ring all-reduce collective over RDMA.*

## Overview

This repository collects three self-contained systems that explore networking
performance from the socket layer down to the NIC. Each builds independently,
ships with its own README, and is written to one consistent C style. The TCP
project establishes a throughput baseline over ordinary sockets; the two RDMA
projects use the InfiniBand verbs API directly to push bandwidth and to build a
collective communication primitive of the kind used in distributed training.

## Highlights

- **tcp-throughput** — A client/server TCP throughput benchmark across a
  payload-size sweep. Saturates a 10 GbE link at ~1.36 GB/s and peaks at
  ~5.34 GB/s on loopback.
- **rdma-bandwidth** — A unidirectional RDMA SEND bandwidth benchmark over an RC
  queue pair using InfiniBand verbs. Sustains ~4000 MB/s on the fastest paths,
  both loopback and cross-machine.
- **ring-all-reduce** — A pipelined ring all-reduce collective built on one-sided
  RDMA READs with credit-based flow control. Peaks at ~10.2 Gb/s at a 196 KB
  message size on a 4-node cluster.

## Tech stack

- C (C11 / GNU C11)
- InfiniBand verbs (`libibverbs`) for the RDMA projects
- POSIX TCP sockets
- GNU Make, GCC

## Repository structure

```
rdma-networking/
├── tcp-throughput/   # TCP throughput benchmark (sockets)
├── rdma-bandwidth/   # RDMA SEND bandwidth benchmark (InfiniBand verbs)
├── ring-all-reduce/  # ring all-reduce collective over RDMA
├── LICENSE
└── README.md
```

## Building

Each project has its own Makefile:

```sh
cd tcp-throughput  && make     # pure POSIX sockets, no special hardware
cd rdma-bandwidth  && make     # needs an RDMA NIC + libibverbs
cd ring-all-reduce && make     # needs an RDMA NIC + libibverbs
```

The RDMA projects require an InfiniBand or RoCE NIC with its driver loaded and
the `libibverbs` development package installed. The TCP project needs only a C
toolchain.

## Usage

See each subdirectory's README for full instructions:

- [tcp-throughput](tcp-throughput/README.md)
- [rdma-bandwidth](rdma-bandwidth/README.md)
- [ring-all-reduce](ring-all-reduce/README.md)

## Results

| Project | Metric | Result |
| --- | --- | --- |
| tcp-throughput | 10 GbE saturation | ~1.36 GB/s |
| tcp-throughput | loopback peak | ~5.34 GB/s |
| rdma-bandwidth | sustained plateau | ~4000 MB/s |
| ring-all-reduce | peak all-reduce throughput | ~10.2 Gb/s (196 KB) |

## License

MIT, except the `rdma-bandwidth/` directory, which is derived from the
InfiniBand "pingpong" template and carries that template's dual GPLv2 /
OpenIB.org BSD license. See [LICENSE](LICENSE) for details.
