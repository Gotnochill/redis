/*
 * demo_live.c — Real-time live demonstration for professor
 *
 * Shows operations executing ONE BY ONE with per-operation latency,
 * a live progress bar, and a final side-by-side comparison.
 *
 * Usage:
 *   ./build/demo              # default: 100 ops × 256 bytes
 *   ./build/demo --ops=200    # custom op count
 *   ./build/demo --size=512   # custom payload size
 *
 * What your professor will see:
 *   1. Traditional mode running — each write+fsync with live latency
 *   2. io_uring mode running — batched operations with live latency
 *   3. Final comparison with percentage improvement
 */

#include "aof.h"

#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BATCH_SIZE  32
#define QUEUE_DEPTH (BATCH_SIZE * 2 + 8)

/* ── Helpers ─────────────────────────────────────────────────── */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void print_progress(const char *label, int current, int total,
                           double latency_us, double avg_us) {
    int bar_width = 40;
    int filled = (current * bar_width) / total;

    printf("\r  %s [", label);
    for (int i = 0; i < bar_width; i++)
        printf("%s", i < filled ? "█" : "░");
    printf("] %4d/%-4d | this: %7.1f us | avg: %7.1f us",
           current, total, latency_us, avg_us);
    fflush(stdout);
}

/* ── Traditional: Live per-op ────────────────────────────────── */

static BenchResult demo_traditional(int num_ops, size_t op_size) {
    BenchResult result = { .num_ops = num_ops, .op_size = op_size };

    char path[] = "/tmp/aof_trad_demo_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return result; }
    unlink(path);

    char *buf = malloc(op_size);
    memset(buf, 'S', op_size);  /* S for SET command */
    buf[op_size - 1] = '\n';

    printf("\n  ┌─────────────────────────────────────────────────────┐\n");
    printf("  │  📝 TRADITIONAL MODE: write() + fsync() per op     │\n");
    printf("  │  Each operation = 2 blocking system calls           │\n");
    printf("  └─────────────────────────────────────────────────────┘\n\n");

    double total_latency = 0;
    double t_start = now_sec();

    for (int i = 0; i < num_ops; i++) {
        double op_start = now_sec();

        if (write(fd, buf, op_size) < 0) {
            perror("write");
            break;
        }
        fsync(fd);

        double op_end = now_sec();
        double lat_us = (op_end - op_start) * 1e6;
        total_latency += lat_us;

        /* Show live progress every op (or every few ops if many) */
        if (num_ops <= 200 || i % 5 == 0 || i == num_ops - 1) {
            print_progress("Traditional", i + 1, num_ops,
                           lat_us, total_latency / (i + 1));
        }
    }

    double t_end = now_sec();
    double elapsed = t_end - t_start;

    result.total_time_ms  = elapsed * 1000.0;
    result.ops_per_sec    = num_ops / elapsed;
    result.avg_latency_us = total_latency / num_ops;

    printf("\n  ✓ Done in %.1f ms  (%.0f ops/sec, avg %.1f μs/op)\n",
           result.total_time_ms, result.ops_per_sec, result.avg_latency_us);

    free(buf);
    close(fd);
    return result;
}

/* ── io_uring: Live per-batch ────────────────────────────────── */

static BenchResult demo_iouring(int num_ops, size_t op_size) {
    BenchResult result = { .num_ops = num_ops, .op_size = op_size };

    char path[] = "/tmp/aof_uring_demo_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return result; }
    unlink(path);

    char *buf = malloc(op_size);
    memset(buf, 'S', op_size);
    buf[op_size - 1] = '\n';

    struct io_uring ring;
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        free(buf); close(fd);
        return result;
    }

    printf("\n  ┌─────────────────────────────────────────────────────┐\n");
    printf("  │  🚀 io_uring MODE: batched ring buffer submissions  │\n");
    printf("  │  %d ops batched per submit() call                 │\n", BATCH_SIZE);
    printf("  └─────────────────────────────────────────────────────┘\n\n");

    double total_latency = 0;
    int completed = 0;
    off_t offset = 0;
    double t_start = now_sec();

    while (completed < num_ops) {
        int batch = num_ops - completed;
        if (batch > BATCH_SIZE) batch = BATCH_SIZE;

        double batch_start = now_sec();

        /* Prepare batch of write+fsync pairs */
        for (int i = 0; i < batch; i++) {
            struct io_uring_sqe *sqe;

            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_write(sqe, fd, buf, op_size, offset);
            sqe->flags |= IOSQE_IO_LINK;
            io_uring_sqe_set_data(sqe, (void *)(intptr_t)(completed + i));

            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_fsync(sqe, fd, 0);
            io_uring_sqe_set_data(sqe, (void *)(intptr_t)(-(completed + i + 1)));

            offset += op_size;
        }

        /* One submit for the whole batch */
        io_uring_submit(&ring);

        /* Reap completions */
        for (int i = 0; i < batch * 2; i++) {
            struct io_uring_cqe *cqe;
            io_uring_wait_cqe(&ring, &cqe);
            io_uring_cqe_seen(&ring, cqe);
        }

        double batch_end = now_sec();
        double batch_lat_us = (batch_end - batch_start) * 1e6;
        double per_op_lat = batch_lat_us / batch;
        total_latency += batch_lat_us;
        completed += batch;

        print_progress("io_uring   ", completed, num_ops,
                       per_op_lat, total_latency / completed);
    }

    double t_end = now_sec();
    double elapsed = t_end - t_start;

    result.total_time_ms  = elapsed * 1000.0;
    result.ops_per_sec    = num_ops / elapsed;
    result.avg_latency_us = total_latency / num_ops;

    printf("\n  ✓ Done in %.1f ms  (%.0f ops/sec, avg %.1f μs/op)\n",
           result.total_time_ms, result.ops_per_sec, result.avg_latency_us);

    io_uring_queue_exit(&ring);
    free(buf);
    close(fd);
    return result;
}

/* ── Main ────────────────────────────────────────────────────── */

static int parse_int_arg(const char *arg, const char *prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(arg, prefix, plen) == 0)
        return atoi(arg + plen);
    return -1;
}

int main(int argc, char **argv) {
    int num_ops = 100;
    int op_size = 256;

    for (int i = 1; i < argc; i++) {
        int v;
        if ((v = parse_int_arg(argv[i], "--ops=")) > 0)  num_ops = v;
        if ((v = parse_int_arg(argv[i], "--size=")) > 0)  op_size = v;
    }

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║     🔴 LIVE DEMO: Redis AOF — Traditional vs io_uring    ║\n");
    printf("╠═══════════════════════════════════════════════════════════╣\n");
    printf("║  Operations : %-6d                                     ║\n", num_ops);
    printf("║  Payload    : %-4d bytes (simulated Redis SET command)   ║\n", op_size);
    printf("║  fsync mode : always (worst case, max durability)        ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    /* Phase 1: Traditional */
    printf("\n━━━ Phase 1: Traditional write() + fsync() ━━━━━━━━━━━━━━━\n");
    BenchResult trad = demo_traditional(num_ops, (size_t)op_size);

    /* Pause for dramatic effect */
    printf("\n  ⏳ Starting io_uring in 2 seconds...\n");
    usleep(2000000);

    /* Phase 2: io_uring */
    printf("\n━━━ Phase 2: io_uring batched ring buffers ━━━━━━━━━━━━━━━\n");
    BenchResult uring = demo_iouring(num_ops, (size_t)op_size);

    /* Final comparison */
    double improvement = 0;
    if (trad.ops_per_sec > 0)
        improvement = ((uring.ops_per_sec - trad.ops_per_sec) / trad.ops_per_sec) * 100.0;

    double speedup = (trad.total_time_ms > 0) ? trad.total_time_ms / uring.total_time_ms : 0;

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║                    📊  FINAL RESULTS                     ║\n");
    printf("╠══════════════════╦════════════════╦═══════════════════════╣\n");
    printf("║     Metric       ║  Traditional   ║     io_uring         ║\n");
    printf("╠══════════════════╬════════════════╬═══════════════════════╣\n");
    printf("║  Total time      ║ %9.1f ms   ║ %9.1f ms          ║\n",
           trad.total_time_ms, uring.total_time_ms);
    printf("║  Throughput      ║ %9.0f op/s ║ %9.0f op/s        ║\n",
           trad.ops_per_sec, uring.ops_per_sec);
    printf("║  Avg latency     ║ %9.1f μs   ║ %9.1f μs          ║\n",
           trad.avg_latency_us, uring.avg_latency_us);
    printf("╠══════════════════╩════════════════╩═══════════════════════╣\n");
    printf("║                                                          ║\n");
    printf("║  🚀 io_uring is %.1f× faster  (%+.1f%% throughput)      ║\n",
           speedup, improvement);
    printf("║                                                          ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    return 0;
}
