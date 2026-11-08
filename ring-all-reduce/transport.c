/*
 * transport.c - RDMA data-plane primitives for the ring pipeline.
 *
 * Thin wrappers over the raw verbs post/poll calls used by the collective:
 *   - post_recvs:               arm the receive queue for inbound signals
 *   - async_post_read:          pull a block from the left neighbor (READ)
 *   - async_post_signal:        send a credit to the right neighbor (WRITE+IMM)
 *   - wait_for_block_ready:     block until the expected credit arrives
 *   - wait_for_send_completion: block until a specific send WR completes
 *
 * The posts are asynchronous: they enqueue work and return immediately, which
 * is what lets the pipeline keep several operations in flight at once.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "pg_internal.h"

/*
 * Post `count` empty receive WRs to qp_left's receive queue. The signal
 * payload (the credit token) arrives in-band in the work completion, so no
 * data buffer is needed. Returns 0 on success, -1 on failure.
 */
int post_recvs(pg_handle *h, int count)
{
    struct ibv_recv_wr wr = {0}, *bad = NULL;
    wr.wr_id = 0; /* unused for these receives */

    for (int i = 0; i < count; ++i) {
        if (ibv_post_recv(h->qp_left, &wr, &bad)) {
            perror("ibv_post_recv");
            return -1;
        }
    }
    return 0;
}

/*
 * Asynchronously post an RDMA READ on qp_left. Reads `len` bytes from
 * `remote_addr` in the left neighbor's buffer into `local_addr` in ours.
 * Signaled, so it produces a completion on send_cq. Returns 0 / -1.
 */
int async_post_read(pg_handle *h, uint64_t remote_addr, void *local_addr,
                    size_t len, uint64_t wr_id)
{
    if (len == 0) {
        return 0;
    }
    if (len > UINT32_MAX) {
        return -1;
    }

    struct ibv_sge sge = {
        .addr   = (uintptr_t)local_addr,
        .length = (uint32_t)len,
        .lkey   = h->mr->lkey
    };

    struct ibv_send_wr wr = {0}, *bad = NULL;
    wr.wr_id               = wr_id;
    wr.sg_list             = &sge;
    wr.num_sge             = 1;
    wr.opcode              = IBV_WR_RDMA_READ;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = h->left_conn_info.rkey;

    if (ibv_post_send(h->qp_left, &wr, &bad)) {
        fprintf(stderr, "[Rank %d] async_post_read(wr_id=%" PRIu64 ") failed: %s\n",
                h->rank, wr_id, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Asynchronously post a zero-byte RDMA WRITE_WITH_IMM on qp_right. The 32-bit
 * immediate token tells the right neighbor exactly which block is now ready.
 * Returns 0 / -1.
 */
int async_post_signal(pg_handle *h, phase_t ph, int step, int chunk,
                      int block, uint64_t wr_id)
{
    struct ibv_sge sge = {0}; /* zero-length */

    struct ibv_send_wr wr = {0}, *bad = NULL;
    wr.wr_id               = wr_id;
    wr.sg_list             = &sge;
    wr.num_sge             = 0;
    wr.opcode              = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.imm_data            = htonl(encode_imm(ph, step, chunk, block));
    wr.wr.rdma.remote_addr = h->right_conn_info.addr;
    wr.wr.rdma.rkey        = h->right_conn_info.rkey;

    if (ibv_post_send(h->qp_right, &wr, &bad)) {
        fprintf(stderr, "[Rank %d] async_post_signal(wr_id=%" PRIu64 ") failed: %s\n",
                h->rank, wr_id, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Poll recv_cq until the exact (phase, step, chunk, block) credit arrives.
 * Each consumed receive is immediately re-armed so the RQ never drains;
 * older/unexpected credits are harmless and skipped. Returns 0 / -1.
 */
int wait_for_block_ready(pg_handle *h, phase_t exp_ph, int exp_step,
                         int exp_chunk, int exp_block)
{
    struct ibv_wc wc;
    int n;

    for (;;) {
        do {
            n = ibv_poll_cq(h->recv_cq, 1, &wc);
        } while (n == 0);

        if (n < 0) {
            perror("poll_cq");
            return -1;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "wait_for_block_ready wc error: %s\n",
                    ibv_wc_status_str(wc.status));
            return -1;
        }

        if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM &&
            wc.qp_num == h->qp_left->qp_num) {

            /* Re-arm the receive queue before processing this credit. */
            post_recvs(h, 1);

            phase_t ph;
            int step, chunk, block;
            decode_imm(ntohl(wc.imm_data), &ph, &step, &chunk, &block);

            if (ph == exp_ph && step == exp_step &&
                chunk == exp_chunk && block == exp_block) {
                return 0;
            }
            /* Stale credit: already re-armed, so keep polling. */
        }
    }
}

/*
 * Poll send_cq until the completion for `wr_id` is seen. Completions for other
 * work requests (e.g. older signals) are skipped. Because both QPs post to
 * send_cq, those skipped completions are also drained here. Returns 0 / -1.
 */
int wait_for_send_completion(pg_handle *h, uint64_t wr_id)
{
    struct ibv_wc wc;
    int n;

    for (;;) {
        do {
            n = ibv_poll_cq(h->send_cq, 1, &wc);
        } while (n == 0);

        if (n < 0) {
            perror("poll_cq in wait_for_send_completion");
            return -1;
        }
        if (wc.wr_id != wr_id) {
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "[Rank %d] wait_for_send_completion (id %" PRIu64 ") failed: %s\n",
                    h->rank, wr_id, ibv_wc_status_str(wc.status));
            return -1;
        }
        return 0;
    }
}
