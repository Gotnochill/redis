/*
 * aof_traditional.c — Traditional AOF writer using write() + fsync()
 *
 * This simulates how Redis's flushAppendOnlyFile() works when
 * appendfsync=always: each command append triggers a write() to move
 * data from userspace into the kernel page cache, followed by fsync()
 * to force it to stable storage.
 *
 * Both write() and fsync() are BLOCKING system calls that cause a
 * context switch from userspace → kernel → userspace each time.
 */

#include "aof.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Get wall-clock time in seconds (monotonic) */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

BenchResult aof_traditional_run(int num_ops, size_t op_size) {
    BenchResult result = {0};
    result.num_ops = num_ops;
    result.op_size = op_size;

    /* Create a temporary AOF file */
    char path[] = "/tmp/aof_traditional_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp (traditional)");
        return result;
    }
    /* Remove on exit so we don't leave junk around */
    unlink(path);

    /* Build a payload buffer (simulates a Redis command serialization) */
    char *buf = malloc(op_size);
    if (!buf) {
        perror("malloc");
        close(fd);
        return result;
    }
    /* Fill with a recognisable pattern: "*3\r\n$3\r\nSET\r\n..." style */
    memset(buf, 'A', op_size);
    buf[op_size - 1] = '\n';

    /* === BENCHMARK LOOP === */
    double t_start = now_sec();

    for (int i = 0; i < num_ops; i++) {
        /* 1. write() — userspace → kernel page cache */
        ssize_t written = write(fd, buf, op_size);
        if (written < 0) {
            perror("write");
            break;
        }

        /* 2. fsync() — kernel page cache → stable storage */
        if (fsync(fd) < 0) {
            perror("fsync");
            break;
        }
    }

    double t_end = now_sec();

    /* Calculate metrics */
    double elapsed = t_end - t_start;
    result.total_time_ms  = elapsed * 1000.0;
    result.ops_per_sec    = (elapsed > 0) ? num_ops / elapsed : 0;
    result.avg_latency_us = (elapsed > 0) ? (elapsed / num_ops) * 1e6 : 0;

    free(buf);
    close(fd);
    return result;
}
