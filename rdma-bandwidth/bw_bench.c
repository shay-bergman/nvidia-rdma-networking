/*
 * bw_bench.c - Driver for the RDMA SEND bandwidth benchmark: argument
 *              parsing, device selection, connection bring-up, and the
 *              payload-size sweep that measures and prints throughput.
 *
 * With no host argument the process acts as the server (receiver); given a
 * host it acts as the client (sender).
 *
 * Derived from the InfiniBand "pingpong" bandwidth template; see pingpong.h
 * for the upstream license.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <sys/time.h>
#include <arpa/inet.h>

#include "pingpong.h"

/* Largest payload in the sweep and un-timed sends before timing. */
#define ONE_MB       (1024 * 1024)
#define WARMUP_ITERS 100

/*
 * Post `total_msgs` SENDs, draining completions every WC_BATCH so the send
 * queue never overflows. Returns 0 on success, 1 on failure.
 */
static int send_batch(struct pingpong_context *ctx, int total_msgs)
{
    int posted = 0;

    while (posted < total_msgs) {
        for (int i = 0; i < WC_BATCH && posted < total_msgs; i++, posted++) {
            if (pp_post_send(ctx)) {
                fprintf(stderr, "Failed to post SEND at msg %d\n", posted);
                return 1;
            }
        }
        /* pp_wait_completions returns non-zero on any failure. */
        if (pp_wait_completions(ctx, WC_BATCH) != 0) {
            fprintf(stderr, "Completion failed\n");
            return 1;
        }
    }
    return 0;
}

/* Print one result line: "size <TAB> MB/s". */
static void print_throughput(int msg_size, int iters,
                             struct timeval start, struct timeval end)
{
    double elapsed_usec = (end.tv_sec - start.tv_sec) * 1e6 +
                          (end.tv_usec - start.tv_usec);
    double secs = elapsed_usec / 1e6;
    double mb = (double)msg_size * iters / (1024.0 * 1024.0);

    printf("%u\t%.2f\tMB/s\n", (unsigned)msg_size, mb / secs);
}

static void usage(const char *argv0)
{
    printf("Usage:\n");
    printf("  %s            start a server and wait for connection\n", argv0);
    printf("  %s <host>     connect to server at <host>\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  -p, --port=<port>      listen on/connect to port (default 12345)\n");
    printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first found)\n");
    printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
    printf("  -s, --size=<size>      message size (default 1048576)\n");
    printf("  -m, --mtu=<size>       path MTU (default 2048)\n");
    printf("  -r, --rx-depth=<dep>   receives to post at a time (default 100)\n");
    printf("  -n, --iters=<iters>    number of exchanges (default 5000)\n");
    printf("  -l, --sl=<sl>          service level value\n");
    printf("  -e, --events           sleep on CQ events (default poll)\n");
    printf("  -g, --gid-idx=<idx>    local port gid index\n");
}

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list;
    struct ibv_device *ib_dev;
    struct pingpong_context *ctx;
    struct pingpong_dest my_dest;
    struct pingpong_dest *rem_dest;
    char *ib_devname = NULL;
    char *servername = NULL;
    int port = 12345;
    int ib_port = 1;
    enum ibv_mtu mtu = IBV_MTU_2048;
    int rx_depth = 100;
    int tx_depth = 100;
    int iters = 5000; /* more iterations give more stable measurements */
    int use_event = 0;
    int size = ONE_MB;
    int sl = 0;
    int gidx = -1;
    char gid[GID_STRING_LEN];

    srand48(getpid() * time(NULL));

    while (1) {
        static struct option long_options[] = {
            { .name = "port",    .has_arg = 1, .val = 'p' },
            { .name = "ib-dev",  .has_arg = 1, .val = 'd' },
            { .name = "ib-port", .has_arg = 1, .val = 'i' },
            { .name = "size",    .has_arg = 1, .val = 's' },
            { .name = "mtu",     .has_arg = 1, .val = 'm' },
            { .name = "rx-depth",.has_arg = 1, .val = 'r' },
            { .name = "iters",   .has_arg = 1, .val = 'n' },
            { .name = "sl",      .has_arg = 1, .val = 'l' },
            { .name = "events",  .has_arg = 0, .val = 'e' },
            { .name = "gid-idx", .has_arg = 1, .val = 'g' },
            { 0 }
        };

        int c = getopt_long(argc, argv, "p:d:i:s:m:r:n:l:eg:", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'p':
            port = strtol(optarg, NULL, 0);
            if (port < 0 || port > 65535) { usage(argv[0]); return 1; }
            break;
        case 'd':
            ib_devname = strdup(optarg);
            break;
        case 'i':
            ib_port = strtol(optarg, NULL, 0);
            if (ib_port < 0) { usage(argv[0]); return 1; }
            break;
        case 's':
            size = strtol(optarg, NULL, 0);
            break;
        case 'm':
            mtu = pp_mtu_to_enum(strtol(optarg, NULL, 0));
            if (mtu < 0) { usage(argv[0]); return 1; }
            break;
        case 'r':
            rx_depth = strtol(optarg, NULL, 0);
            break;
        case 'n':
            iters = strtol(optarg, NULL, 0);
            break;
        case 'l':
            sl = strtol(optarg, NULL, 0);
            break;
        case 'e':
            ++use_event;
            break;
        case 'g':
            gidx = strtol(optarg, NULL, 0);
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind == argc - 1) {
        servername = strdup(argv[optind]);
    } else if (optind < argc) {
        usage(argv[0]);
        return 1;
    }

    page_size = sysconf(_SC_PAGESIZE);

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("Failed to get IB devices list");
        return 1;
    }

    if (!ib_devname) {
        ib_dev = *dev_list;
        if (!ib_dev) {
            fprintf(stderr, "No IB devices found\n");
            return 1;
        }
    } else {
        int i;
        for (i = 0; dev_list[i]; ++i) {
            if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname)) {
                break;
            }
        }
        ib_dev = dev_list[i];
        if (!ib_dev) {
            fprintf(stderr, "IB device %s not found\n", ib_devname);
            return 1;
        }
    }

    ctx = pp_init_ctx(ib_dev, size, rx_depth, tx_depth, ib_port,
                      use_event, !servername);
    if (!ctx) {
        return 1;
    }

    /* Pre-post receive buffers before any traffic can arrive. */
    ctx->routs = pp_post_recv(ctx, ctx->rx_depth);
    if (ctx->routs < ctx->rx_depth) {
        fprintf(stderr, "Couldn't post receive (%d)\n", ctx->routs);
        return 1;
    }

    if (use_event) {
        if (ibv_req_notify_cq(ctx->cq, 0)) {
            fprintf(stderr, "Couldn't request CQ notification\n");
            return 1;
        }
    }

    if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo)) {
        fprintf(stderr, "Couldn't get port info\n");
        return 1;
    }

    my_dest.lid = ctx->portinfo.lid;
    if (ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !my_dest.lid) {
        fprintf(stderr, "Couldn't get local LID\n");
        return 1;
    }

    if (gidx >= 0) {
        if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
            fprintf(stderr, "Could not get local gid for gid index %d\n", gidx);
            return 1;
        }
    } else {
        memset(&my_dest.gid, 0, sizeof my_dest.gid);
    }

    my_dest.qpn = ctx->qp->qp_num;
    my_dest.psn = lrand48() & 0xffffff;
    inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
    printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           my_dest.lid, my_dest.qpn, my_dest.psn, gid);

    if (servername) {
        rem_dest = pp_client_exch_dest(servername, port, &my_dest);
    } else {
        rem_dest = pp_server_exch_dest(ctx, ib_port, mtu, port, sl, &my_dest, gidx);
    }
    if (!rem_dest) {
        return 1;
    }

    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
    printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

    /* The client completes its own QP bring-up (the server did so already). */
    if (servername) {
        if (pp_connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest, gidx)) {
            return 1;
        }
    }

    /*
     * Benchmark: the client sends RDMA SENDs of increasing size and times
     * them; the server drains each size's messages and sends one control
     * message back to mark completion.
     */
    if (servername) {
        struct timeval start_time, end_time;

        for (int msg_size = 1; msg_size <= ONE_MB; msg_size *= 2) {
            ctx->size = msg_size;

            if (send_batch(ctx, WARMUP_ITERS)) {
                return 1;
            }

            gettimeofday(&start_time, NULL);
            if (send_batch(ctx, iters)) {
                return 1;
            }
            pp_wait_completions(ctx, 1); /* server's completion signal */
            gettimeofday(&end_time, NULL);

            print_throughput(msg_size, iters, start_time, end_time);
        }
    } else {
        for (int msg_size = 1; msg_size <= ONE_MB; msg_size *= 2) {
            ctx->size = msg_size;

            /* Drain the client's warm-up and timed sends. */
            pp_wait_completions(ctx, WARMUP_ITERS + iters);

            if (pp_post_send(ctx)) {
                fprintf(stderr, "Server couldn't post SEND message\n");
                return 1;
            }
            pp_wait_completions(ctx, 1); /* wait for that send to complete */
        }
        printf("Server Done.\n");
    }

    pp_close_ctx(ctx);
    ibv_free_device_list(dev_list);
    free(rem_dest);
    free(servername);
    free(ib_devname);
    return 0;
}
