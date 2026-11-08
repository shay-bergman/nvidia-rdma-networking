/*
 * pg_internal.h - Internal definitions shared across the Ring All-Reduce
 *                 modules (connection, memory, transport, collective).
 *                 Not part of the public API.
 */
#ifndef PG_INTERNAL_H
#define PG_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <arpa/inet.h> /* htonl / ntohl */
#include <infiniband/verbs.h>

#include "rdma_lib.h"

/* Size of a single pipeline block (the unit of work for the pipeline). */
#define BLOCK_BYTES (32 * 1024)

/* Receive Work Requests to pre-post; must be >= PIPE_DEPTH. */
#define MAX_RECV_WR 1024

/* Sliding-window depth: RDMA READs kept in flight to overlap I/O and compute. */
#define PIPE_DEPTH 8

/* Scratch buffer size; must be at least PIPE_DEPTH * BLOCK_BYTES. */
#define SCRATCH_BYTES (PIPE_DEPTH * BLOCK_BYTES)

/* Work Request ID namespaces (upper bits tag the operation kind). */
#define READ_ID(b) (0xBAAD0000 | (uint64_t)(b)) /* RDMA READ      */
#define SIGN_ID(b) (0xC19B0000 | (uint64_t)(b)) /* WRITE_WITH_IMM */

/* Collective phase, carried in the immediate-data token. */
typedef enum { PHASE_RS = 0, PHASE_AG = 1 } phase_t;

/*
 * Connection "business card" exchanged over TCP: everything a peer needs to
 * open an RDMA connection to us and to RDMA-READ our buffer.
 */
typedef struct {
    uint16_t lid;    /* local port LID                        */
    uint32_t qp_num; /* queue-pair number                     */
    uint32_t psn;    /* starting packet sequence number       */
    uint64_t addr;   /* base address of our registered buffer */
    uint32_t rkey;   /* remote key for our memory region      */
} qp_conn_info;

/*
 * Per-process handle. The single pinned buffer `buf` is split into:
 *   - data_base:    main data (reduce destination; all-gather READ target)
 *   - scratch_base: landing pad for incoming READs during reduce-scatter
 */
struct pg_handle {
    /* Core verbs resources. */
    struct ibv_context *context;
    struct ibv_pd      *pd;
    struct ibv_cq      *send_cq;  /* all outbound completions (READ, WRITE_IMM) */
    struct ibv_cq      *recv_cq;  /* all inbound signals (RECV)                 */
    struct ibv_qp      *qp_left;  /* read data FROM the left neighbor           */
    struct ibv_qp      *qp_right; /* signal TO the right neighbor               */
    struct ibv_mr      *mr;       /* the single pinned memory region            */
    void               *buf;      /* backing allocation for the MR              */

    /* Views into `buf`. */
    uint8_t *data_base;
    uint8_t *scratch_base;
    size_t   data_bytes;

    /* Ring geometry. */
    int steps;  /* P - 1 */
    int chunks; /* P     */

    /* Remote neighbor connection info. */
    qp_conn_info left_conn_info;
    qp_conn_info right_conn_info;

    /* Rank within the ring. */
    int rank;
    int num_servers;
};

/* --- Immediate-data token codec (shared, header-inline) --- */

/* Pack pipeline state into a 32-bit immediate token. */
static inline uint32_t encode_imm(phase_t ph, int step, int chunk, int block)
{
    /* [1 bit phase][15 bits step][8 bits chunk][8 bits block] */
    return ((uint32_t)ph << 31) | ((uint32_t)step << 16) |
           ((uint32_t)chunk << 8) | (uint32_t)block;
}

/* Unpack a 32-bit immediate token back into pipeline state. */
static inline void decode_imm(uint32_t imm, phase_t *ph, int *step,
                              int *chunk, int *block)
{
    *ph    = (imm >> 31) & 1;
    *step  = (imm >> 16) & 0x7FFF;
    *chunk = (imm >> 8) & 0xFF;
    *block = imm & 0xFF;
}

/* Base address of the (left) neighbor's remote buffer. */
static inline uint64_t remote_data_base(const pg_handle *h)
{
    return h->left_conn_info.addr;
}

/* --- memory.c: buffer allocation and MR registration --- */
int  pg_alloc_and_register(pg_handle *h);
void pg_release_memory(pg_handle *h);

/* --- transport.c: RDMA data-plane primitives --- */
int post_recvs(pg_handle *h, int count);
int async_post_read(pg_handle *h, uint64_t remote_addr, void *local_addr,
                    size_t len, uint64_t wr_id);
int async_post_signal(pg_handle *h, phase_t ph, int step, int chunk,
                      int block, uint64_t wr_id);
int wait_for_block_ready(pg_handle *h, phase_t exp_ph, int exp_step,
                         int exp_chunk, int exp_block);
int wait_for_send_completion(pg_handle *h, uint64_t wr_id);

#endif /* PG_INTERNAL_H */
