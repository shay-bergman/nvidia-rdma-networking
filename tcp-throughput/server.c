/*
 * server.c - TCP throughput benchmark, receiving side.
 *
 * Binds to PORT, accepts a single client, and for each payload size in the
 * sweep (1 B, 2 B, 4 B, ... , 1 MiB) drains WARMUP_CYCLES + TEST_CYCLES
 * messages and replies with a one-byte ACK so the client can stop its timer.
 */
#include "shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* Create, bind, and listen on a TCP socket; returns the listening fd. */
static int setup_server_socket(void)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        error_exit("socket");
    }

    /* Allow a quick restart of the server on the same port. */
    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; /* accept on any local interface */
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_exit("bind");
    }
    if (listen(server_fd, 5) < 0) {
        error_exit("listen");
    }

    printf("Server listening on port %d...\n", PORT);
    return server_fd;
}

/* Block until a client connects; returns the connected client fd. */
static int accept_client(int server_fd)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        error_exit("accept");
    }
    return client_fd;
}

/*
 * Run the receive-and-ACK cycle across every payload size in the sweep. The
 * one-byte ACK after each size bounds the client's timed window.
 */
static void handle_client(int client_fd)
{
    for (size_t size = 1; size <= ONE_MB; size *= 2) {
        char *buffer = allocate_buffer(size);

        for (int i = 0; i < WARMUP_CYCLES; i++) {
            receive_all(client_fd, buffer, size);
        }
        for (int i = 0; i < TEST_CYCLES; i++) {
            receive_all(client_fd, buffer, size);
        }

        char ack = 'A';
        send_all(client_fd, &ack, 1);

        free(buffer);
    }
}

int main(void)
{
    int server_fd = setup_server_socket();
    int client_fd = accept_client(server_fd);

    handle_client(client_fd);

    close(client_fd);
    close(server_fd);
    return 0;
}
