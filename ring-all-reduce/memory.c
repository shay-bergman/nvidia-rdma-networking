/*
 * memory.c - Buffer allocation and memory-region management.
 *
 * A single large buffer is allocated once and pinned (registered) with the
 * NIC exactly once, then split into the main data region and a scratch
 * landing pad used during reduce-scatter. Registering once avoids the high
 * per-operation cost of pinning and unpinning memory.
 */
#include <stdio.h>
#include <stdlib.h>

#include "pg_internal.h"

/* Size of the main data region (the scratch pad is appended after it). */
#define DATA_REGION_BYTES (64 * 1024 * 1024) /* 64 MiB */

/*
 * Allocate the backing buffer, carve it into data/scratch views, and register
 * the whole region for local write and remote read/write access.
 * Returns 0 on success, -1 on failure.
 */
int pg_alloc_and_register(pg_handle *h)
{
    h->data_bytes = DATA_REGION_BYTES;
    size_t buf_size = h->data_bytes + SCRATCH_BYTES;

    h->buf = malloc(buf_size);
    if (!h->buf) {
        perror("malloc buffer");
        return -1;
    }

    h->data_base = (uint8_t *)h->buf;
    h->scratch_base = (uint8_t *)h->buf + h->data_bytes;

    h->mr = ibv_reg_mr(h->pd, h->buf, buf_size,
                       IBV_ACCESS_LOCAL_WRITE |
                       IBV_ACCESS_REMOTE_WRITE |
                       IBV_ACCESS_REMOTE_READ);
    if (!h->mr) {
        perror("ibv_reg_mr");
        return -1;
    }
    return 0;
}

/* Deregister the memory region and free the backing buffer. Idempotent. */
void pg_release_memory(pg_handle *h)
{
    if (h->mr) {
        ibv_dereg_mr(h->mr);
        h->mr = NULL;
    }
    if (h->buf) {
        free(h->buf);
        h->buf = NULL;
    }
}
