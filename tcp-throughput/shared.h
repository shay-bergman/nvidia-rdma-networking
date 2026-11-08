/*
 * shared.h - Common constants and socket helpers shared by the TCP
 *            throughput client and server.
 */
#ifndef SHARED_H
#define SHARED_H

#include <stddef.h>    /* size_t  */
#include <sys/types.h> /* ssize_t */

/* TCP port both endpoints use. */
#define PORT 12345

/* Largest payload tested (1 MiB). The size sweep runs 1 B .. ONE_MB. */
#define ONE_MB (1048576)

/* Un-timed sends/receives used to let the TCP window open before timing. */
#define WARMUP_CYCLES 5

/* Timed messages exchanged at each size; averages out transient jitter. */
#define TEST_CYCLES 10000

/* Print an error (with errno context) and terminate the process. */
void error_exit(const char *msg);

/* Allocate a zero-initialized buffer of `size` bytes, or exit on failure. */
char *allocate_buffer(size_t size);

/* Receive exactly `expected_bytes` bytes, looping over short reads. */
void receive_all(int fd, char *buffer, size_t expected_bytes);

/* Send exactly `expected_bytes` bytes, looping over short writes. */
void send_all(int fd, const char *buffer, size_t expected_bytes);

#endif /* SHARED_H */
