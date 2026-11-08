/*
 * connection.c - Process-group setup, teardown, and TCP bootstrap.
 *
 * Builds the RDMA resources (context, PD, CQs, two QPs), discovers this
 * process's rank in the ring, and connects to the left and right neighbors.
 * Neighbors exchange connection "business cards" over TCP using an even/odd
 * ordering that avoids a connect/accept deadlock, then both QPs are driven
 * through INIT -> RTR -> RTS so they are ready for RDMA traffic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "pg_internal.h"

/* ---- TCP helpers ---- */

/* Read exactly `n` bytes; handles partial reads and EINTR. */
static ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, (char *)buf + off, n - off);
        if (r == 0) {
            return (ssize_t)off; /* peer closed */
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)r;
    }
    return (ssize_t)off;
}

/* Write exactly `n` bytes; handles partial writes and EINTR. */
static ssize_t write_full(int fd, const void *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char *)buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)w;
    }
    return (ssize_t)off;
}

/* Split "host:port" into host and port strings, applying a default port. */
static int parse_hostport(const char *hostport, const char *default_port,
                          char **host_out, char **port_out)
{
    if (!hostport || !host_out || !port_out) {
        return -1;
    }
    const char *colon = strrchr(hostport, ':');
    if (!colon || colon == hostport || *(colon + 1) == '\0') {
        *host_out = strdup(hostport ? hostport : "0.0.0.0");
        *port_out = strdup(default_port ? default_port : "18515");
        return (*host_out && *port_out) ? 0 : -1;
    }
    size_t host_len = (size_t)(colon - hostport);
    *host_out = (char *)malloc(host_len + 1);
    *port_out = strdup(colon + 1);
    if (!*host_out || !*port_out) {
        free(*host_out);
        free(*port_out);
        return -1;
    }
    memcpy(*host_out, hostport, host_len);
    (*host_out)[host_len] = '\0';
    return 0;
}

/* Connect a TCP socket to "host:port"; returns fd or -1. */
static int tcp_client_connect(const char *hostport)
{
    char *host = NULL, *port = NULL;
    if (parse_hostport(hostport, "18515", &host, &port) != 0) {
        return -1;
    }

    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s,%s): %s\n", host, port, gai_strerror(rc));
        free(host);
        free(port);
        return -1;
    }

    int sockfd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            sockfd = fd;
            break;
        }
        close(fd);
    }
    freeaddrinfo(res);
    free(host);
    free(port);
    return sockfd;
}

/* Retry a TCP connect with exponential backoff (the neighbor may be booting). */
static int connect_with_retry(const char *hostport, int attempts,
                              int backoff_ms_initial)
{
    int delay = backoff_ms_initial > 0 ? backoff_ms_initial : 50;
    for (int i = 0; i < attempts; ++i) {
        int fd = tcp_client_connect(hostport);
        if (fd >= 0) {
            return fd;
        }
        usleep((useconds_t)delay * 1000);
        delay *= 2;
        if (delay > 2000) {
            delay = 2000;
        }
    }
    return -1;
}

/* Create a listening TCP socket on the given "host:port"; returns fd or -1. */
static int tcp_server_listen(const char *hostport)
{
    char *host_ignored = NULL, *port = NULL;
    if (parse_hostport(hostport, "18515", &host_ignored, &port) != 0) {
        return -1;
    }

    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    int rc = getaddrinfo(NULL, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(NULL,%s): %s\n", port, gai_strerror(rc));
        free(host_ignored);
        free(port);
        return -1;
    }

    int listen_fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int yes = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            listen_fd = fd;
            break;
        }
        close(fd);
    }
    freeaddrinfo(res);

    if (listen_fd < 0) {
        fprintf(stderr, "tcp_server_listen: bind *:%s failed (errno=%d)\n",
                port, errno);
        free(host_ignored);
        free(port);
        return -1;
    }
    if (listen(listen_fd, 1) != 0) { /* a single neighbor connects to us */
        perror("listen");
        close(listen_fd);
        free(host_ignored);
        free(port);
        return -1;
    }
    free(host_ignored);
    free(port);
    return listen_fd;
}

/* ---- QP connection info and state transitions ---- */

/* Gather this process's local QP info (LID, QPN, PSN). */
static int get_local_conn_info(struct ibv_context *ctx, struct ibv_qp *qp,
                               qp_conn_info *info_out)
{
    struct ibv_port_attr port_attr;
    if (ibv_query_port(ctx, 1, &port_attr)) {
        perror("ibv_query_port");
        return -1;
    }
    info_out->lid = port_attr.lid;
    info_out->qp_num = qp->qp_num;
    info_out->psn = lrand48() & 0xffffff;
    return 0;
}

/* Exchange connection info with a peer over a connected TCP socket. */
static int exchange_qp_info(int sock, qp_conn_info *local, qp_conn_info *remote)
{
    if (write_full(sock, local, sizeof(*local)) != (ssize_t)sizeof(*local)) {
        perror("send local qp info");
        return -1;
    }
    if (read_full(sock, remote, sizeof(*remote)) != (ssize_t)sizeof(*remote)) {
        perror("recv remote qp info");
        return -1;
    }
    return 0;
}

/* Drive a QP from INIT through RTR to RTS so it can carry RDMA traffic. */
static int modify_qp_to_rtr_rts(struct ibv_qp *qp, qp_conn_info *remote_info,
                                qp_conn_info *local_info)
{
    struct ibv_qp_attr attr;

    /* INIT -> RTR (Ready To Receive). */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = IBV_MTU_4096;
    attr.dest_qp_num        = remote_info->qp_num;
    attr.rq_psn             = remote_info->psn;
    attr.max_dest_rd_atomic = 8;
    attr.min_rnr_timer      = 12;
    attr.ah_attr.is_global  = 0;
    attr.ah_attr.dlid       = remote_info->lid;
    attr.ah_attr.sl         = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num   = 1;
    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        perror("ibv_modify_qp to RTR");
        return -1;
    }

    /* RTR -> RTS (Ready To Send). */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = local_info->psn;
    attr.max_rd_atomic = 8;
    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
        perror("ibv_modify_qp to RTS");
        return -1;
    }
    return 0;
}

/* ---- Public API: group lifecycle ---- */

int connect_process_group(const char *server_str, void **out_pg)
{
    if (out_pg) {
        *out_pg = NULL;
    }

    /* 1. Parse the server list and locate our own rank. */
    char *str_copy = strdup(server_str);
    if (!str_copy) {
        perror("strdup");
        return -1;
    }

    int num_servers = 0;
    char **server_list = NULL;
    for (char *tok = strtok(str_copy, " "); tok; tok = strtok(NULL, " ")) {
        char **grown = realloc(server_list, sizeof(char *) * (num_servers + 1));
        if (!grown) {
            perror("realloc");
            free(server_list);
            free(str_copy);
            return -1;
        }
        server_list = grown;
        server_list[num_servers++] = tok;
    }

    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        perror("gethostname");
        free(server_list);
        free(str_copy);
        return -1;
    }

    int rank = -1;
    for (int i = 0; i < num_servers; i++) {
        if (strstr(server_list[i], hostname)) {
            rank = i;
            break;
        }
    }
    if (rank == -1) {
        fprintf(stderr, "Could not find own hostname '%s' in server list\n",
                hostname);
        free(server_list);
        free(str_copy);
        return -1;
    }

    /* 2. Allocate the handle and core verbs resources. */
    pg_handle *handle = calloc(1, sizeof(pg_handle));
    if (!handle) {
        perror("calloc");
        free(server_list);
        free(str_copy);
        return -1;
    }

    handle->rank = rank;
    handle->num_servers = num_servers;
    srand48((long)(time(NULL) ^ getpid() ^ rank));
    handle->steps = (num_servers > 0) ? (num_servers - 1) : 0;
    handle->chunks = num_servers;

    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list || !dev_list[0]) {
        perror("ibv_get_device_list");
        goto fail;
    }
    handle->context = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!handle->context) {
        perror("ibv_open_device");
        goto fail;
    }

    handle->pd = ibv_alloc_pd(handle->context);
    if (!handle->pd) {
        perror("ibv_alloc_pd");
        goto fail;
    }

    /* One CQ for all outbound completions, one for all inbound signals. */
    handle->send_cq = ibv_create_cq(handle->context, MAX_RECV_WR * 2, NULL, NULL, 0);
    handle->recv_cq = ibv_create_cq(handle->context, MAX_RECV_WR * 2, NULL, NULL, 0);
    if (!handle->send_cq || !handle->recv_cq) {
        perror("ibv_create_cq");
        goto fail;
    }

    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = handle->send_cq,
        .recv_cq = handle->recv_cq,
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 512, .max_recv_wr = MAX_RECV_WR,
                 .max_send_sge = 1, .max_recv_sge = 1 }
    };
    handle->qp_left  = ibv_create_qp(handle->pd, &qp_init_attr);
    handle->qp_right = ibv_create_qp(handle->pd, &qp_init_attr);
    if (!handle->qp_left || !handle->qp_right) {
        perror("ibv_create_qp");
        goto fail;
    }

    /* 3. Allocate and register the single pinned buffer. */
    if (pg_alloc_and_register(handle) != 0) {
        goto fail;
    }

    /* A single-process group needs no network setup. */
    if (num_servers == 1) {
        *out_pg = handle;
        free(server_list);
        free(str_copy);
        return 0;
    }

    /* 4. Transition both QPs to INIT. */
    struct ibv_qp_attr init_attr = {
        .qp_state = IBV_QPS_INIT, .pkey_index = 0, .port_num = 1,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    };
    int init_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(handle->qp_left, &init_attr, init_mask) ||
        ibv_modify_qp(handle->qp_right, &init_attr, init_mask)) {
        perror("ibv_modify_qp (INIT)");
        goto fail;
    }

    /* 5. Build our business cards (both point at the same buffer/rkey). */
    int right_rank = (rank + 1) % num_servers;
    qp_conn_info local_left_info, remote_left_info;
    qp_conn_info local_right_info, remote_right_info;
    get_local_conn_info(handle->context, handle->qp_left, &local_left_info);
    get_local_conn_info(handle->context, handle->qp_right, &local_right_info);
    local_left_info.addr = local_right_info.addr = (uintptr_t)handle->buf;
    local_left_info.rkey = local_right_info.rkey = handle->mr->rkey;

    /* 6. TCP bootstrap with even/odd ordering to avoid deadlock. */
    int listen_fd = tcp_server_listen(server_list[rank]);
    if (listen_fd < 0) {
        goto fail;
    }

    int left_fd = -1, right_fd = -1;
    if (rank % 2 == 0) {
        /* Even ranks: connect right, then accept left. */
        right_fd = connect_with_retry(server_list[right_rank], 20, 100);
        if (right_fd < 0) {
            fprintf(stderr, "[Rank %d] connect to right neighbor (rank %d) at %s failed\n",
                    rank, right_rank, server_list[right_rank]);
            close(listen_fd);
            goto fail;
        }
        left_fd = accept(listen_fd, NULL, NULL);
        if (left_fd < 0) {
            perror("accept from left neighbor");
            close(listen_fd);
            close(right_fd);
            goto fail;
        }
    } else {
        /* Odd ranks: accept left, then connect right. */
        left_fd = accept(listen_fd, NULL, NULL);
        if (left_fd < 0) {
            perror("accept from left neighbor");
            close(listen_fd);
            goto fail;
        }
        right_fd = connect_with_retry(server_list[right_rank], 20, 100);
        if (right_fd < 0) {
            fprintf(stderr, "[Rank %d] connect to right neighbor (rank %d) at %s failed\n",
                    rank, right_rank, server_list[right_rank]);
            close(listen_fd);
            close(left_fd);
            goto fail;
        }
    }
    close(listen_fd);

    /* Exchange in matching even/odd order to avoid a write/read deadlock. */
    if (rank % 2 == 0) {
        if (exchange_qp_info(right_fd, &local_right_info, &remote_right_info) != 0 ||
            exchange_qp_info(left_fd,  &local_left_info,  &remote_left_info)  != 0) {
            close(left_fd);
            close(right_fd);
            goto fail;
        }
    } else {
        if (exchange_qp_info(left_fd,  &local_left_info,  &remote_left_info)  != 0 ||
            exchange_qp_info(right_fd, &local_right_info, &remote_right_info) != 0) {
            close(left_fd);
            close(right_fd);
            goto fail;
        }
    }
    close(right_fd);
    close(left_fd);

    /* 7. Bring both QPs up to RTS. */
    handle->left_conn_info  = remote_left_info;
    handle->right_conn_info = remote_right_info;
    if (modify_qp_to_rtr_rts(handle->qp_left, &remote_left_info, &local_left_info) != 0) {
        goto fail;
    }
    if (modify_qp_to_rtr_rts(handle->qp_right, &remote_right_info, &local_right_info) != 0) {
        goto fail;
    }

    /* 8. Prime the receive queue so inbound credits are caught. */
    if (post_recvs(handle, MAX_RECV_WR) != 0) {
        goto fail;
    }

    *out_pg = handle;
    free(server_list);
    free(str_copy);
    return 0;

fail:
    pg_close(handle); /* safe on a partially initialized handle */
    free(server_list);
    free(str_copy);
    return -1;
}

int pg_close(void *pg)
{
    pg_handle *h = (pg_handle *)pg;
    if (!h) {
        return 0;
    }

    /* Tear down in reverse order of creation. */
    if (h->qp_left) {
        ibv_destroy_qp(h->qp_left);
    }
    if (h->qp_right) {
        ibv_destroy_qp(h->qp_right);
    }
    if (h->send_cq) {
        ibv_destroy_cq(h->send_cq);
    }
    if (h->recv_cq) {
        ibv_destroy_cq(h->recv_cq);
    }

    pg_release_memory(h); /* dereg MR + free buffer (before PD dealloc) */

    if (h->pd) {
        ibv_dealloc_pd(h->pd);
    }
    if (h->context) {
        ibv_close_device(h->context);
    }

    free(h);
    return 0;
}
