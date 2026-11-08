/*
 * client.c - TCP throughput benchmark, sending side.
 *
 * Connects to a server and, for each payload size in the sweep
 * (1 B, 2 B, 4 B, ... , 1 MiB), sends WARMUP_CYCLES un-timed messages followed
 * by TEST_CYCLES timed messages, then waits for the server's one-byte ACK.
 * Throughput is printed per size as "size <TAB> value <TAB> MB/s".
 *
 * Usage: ./client <server-ip>
 */
#include "shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* Elapsed wall-clock time between two timestamps, in seconds. */
static double get_elapsed_time_sec(struct timeval start, struct timeval end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_usec - start.tv_usec) / 1e6;
}

/* Create a TCP socket and connect to `ip_address` on PORT. */
static int connect_to_server(const char *ip_address)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error_exit("socket");
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, ip_address, &server_addr.sin_addr) <= 0) {
        error_exit("Invalid address");
    }
    if (connect(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        error_exit("connect");
    }
    return fd;
}

/*
 * Sweep payload sizes, timing TEST_CYCLES sends at each. The server ACK is
 * awaited inside the timed window so the measurement reflects the time for the
 * data to be fully drained by the receiver.
 */
static void run_benchmark(int fd)
{
    for (size_t size = 1; size <= ONE_MB; size *= 2) {
        char *buffer = allocate_buffer(size);
        char ack;

        for (int i = 0; i < WARMUP_CYCLES; i++) {
            send_all(fd, buffer, size);
        }

        struct timeval start, end;
        gettimeofday(&start, NULL);
        for (int i = 0; i < TEST_CYCLES; i++) {
            send_all(fd, buffer, size);
        }
        receive_all(fd, &ack, 1); /* wait for the server ACK before stopping */
        gettimeofday(&end, NULL);

        double time_sec = get_elapsed_time_sec(start, end);
        double total_bytes = (double)size * TEST_CYCLES;
        double throughput = total_bytes / time_sec / (1024.0 * 1024.0); /* MB/s */

        printf("%zu\t%.4f\tMB/s\n", size, throughput);

        free(buffer);
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server-ip>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int socket_fd = connect_to_server(argv[1]);
    run_benchmark(socket_fd);
    close(socket_fd);
    return 0;
}
