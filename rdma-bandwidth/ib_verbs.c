/*
 * ib_verbs.c - InfiniBand verbs resource management and data plane for the
 *              RDMA SEND bandwidth benchmark: device/context setup, queue-pair
 *              state transitions, and the post/poll send and receive paths.
 *
 * Derived from the InfiniBand "pingpong" bandwidth template; see pingpong.h
 * for the upstream license.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/param.h> /* roundup */

#include "pingpong.h"

int page_size;

/* Map a numeric path MTU to its verbs enum, or -1 if unsupported. */
enum ibv_mtu pp_mtu_to_enum(int mtu)
{
    switch (mtu) {
    case 256:  return IBV_MTU_256;
    case 512:  return IBV_MTU_512;
    case 1024: return IBV_MTU_1024;
    case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096;
    default:   return -1;
    }
}

/* Query attributes of an IB port. */
int pp_get_port_info(struct ibv_context *context, int port,
                     struct ibv_port_attr *attr)
{
    return ibv_query_port(context, port, attr);
}

/*
 * Drive the QP from INIT to RTR and then RTS using the remote peer's address
 * info, so it can both receive and send. Returns 0 on success, 1 on failure.
 */
int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
                   enum ibv_mtu mtu, int sl, struct pingpong_dest *dest,
                   int sgid_idx)
{
    struct ibv_qp_attr attr = {
        .qp_state           = IBV_QPS_RTR,
        .path_mtu           = mtu,
        .dest_qp_num        = dest->qpn,
        .rq_psn             = dest->psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer      = 12,
        .ah_attr = {
            .is_global     = 0,
            .dlid          = dest->lid,
            .sl            = sl,
            .src_path_bits = 0,
            .port_num      = port
        }
    };

    /* A non-zero interface id means the peer gave a GID: route globally. */
    if (dest->gid.global.interface_id) {
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.hop_limit  = 1;
        attr.ah_attr.grh.dgid       = dest->gid;
        attr.ah_attr.grh.sgid_index = sgid_idx;
    }

    if (ibv_modify_qp(ctx->qp, &attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state     = IBV_QPS_RTS;
    attr.timeout      = 14;
    attr.retry_cnt    = 7;
    attr.rnr_retry    = 7;
    attr.sq_psn       = my_psn;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(ctx->qp, &attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                      IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }
    return 0;
}

/*
 * Allocate the device context, PD, MR, CQ, and a reliable-connection QP, and
 * move the QP to INIT. Returns the context, or NULL on failure.
 */
struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
                                     int rx_depth, int tx_depth, int port,
                                     int use_event, int is_server)
{
    struct pingpong_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->size = size;
    ctx->rx_depth = rx_depth;
    ctx->routs = rx_depth;

    ctx->buf = malloc(roundup(size, page_size));
    if (!ctx->buf) {
        fprintf(stderr, "Couldn't allocate work buf.\n");
        goto clean_ctx;
    }
    /* Distinct server/client fill patterns aid debugging of stale data. */
    memset(ctx->buf, 0x7b + is_server, size);

    ctx->context = ibv_open_device(ib_dev);
    if (!ctx->context) {
        fprintf(stderr, "Couldn't get context for %s\n",
                ibv_get_device_name(ib_dev));
        goto clean_buffer;
    }

    if (use_event) {
        ctx->channel = ibv_create_comp_channel(ctx->context);
        if (!ctx->channel) {
            fprintf(stderr, "Couldn't create completion channel\n");
            goto clean_device;
        }
    } else {
        ctx->channel = NULL;
    }

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        goto clean_comp_channel;
    }

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr) {
        fprintf(stderr, "Couldn't register MR\n");
        goto clean_pd;
    }

    ctx->cq = ibv_create_cq(ctx->context, rx_depth + tx_depth, NULL,
                            ctx->channel, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        goto clean_mr;
    }

    {
        struct ibv_qp_init_attr attr = {
            .send_cq = ctx->cq,
            .recv_cq = ctx->cq,
            .cap = {
                .max_send_wr  = tx_depth,
                .max_recv_wr  = rx_depth,
                .max_send_sge = 1,
                .max_recv_sge = 1,
                /* Request inline send support (typical threshold ~256 B). */
                .max_inline_data = 256
            },
            .qp_type = IBV_QPT_RC
        };

        ctx->qp = ibv_create_qp(ctx->pd, &attr);
        if (!ctx->qp) {
            fprintf(stderr, "Couldn't create QP\n");
            goto clean_cq;
        }
        /* Record the inline capacity the NIC actually granted. */
        ctx->max_inline_data = attr.cap.max_inline_data;
    }

    {
        struct ibv_qp_attr attr = {
            .qp_state        = IBV_QPS_INIT,
            .pkey_index      = 0,
            .port_num        = port,
            .qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
        };

        if (ibv_modify_qp(ctx->qp, &attr,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                          IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
            fprintf(stderr, "Failed to modify QP to INIT\n");
            goto clean_qp;
        }
    }

    return ctx;

clean_qp:
    ibv_destroy_qp(ctx->qp);
clean_cq:
    ibv_destroy_cq(ctx->cq);
clean_mr:
    ibv_dereg_mr(ctx->mr);
clean_pd:
    ibv_dealloc_pd(ctx->pd);
clean_comp_channel:
    if (ctx->channel) {
        ibv_destroy_comp_channel(ctx->channel);
    }
clean_device:
    ibv_close_device(ctx->context);
clean_buffer:
    free(ctx->buf);
clean_ctx:
    free(ctx);
    return NULL;
}

/* Tear down all resources held by the context. Returns 0, or 1 on error. */
int pp_close_ctx(struct pingpong_context *ctx)
{
    if (ibv_destroy_qp(ctx->qp)) {
        fprintf(stderr, "Couldn't destroy QP\n");
        return 1;
    }
    if (ibv_destroy_cq(ctx->cq)) {
        fprintf(stderr, "Couldn't destroy CQ\n");
        return 1;
    }
    if (ibv_dereg_mr(ctx->mr)) {
        fprintf(stderr, "Couldn't deregister MR\n");
        return 1;
    }
    if (ibv_dealloc_pd(ctx->pd)) {
        fprintf(stderr, "Couldn't deallocate PD\n");
        return 1;
    }
    if (ctx->channel) {
        if (ibv_destroy_comp_channel(ctx->channel)) {
            fprintf(stderr, "Couldn't destroy completion channel\n");
            return 1;
        }
    }
    if (ibv_close_device(ctx->context)) {
        fprintf(stderr, "Couldn't release context\n");
        return 1;
    }

    free(ctx->buf);
    free(ctx);
    return 0;
}

/* Post `n` receive WRs; returns the number actually posted. */
int pp_post_recv(struct pingpong_context *ctx, int n)
{
    struct ibv_sge list = {
        .addr   = (uintptr_t)ctx->buf,
        .length = ctx->mr->length,
        .lkey   = ctx->mr->lkey
    };
    struct ibv_recv_wr wr = {
        .wr_id   = PINGPONG_RECV_WRID,
        .sg_list = &list,
        .num_sge = 1,
        .next    = NULL
    };
    struct ibv_recv_wr *bad_wr;
    int i;

    for (i = 0; i < n; ++i) {
        if (ibv_post_recv(ctx->qp, &wr, &bad_wr)) {
            break;
        }
    }
    return i;
}

/* Post one SEND of ctx->size bytes, inline when it fits. Returns 0 / errno. */
int pp_post_send(struct pingpong_context *ctx)
{
    struct ibv_sge list = {
        .addr   = (uintptr_t)ctx->buf,
        .length = ctx->size,
        .lkey   = ctx->mr->lkey
    };
    struct ibv_send_wr *bad_wr;
    struct ibv_send_wr wr = {
        .wr_id      = PINGPONG_SEND_WRID,
        .sg_list    = &list,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
        .next       = NULL
    };

    if (ctx->size <= ctx->max_inline_data) {
        wr.send_flags |= IBV_SEND_INLINE;
    }
    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

/*
 * Poll the CQ until `iters` completions have been reaped. Receive completions
 * trigger re-posting so the receive queue never fully drains. Returns 0 on
 * success, 1 on any failed completion or poll error.
 */
int pp_wait_completions(struct pingpong_context *ctx, int iters)
{
    int rcnt = 0, scnt = 0;

    while (rcnt + scnt < iters) {
        struct ibv_wc wc[WC_BATCH];
        int ne, i;

        do {
            ne = ibv_poll_cq(ctx->cq, WC_BATCH, wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
                return 1;
            }
        } while (ne < 1);

        for (i = 0; i < ne; ++i) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int)wc[i].wr_id);
                return 1;
            }

            switch ((int)wc[i].wr_id) {
            case PINGPONG_SEND_WRID:
                ++scnt;
                break;
            case PINGPONG_RECV_WRID:
                /* Top the receive queue back up before it runs dry. */
                if (--ctx->routs <= 10) {
                    ctx->routs += pp_post_recv(ctx, ctx->rx_depth - ctx->routs);
                    if (ctx->routs < ctx->rx_depth) {
                        fprintf(stderr, "Couldn't post receive (%d)\n",
                                ctx->routs);
                        return 1;
                    }
                }
                ++rcnt;
                break;
            default:
                fprintf(stderr, "Completion for unknown wr_id %d\n",
                        (int)wc[i].wr_id);
                return 1;
            }
        }
    }
    return 0;
}
