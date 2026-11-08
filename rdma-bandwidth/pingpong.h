/*
 * pingpong.h - Shared types and declarations for the RDMA SEND bandwidth
 *              benchmark. Split across ib_verbs.c (verbs resources and data
 *              plane), tcp_exchange.c (out-of-band connection setup), and
 *              bw_bench.c (size sweep and driver).
 *
 * Derived from the InfiniBand "pingpong" bandwidth template. The upstream
 * BSD/GPL dual license is reproduced below and applies to this derived work.
 */

/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2006 Cisco Systems.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef PINGPONG_H
#define PINGPONG_H

#include <infiniband/verbs.h>

/* Completions polled per batch. */
#define WC_BATCH 10

/* Length of a GID in its 32-hex-char wire form, plus the NUL terminator. */
#define GID_STRING_LEN 33

/* Work-request IDs used to tag send and receive completions. */
enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2
};

/* Per-process RDMA resources and the message buffer. */
struct pingpong_context {
    struct ibv_context     *context;
    struct ibv_comp_channel *channel;
    struct ibv_pd          *pd;
    struct ibv_mr          *mr;
    struct ibv_cq          *cq;
    struct ibv_qp          *qp;
    void                   *buf;
    int                     size;
    int                     rx_depth;
    int                     routs;
    int                     max_inline_data;
    struct ibv_port_attr    portinfo;
};

/* Connection info exchanged out-of-band to connect the queue pairs. */
struct pingpong_dest {
    int             lid;
    int             qpn;
    int             psn;
    union ibv_gid   gid;
};

/* System page size; set once by the driver and read during setup. */
extern int page_size;

/* ---- ib_verbs.c: verbs resources and data plane ---- */
enum ibv_mtu pp_mtu_to_enum(int mtu);
int pp_get_port_info(struct ibv_context *context, int port,
                     struct ibv_port_attr *attr);
int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
                   enum ibv_mtu mtu, int sl, struct pingpong_dest *dest,
                   int sgid_idx);
struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
                                     int rx_depth, int tx_depth, int port,
                                     int use_event, int is_server);
int pp_close_ctx(struct pingpong_context *ctx);
int pp_post_recv(struct pingpong_context *ctx, int n);
int pp_post_send(struct pingpong_context *ctx);
int pp_wait_completions(struct pingpong_context *ctx, int iters);

/* ---- tcp_exchange.c: out-of-band connection setup ---- */
struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
                                          const struct pingpong_dest *my_dest);
struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
                                          int ib_port, enum ibv_mtu mtu,
                                          int port, int sl,
                                          const struct pingpong_dest *my_dest,
                                          int sgid_idx);

#endif /* PINGPONG_H */
