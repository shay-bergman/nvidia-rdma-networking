/*
 * collective.c - The ring collective algorithm: reduce-scatter, all-gather,
 *                and all-reduce.
 *
 * Each collective runs in P-1 steps over a ring of P processes. A sliding
 * window keeps PIPE_DEPTH RDMA READs in flight so that network transfer of one
 * block overlaps the reduction of another. Credits (WRITE_WITH_IMM) carry the
 * "block ready" signal around the ring so a consumer only READs data its
 * producer has already prepared.
 */
#include <stdio.h>
#include <string.h>

#include "pg_internal.h"

/* Bytes per element for a given datatype. */
static size_t element_size(DATATYPE datatype)
{
    return (datatype == DATATYPE_INT) ? sizeof(int) : sizeof(double);
}

/* Reduce `len` bytes of src into dst element-wise (dst = dst op src). */
static void reduce_block(void *dst_bytes, const void *src_bytes, size_t len,
                         DATATYPE datatype, OPERATION op)
{
    if (datatype == DATATYPE_INT) {
        int *dst = (int *)dst_bytes;
        const int *src = (const int *)src_bytes;
        for (size_t i = 0; i < len / sizeof(int); i++) {
            dst[i] = (op == OP_SUM) ? (dst[i] + src[i]) : (dst[i] * src[i]);
        }
    } else {
        double *dst = (double *)dst_bytes;
        const double *src = (const double *)src_bytes;
        for (size_t i = 0; i < len / sizeof(double); i++) {
            dst[i] = (op == OP_SUM) ? (dst[i] + src[i]) : (dst[i] * src[i]);
        }
    }
}

/*
 * Pipelined ring reduce-scatter (Phase A of all-reduce). Over P-1 steps, each
 * rank reads one block at a time from its left neighbor into scratch, reduces
 * it into the matching chunk of data_base, and signals the right neighbor that
 * the block is ready. After the final step this rank owns the fully reduced
 * chunk (rank+1) % P. Returns 0 on success, -1 on failure.
 */
int pg_reduce_scatter(void *sendbuf, void *recvbuf, int count,
                      DATATYPE datatype, OPERATION op, void *pg)
{
    pg_handle *h = (pg_handle *)pg;
    if (!h || count == 0) {
        return 0;
    }

    const int P = h->num_servers, rank = h->rank;
    const size_t esz = element_size(datatype);

    /* Single-process group: just copy the input through. */
    if (P == 1) {
        if (sendbuf != recvbuf) {
            memcpy(recvbuf, sendbuf, (size_t)count * esz);
        }
        return 0;
    }

    if (count % P != 0) {
        fprintf(stderr, "[Rank %d] count (%d) must be divisible by P (%d)\n",
                rank, count, P);
        return -1;
    }

    const size_t total_bytes = (size_t)count * esz;
    const size_t chunk_bytes = ((size_t)count / P) * esz;
    if (h->data_bytes < total_bytes) {
        fprintf(stderr, "[Rank %d] Buffer too small for %zu bytes\n",
                rank, total_bytes);
        return -1;
    }

    /* Stage the user's data into the registered buffer if needed. */
    if (sendbuf != h->data_base) {
        memcpy(h->data_base, sendbuf, total_bytes);
    }

    const uint64_t left_base = remote_data_base(h);
    const size_t blocks = (chunk_bytes + BLOCK_BYTES - 1) / BLOCK_BYTES;

    /* Step 0: tell the right neighbor our own starting chunk is ready. */
    for (size_t b = 0; b < blocks; ++b) {
        if (async_post_signal(h, PHASE_RS, 0, rank, (int)b, SIGN_ID(b)) != 0) {
            return -1;
        }
    }

    for (int s = 0; s < P - 1; ++s) {
        const int recv_idx = ((rank - s - 1) % P + P) % P;
        const uint64_t remote_chunk = left_base + (uint64_t)recv_idx * chunk_bytes;
        uint8_t *dst_bytes = h->data_base + (size_t)recv_idx * chunk_bytes;

        size_t K = PIPE_DEPTH < blocks ? PIPE_DEPTH : blocks;

        /* Prime the window: wait for and post the first K reads. */
        for (size_t i = 0; i < K; ++i) {
            if (wait_for_block_ready(h, PHASE_RS, s, recv_idx, (int)i) != 0) {
                return -1;
            }
            size_t off_i = i * BLOCK_BYTES;
            size_t len_i = (off_i + BLOCK_BYTES <= chunk_bytes)
                               ? BLOCK_BYTES : (chunk_bytes - off_i);
            void *dst_i = h->scratch_base + ((int)i % PIPE_DEPTH) * BLOCK_BYTES;
            if (async_post_read(h, remote_chunk + off_i, dst_i, len_i, READ_ID(i)) != 0) {
                return -1;
            }
        }

        /* Drain in order, reduce each block, signal, and refill the window. */
        size_t next_to_post = K;
        for (size_t b = 0; b < blocks; ++b) {
            const size_t off = b * BLOCK_BYTES;
            const size_t len = (off + BLOCK_BYTES <= chunk_bytes)
                                   ? BLOCK_BYTES : (chunk_bytes - off);

            if (wait_for_send_completion(h, READ_ID(b)) != 0) {
                return -1;
            }

            int slot = (int)(b % PIPE_DEPTH);
            void *cur_src = h->scratch_base + slot * BLOCK_BYTES;
            reduce_block(dst_bytes + off, cur_src, len, datatype, op);

            if (async_post_signal(h, PHASE_RS, s + 1, recv_idx, (int)b, SIGN_ID(b)) != 0) {
                return -1;
            }

            if (next_to_post < blocks) {
                if (wait_for_block_ready(h, PHASE_RS, s, recv_idx, (int)next_to_post) != 0) {
                    return -1;
                }
                size_t off_n = next_to_post * BLOCK_BYTES;
                size_t len_n = (off_n + BLOCK_BYTES <= chunk_bytes)
                                   ? BLOCK_BYTES : (chunk_bytes - off_n);
                void *dst_n = h->scratch_base + ((int)next_to_post % PIPE_DEPTH) * BLOCK_BYTES;
                if (async_post_read(h, remote_chunk + off_n, dst_n, len_n,
                                    READ_ID(next_to_post)) != 0) {
                    return -1;
                }
                ++next_to_post;
            }
        }
    }

    /* Hand off: signal that our final chunk is ready for the all-gather. */
    const int my_piece_idx = (rank + 1) % P;
    for (size_t b = 0; b < blocks; ++b) {
        if (async_post_signal(h, PHASE_AG, 0, my_piece_idx, (int)b, SIGN_ID(b)) != 0) {
            return -1;
        }
    }

    /* Optionally hand the caller just this rank's reduced chunk. */
    if (recvbuf) {
        memcpy(recvbuf, h->data_base + (size_t)my_piece_idx * chunk_bytes, chunk_bytes);
    }
    return 0;
}

/*
 * Pipelined ring all-gather (Phase B of all-reduce). Over P-1 steps, each rank
 * reads the next chunk from its left neighbor directly into data_base (no
 * reduction, so this is zero-copy) and echoes the "ready" signal onward. After
 * the final step data_base holds the complete vector. Returns 0 / -1.
 */
int pg_all_gather(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op, void *pg)
{
    (void)sendbuf; /* input already lives in data_base from reduce-scatter */
    (void)op;      /* no reduction in all-gather */

    pg_handle *h = (pg_handle *)pg;
    if (!h || count == 0) {
        return 0;
    }

    const int P = h->num_servers, rank = h->rank;
    const size_t esz = element_size(datatype);
    const size_t total_bytes = (size_t)count * esz;

    if (P == 1) {
        if (recvbuf && h->data_base && recvbuf != h->data_base) {
            memcpy(recvbuf, h->data_base, total_bytes);
        }
        return 0;
    }

    if (count % P != 0) {
        fprintf(stderr, "[Rank %d] count (%d) must be divisible by P (%d)\n",
                rank, count, P);
        return -1;
    }

    const size_t chunk_bytes = ((size_t)count / P) * esz;
    const size_t blocks = (chunk_bytes + BLOCK_BYTES - 1) / BLOCK_BYTES;
    const uint64_t left_base = remote_data_base(h);

    /* We start holding the chunk reduce-scatter left us: (rank+1) % P. */
    int current_chunk_to_pass = (rank + 1) % P;

    for (int s = 0; s < P - 1; ++s) {
        const int chunk_to_get = (current_chunk_to_pass - 1 + P) % P;
        const uint64_t remote_chunk_addr = left_base + (uint64_t)chunk_to_get * chunk_bytes;
        uint8_t *dst_bytes = h->data_base + (size_t)chunk_to_get * chunk_bytes;

        size_t K = PIPE_DEPTH < blocks ? PIPE_DEPTH : blocks;

        /* Prime the window: read directly into data_base (zero-copy). */
        for (size_t i = 0; i < K; ++i) {
            if (wait_for_block_ready(h, PHASE_AG, s, chunk_to_get, (int)i) != 0) {
                return -1;
            }
            size_t off_i = i * BLOCK_BYTES;
            size_t len_i = (off_i + BLOCK_BYTES <= chunk_bytes)
                               ? BLOCK_BYTES : (chunk_bytes - off_i);
            if (async_post_read(h, remote_chunk_addr + off_i, dst_bytes + off_i,
                                len_i, READ_ID(i)) != 0) {
                return -1;
            }
        }

        size_t next_to_post = K;
        for (size_t b = 0; b < blocks; ++b) {
            if (wait_for_send_completion(h, READ_ID(b)) != 0) {
                return -1;
            }

            /* No compute step; just echo the credit to the right neighbor. */
            if (async_post_signal(h, PHASE_AG, s + 1, chunk_to_get, (int)b, SIGN_ID(b)) != 0) {
                return -1;
            }

            if (next_to_post < blocks) {
                if (wait_for_block_ready(h, PHASE_AG, s, chunk_to_get, (int)next_to_post) != 0) {
                    return -1;
                }
                size_t off_n = next_to_post * BLOCK_BYTES;
                size_t len_n = (off_n + BLOCK_BYTES <= chunk_bytes)
                                   ? BLOCK_BYTES : (chunk_bytes - off_n);
                if (async_post_read(h, remote_chunk_addr + off_n, dst_bytes + off_n,
                                    len_n, READ_ID(next_to_post)) != 0) {
                    return -1;
                }
                ++next_to_post;
            }
        }

        current_chunk_to_pass = chunk_to_get;
    }

    if (recvbuf && recvbuf != h->data_base) {
        memcpy(recvbuf, h->data_base, total_bytes);
    }
    return 0;
}

/*
 * Full all-reduce: a reduce-scatter (leaving the reduced result in data_base,
 * so recvbuf is NULL there) followed by an all-gather that assembles the
 * complete reduced vector into recvbuf. Returns 0 / -1.
 */
int pg_all_reduce(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op, void *pg)
{
    int rc = pg_reduce_scatter(sendbuf, NULL, count, datatype, op, pg);
    if (rc != 0) {
        return rc;
    }
    return pg_all_gather(NULL, recvbuf, count, datatype, op, pg);
}
