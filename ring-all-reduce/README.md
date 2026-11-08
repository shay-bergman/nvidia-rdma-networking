# Ring All-Reduce over RDMA

*A pipelined ring all-reduce collective built directly on InfiniBand verbs.*

## Overview

This is a from-scratch implementation of the ring all-reduce collective (and
its reduce-scatter and all-gather halves) over RDMA. Processes form a ring and
exchange data with one-sided RDMA READs, coordinated by a credit-based
signalling scheme that uses zero-byte `WRITE_WITH_IMM` packets. A sliding
window keeps several reads in flight so network transfer overlaps reduction.
The collective runs in `P-1` reduce-scatter steps followed by `P-1` all-gather
steps across `P` processes.

## Highlights

- Peak throughput of ~10,222 Mb/s at a 196 KB message size on a 4-node cluster.
- Correctness validated: all nodes converge on the identical summed vector, and
  reduce-scatter / all-gather produce the expected per-chunk results.
- Four distinct performance regions observed — latency-bound, network/cache
  bound (peak), memory-bandwidth bound (dip at 393-786 KB as the working set
  outgrows L3), and amortization (slow climb back to ~9,062 Mb/s at 3 MB).

## Key optimizations

- **Pipelining (32 KB blocks):** chunks are transferred in 32 KB blocks so the
  NIC fetches block N while the CPU reduces block N-1.
- **One-sided RDMA READ:** transfers are consumer-driven; the producer side is
  passive.
- **Credit-based flow control:** a 0-byte `WRITE_WITH_IMM` carries a 32-bit
  token encoding exactly which block is ready, so a consumer never reads data
  that is not yet prepared.
- **Sliding window (depth 8):** eight reads are kept in flight to keep the link
  saturated.
- **Zero-copy all-gather:** all-gather reads land directly in the final buffer,
  skipping the scratch copy that reduce-scatter needs.
- **Single pinned memory region:** all memory is registered once up front,
  avoiding per-operation pin/unpin cost.

## Tech stack

- C (C11), InfiniBand verbs (`libibverbs`), `librt`
- POSIX TCP sockets for the ring bootstrap
- GNU Make, GCC

## Repository structure

```
ring-all-reduce/
├── rdma_lib.h     # public collective API (opaque handle, data types, ops)
├── pg_internal.h  # internal handle, constants, immediate-token codec
├── connection.c   # group setup/teardown, TCP bootstrap, QP state machine
├── memory.c       # single-buffer allocation + memory-region registration
├── transport.c    # RDMA post/poll primitives (read, signal, wait)
├── collective.c   # reduce-scatter, all-gather, all-reduce algorithm
├── test.c         # correctness and throughput test driver
└── Makefile
```

## Building

Requires an RDMA-capable NIC (InfiniBand or RoCE) with its driver loaded, plus
the `libibverbs` development headers and library.

```sh
make            # builds ./test_rdma
make clean
```

## Usage

Launch the same binary on every node in the ring, passing the identical
space-separated host list (in ring order) to each. A process finds its own rank
by matching its hostname in the list.

```sh
./test_rdma "host1 host2 host3 host4"
```

The driver runs a correctness suite on a small vector, then sweeps message
sizes and prints per-size timing and throughput. In `test.c`, the per-node seed
data is keyed off hostname substrings (`node1`..`node4`); edit those to match
the hostnames of your ring.

## Results

Measured on a 4-node cluster, 1000 iterations per size, vectors from 24 B to
3 MB. Representative points:

| Message size | Throughput | Regime |
| --- | --- | --- |
| 24 B | ~23 Mb/s | latency-bound |
| 196 KB | ~10,222 Mb/s | peak (network/cache bound) |
| 393-786 KB | ~8,717 Mb/s | dip (memory-bandwidth bound) |
| 3 MB | ~9,062 Mb/s | amortization (slow climb) |

The peak sits where the pipeline is full and the working set still fits in L3;
the dip appears once the per-step chunk exceeds L3 and the reduction becomes
DRAM-bandwidth bound.

## License

MIT. See the [LICENSE](../LICENSE) at the repository root.
