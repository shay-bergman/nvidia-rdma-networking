/*
 * tcp_exchange.c - Out-of-band (TCP) exchange of RDMA connection info.
 *
 * Before any RDMA traffic flows, the two peers must learn each other's LID,
 * QP number, PSN, and GID. They do that over an ordinary TCP socket here; the
 * server also brings its QP up to RTS as part of the exchange.
 *
 * Derived from the InfiniBand "pingpong" bandwidth template; see pingpong.h
 * for the upstream license.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include "pingpong.h"

/* Parse a 32-hex-char wire GID into a union ibv_gid. */
static void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
    char tmp[9];
    uint32_t v32;

    tmp[8] = 0;
    for (int i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        *(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
    }
}

/* Format a union ibv_gid into a 32-hex-char wire GID. */
static void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
    for (int i = 0; i < 4; ++i) {
        sprintf(&wgid[i * 8], "%08x",
                htonl(*(uint32_t *)(gid->raw + i * 4)));
    }
}

/*
 * Client side of the exchange: connect to the server, send our address, read
 * the server's address back, and confirm. Returns the remote dest (caller
 * frees) or NULL on failure.
 */
struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
                                          const struct pingpong_dest *my_dest)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1;
    struct pingpong_dest *rem_dest = NULL;
    char gid[GID_STRING_LEN];

    if (asprintf(&service, "%d", port) < 0) {
        return NULL;
    }

    n = getaddrinfo(servername, service, &hints, &res);
    if (n < 0) {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            if (!connect(sockfd, t->ai_addr, t->ai_addrlen)) {
                break;
            }
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
        return NULL;
    }

    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s",
            my_dest->lid, my_dest->qpn, my_dest->psn, gid);
    if (write(sockfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address\n");
        goto out;
    }

    if (read(sockfd, msg, sizeof msg) != sizeof msg) {
        perror("client read");
        fprintf(stderr, "Couldn't read remote address\n");
        goto out;
    }

    /* Acknowledge so the server can proceed. */
    if (write(sockfd, "done", sizeof "done") != sizeof "done") {
        fprintf(stderr, "Couldn't send \"done\" ack\n");
        goto out;
    }

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest) {
        goto out;
    }

    sscanf(msg, "%x:%x:%x:%s",
           &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);

out:
    close(sockfd);
    return rem_dest;
}

/*
 * Server side of the exchange: listen, accept one client, read its address,
 * bring our QP up (pp_connect_ctx), then send our address back. Returns the
 * remote dest (caller frees) or NULL on failure.
 */
struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
                                          int ib_port, enum ibv_mtu mtu,
                                          int port, int sl,
                                          const struct pingpong_dest *my_dest,
                                          int sgid_idx)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_flags    = AI_PASSIVE,
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1, connfd;
    struct pingpong_dest *rem_dest = NULL;
    char gid[GID_STRING_LEN];

    if (asprintf(&service, "%d", port) < 0) {
        return NULL;
    }

    n = getaddrinfo(NULL, service, &hints, &res);
    if (n < 0) {
        fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            int one = 1;
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (!bind(sockfd, t->ai_addr, t->ai_addrlen)) {
                break;
            }
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't listen to port %d\n", port);
        return NULL;
    }

    listen(sockfd, 1);
    connfd = accept(sockfd, NULL, 0);
    close(sockfd);
    if (connfd < 0) {
        fprintf(stderr, "accept() failed\n");
        return NULL;
    }

    n = read(connfd, msg, sizeof msg);
    if (n != sizeof msg) {
        perror("server read");
        fprintf(stderr, "%d/%d: Couldn't read remote address\n",
                n, (int)sizeof msg);
        goto out;
    }

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest) {
        goto out;
    }

    sscanf(msg, "%x:%x:%x:%s",
           &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);

    if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest, sgid_idx)) {
        fprintf(stderr, "Couldn't connect to remote QP\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }

    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s",
            my_dest->lid, my_dest->qpn, my_dest->psn, gid);
    if (write(connfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }

    /* Wait for the client's "done" ack before returning. */
    if (read(connfd, msg, sizeof msg) <= 0) {
        fprintf(stderr, "Couldn't read \"done\" ack\n");
        free(rem_dest);
        rem_dest = NULL;
    }

out:
    close(connfd);
    return rem_dest;
}
