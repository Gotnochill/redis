/*
 * aof_iouring.c — io_uring AOF writer using liburing
 *
 * This is the improved version of the Redis AOF flush mechanism.
 * Instead of blocking write() + fsync() calls, we use io_uring's
 * submission/completion queue model:
 *
 *   1. Prepare write + fsync SQEs (Submission Queue Entries)
 *   2. Chain them with IOSQE_IO_LINK so fsync runs after write
 *   3. Submit batches of operations at once
 *   4. Reap completions from the Completion Queue (CQ)
 *
 * Benefits:
 *   - Fewer context switches (single submit for a batch)
 *   - Kernel processes I/O without blocking our thread
 *   - Ring buffer is shared memory — no syscall per operation
 */

#include "aof.h"

#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* How many ops to batch in one io_uring submission.
 * Each op = 1 write SQE + 1 fsync SQE = 2 SQEs per op.
 * We keep this moderate to avoid overflowing the SQ. */
#define BATCH_SIZE  32
#define QUEUE_DEPTH (BATCH_SIZE * 2 + 8)  /* write+fsync pairs + headroom */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

BenchResult aof_iouring_run(int num_ops, size_t op_size) {
    BenchResult result = {0};
    result.num_ops = num_ops;
    result.op_size = op_size;

    /* Create a temporary AOF file */
    char path[] = "/tmp/aof_iouring_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp (iouring)");
        return result;
    }
    unlink(path);

    /* Build payload buffer */
    char *buf = malloc(op_size);
    if (!buf) {
        perror("malloc");
        close(fd);
        return result;
    }
    memset(buf, 'A', op_size);
    buf[op_size - 1] = '\n';

    /* Initialize io_uring */
    struct io_uring ring;
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        free(buf);
        close(fd);
        return result;
    }

    /* === BENCHMARK LOOP === */
    double t_start = now_sec();

    int submitted = 0;
    off_t offset = 0;

    while (submitted < num_ops) {
        /* Determine batch size */
        int batch = num_ops - submitted;
        if (batch > BATCH_SIZE) batch = BATCH_SIZE;

        /* Prepare a batch of write+fsync pairs */
        for (int i = 0; i < batch; i++) {
            struct io_uring_sqe *sqe;

            /* WRITE SQE — chained to the following fsync */
            sqe = io_uring_get_sqe(&ring);
            if (!sqe) {
                fprintf(stderr, "Failed to get SQE for write\n");
                goto done;
            }
            io_uring_prep_write(sqe, fd, buf, op_size, offset);
            sqe->flags |= IOSQE_IO_LINK;  /* chain: fsync after write */
            io_uring_sqe_set_data(sqe, (void *)(intptr_t)(submitted + i));

            /* FSYNC SQE — ensures durability (like Redis appendfsync=always) */
            sqe = io_uring_get_sqe(&ring);
            if (!sqe) {
                fprintf(stderr, "Failed to get SQE for fsync\n");
                goto done;
            }
            io_uring_prep_fsync(sqe, fd, 0);
            io_uring_sqe_set_data(sqe, (void *)(intptr_t)(-(submitted + i + 1)));

            offset += op_size;
        }

        /* Submit the entire batch in one syscall */
        int ret = io_uring_submit(&ring);
        if (ret < 0) {
            fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
            goto done;
        }

        /* Reap all completions for this batch (2 CQEs per op: write + fsync) */
        for (int i = 0; i < batch * 2; i++) {
            struct io_uring_cqe *cqe;
            ret = io_uring_wait_cqe(&ring, &cqe);
            if (ret < 0) {
                fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-ret));
                goto done;
            }
            if (cqe->res < 0) {
                fprintf(stderr, "CQE error: %s\n", strerror(-cqe->res));
            }
            io_uring_cqe_seen(&ring, cqe);
        }

        submitted += batch;
    }

done:;
    double t_end = now_sec();

    /* Calculate metrics */
    double elapsed = t_end - t_start;
    result.total_time_ms  = elapsed * 1000.0;
    result.ops_per_sec    = (elapsed > 0) ? num_ops / elapsed : 0;
    result.avg_latency_us = (elapsed > 0) ? (elapsed / num_ops) * 1e6 : 0;

    io_uring_queue_exit(&ring);
    free(buf);
    close(fd);
    return result;
}
