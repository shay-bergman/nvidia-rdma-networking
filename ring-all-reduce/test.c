/*
 * test.c - Driver for the Ring All-Reduce library.
 *
 * Runs two suites against a connected process group:
 *   - logic_test:       small fixed vectors that verify reduce-scatter,
 *                       all-gather, and all-reduce produce correct results.
 *   - throughput_test:  a range of message sizes timed over many iterations
 *                       to measure all-reduce bandwidth.
 *
 * Per-node input in logic_test is keyed off the hostname. Replace the node
 * names below with the hostnames of the ring so each node seeds distinct data.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "rdma_lib.h"

/* Elapsed time between two monotonic timestamps, in microseconds. */
static double time_diff_usec(struct timespec start, struct timespec end)
{
    double start_usec = start.tv_sec * 1e6 + start.tv_nsec / 1e3;
    double end_usec = end.tv_sec * 1e6 + end.tv_nsec / 1e3;
    return end_usec - start_usec;
}

/*
 * Seed `buf` with node-specific data so the reduction has a known expected
 * result. Returns 0 on success, -1 if the hostname is not in the ring list.
 */
static int seed_node_data(const char *hostname, int *buf, size_t n)
{
    int multiplier;
    if (strstr(hostname, "node1")) {
        multiplier = 1;
    } else if (strstr(hostname, "node2")) {
        multiplier = 10;
    } else if (strstr(hostname, "node3")) {
        multiplier = 100;
    } else if (strstr(hostname, "node4")) {
        multiplier = 1000;
    } else {
        fprintf(stderr, "Unknown hostname: %s\n", hostname);
        return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        buf[i] = (int)i * multiplier;
    }
    return 0;
}

/* Validate reduce-scatter, all-gather, and all-reduce on a small vector. */
static int logic_test(pg_handle *handle)
{
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        perror("gethostname");
        return -1;
    }

    const size_t buffer_size = 12;
    int *buf = malloc(buffer_size * sizeof(int));
    int *send_buf = malloc(buffer_size * sizeof(int));
    if (!buf || !send_buf) {
        perror("malloc");
        free(buf);
        free(send_buf);
        return -1;
    }

    if (seed_node_data(hostname, buf, buffer_size) != 0) {
        free(buf);
        free(send_buf);
        return -1;
    }

    printf("Host %s starting test with buffer of size %zu:\n", hostname, buffer_size);
    for (size_t i = 0; i < buffer_size; ++i) {
        printf("%d ", buf[i]);
    }
    printf("\n");

    /* All-reduce (in place): every node should end with the summed vector. */
    printf("Running pg_all_reduce...\n");
    if (pg_all_reduce(buf, buf, buffer_size, DATATYPE_INT, OP_SUM, handle) == -1) {
        printf("pg_all_reduce failed...\n");
    }
    printf("After pg_all_reduce:\n");
    for (size_t i = 0; i < buffer_size; ++i) {
        printf("%d ", buf[i]);
    }
    printf("\n");

    /* Reduce-scatter: each node keeps only its designated reduced chunk. */
    if (seed_node_data(hostname, buf, buffer_size) != 0) {
        free(buf);
        free(send_buf);
        return -1;
    }
    printf("Running pg_reduce_scatter...\n");
    pg_reduce_scatter(buf, send_buf, buffer_size, DATATYPE_INT, OP_SUM, handle);
    printf("After pg_reduce_scatter:\n");
    for (size_t i = 0; i < buffer_size; ++i) {
        printf("%d ", send_buf[i]);
    }
    printf("\n");

    /* All-gather: reassemble the full vector from the scattered chunks. */
    printf("Running pg_all_gather...\n");
    pg_all_gather(send_buf, buf, buffer_size, DATATYPE_INT, OP_SUM, handle);
    printf("After pg_all_gather:\n");
    for (size_t i = 0; i < buffer_size; ++i) {
        printf("%d ", buf[i]);
    }
    printf("\n");

    free(buf);
    free(send_buf);
    return 0;
}

/* Time all-reduce across a sweep of message sizes and report throughput. */
static int throughput_test(pg_handle *handle)
{
    const size_t buffer_size = 3 << 20; /* 3 Mi integers */
    printf("Testing pg_all_reduce with buffer size: %zu bytes\n",
           buffer_size * sizeof(int));

    int *recv_buff = calloc(buffer_size, sizeof(int));
    int *send_buff = calloc(buffer_size, sizeof(int));
    if (!recv_buff || !send_buff) {
        perror("calloc");
        free(recv_buff);
        free(send_buff);
        return -1;
    }
    for (size_t i = 0; i < buffer_size; ++i) {
        recv_buff[i] = 1;
    }

    const int iters = 1000;
    for (int exp = 3; exp < 21; ++exp) {
        int size = 3 << exp;

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int i = 0; i < iters; ++i) {
            if (pg_all_reduce(send_buff, recv_buff, size,
                              DATATYPE_INT, OP_SUM, handle) == -1) {
                fprintf(stderr, "pg_all_reduce failed\n");
                free(recv_buff);
                free(send_buff);
                return -1;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &end);

        double usec = time_diff_usec(start, end);
        double sec = usec / 1e6;
        printf("Test with size %d: took %.2f seconds for %d iterations\n",
               size, sec, iters);
        printf("Average time per iteration: %d microseconds\n",
               (int)usec / iters);
        printf("Average throughput: %.2f Mb/s\n",
               (8.0 * sizeof(int) * size * iters) / (sec * (1 << 20)));
        printf("\n");
    }

    free(recv_buff);
    free(send_buff);
    return 0;
}

int main(int argc, char const *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s \"host1 host2 ... hostN\"\n", argv[0]);
        return EXIT_FAILURE;
    }

    pg_handle *handle;
    if (connect_process_group(argv[1], (void **)&handle)) {
        return EXIT_FAILURE;
    }
    if (logic_test(handle)) {
        pg_close(handle);
        return EXIT_FAILURE;
    }
    if (throughput_test(handle)) {
        pg_close(handle);
        return EXIT_FAILURE;
    }

    pg_close(handle);
    return 0;
}
