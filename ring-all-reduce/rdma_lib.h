/*
 * rdma_lib.h - Public API for the Ring All-Reduce collective over RDMA.
 *
 * A process group forms a ring of peers. Collectives (reduce-scatter,
 * all-gather, all-reduce) are implemented with one-sided RDMA READs and a
 * credit-based signalling scheme over InfiniBand verbs.
 *
 * Author: Shay Bergman
 */
#ifndef RDMA_LIB_H
#define RDMA_LIB_H

/* Opaque handle; implementation details live in pg_internal.h. */
typedef struct pg_handle pg_handle;

/* Supported element types for the collective. */
typedef enum {
    DATATYPE_INT    = 0,
    DATATYPE_DOUBLE = 1
} DATATYPE;

/* Supported reduction operations. */
typedef enum {
    OP_SUM  = 0,
    OP_MULT = 1
} OPERATION;

/*
 * Connect this process into the ring of peers.
 *
 * Initializes RDMA resources, performs a TCP bootstrap to connect to the left
 * and right ring neighbors, and prepares the handle for collective calls.
 *
 * server_str: space-separated "host[:port]" identifiers for every process in
 *             the group, in ring order (e.g. "host1:port1 host2:port2").
 * out_pg:     set to the allocated process-group handle on success.
 * Returns 0 on success, -1 on failure.
 */
int connect_process_group(const char *server_str, void **out_pg);

/*
 * Reduce-scatter: every process contributes a vector in sendbuf; the vectors
 * are reduced element-wise and the result is scattered in chunks, so process i
 * receives chunk (i+1) % P of the reduced vector.
 *
 * count must be divisible by the number of processes.
 * Returns 0 on success, -1 on failure.
 */
int pg_reduce_scatter(void *sendbuf, void *recvbuf, int count,
                      DATATYPE datatype, OPERATION op, void *pg);

/*
 * All-gather: every process starts with one chunk; after the call every
 * process holds the complete, reconstructed vector. `op` is ignored.
 * Returns 0 on success, -1 on failure.
 */
int pg_all_gather(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op, void *pg);

/*
 * All-reduce: logically a reduce-scatter followed by an all-gather. Every
 * process starts with a vector and ends with a copy of the fully reduced one.
 * Returns 0 on success, -1 on failure.
 */
int pg_all_reduce(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op, void *pg);

/*
 * Destroy the process-group handle and release all RDMA resources.
 * Safe to call on a partially initialized handle. Returns 0.
 */
int pg_close(void *pg);

#endif /* RDMA_LIB_H */
