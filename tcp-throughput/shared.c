/*
 * shared.c - Socket utility routines shared by the client and server.
 *
 * Concentrating these helpers here keeps the connection-setup and
 * benchmarking logic in client.c / server.c free of low-level details.
 */
#include "shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

void error_exit(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

char *allocate_buffer(size_t size)
{
    /* Zero-initialized so the payload content is deterministic. */
    char *buf = calloc(1, size);
    if (!buf) {
        error_exit("calloc failed");
    }
    return buf;
}

/*
 * recv() may return fewer bytes than requested, so loop until the full
 * message has been read. A return of <= 0 means an error or an early close.
 */
void receive_all(int fd, char *buffer, size_t expected_bytes)
{
    size_t total = 0;
    while (total < expected_bytes) {
        ssize_t received = recv(fd, buffer + total, expected_bytes - total, 0);
        if (received <= 0) {
            error_exit("recv failed");
        }
        total += (size_t)received;
    }
}

/* Mirror of receive_all: loop until the full buffer has been written. */
void send_all(int fd, const char *buffer, size_t expected_bytes)
{
    size_t total = 0;
    while (total < expected_bytes) {
        ssize_t sent = send(fd, buffer + total, expected_bytes - total, 0);
        if (sent <= 0) {
            error_exit("send failed");
        }
        total += (size_t)sent;
    }
}
