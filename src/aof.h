/*
 * aof.h — Common header for Redis AOF io_uring benchmark
 *
 * Defines the shared BenchResult struct and function declarations
 * for both traditional and io_uring AOF implementations.
 */

#ifndef AOF_H
#define AOF_H

#include <stddef.h>
#include <stdint.h>

/* Result of a single benchmark run */
typedef struct {
    int      num_ops;          /* number of append operations performed   */
    size_t   op_size;          /* size of each append payload in bytes    */
    double   total_time_ms;    /* wall-clock time in milliseconds         */
    double   ops_per_sec;      /* throughput: num_ops / total_time_sec    */
    double   avg_latency_us;   /* average per-op latency in microseconds  */
} BenchResult;

/*
 * Run the traditional AOF benchmark:
 *   - Opens a temp file
 *   - Appends `num_ops` entries of `op_size` bytes each
 *   - Each append does write() + fsync()  (appendfsync=always semantics)
 *   - Returns timing results in a BenchResult
 */
BenchResult aof_traditional_run(int num_ops, size_t op_size);

/*
 * Run the io_uring AOF benchmark:
 *   - Same workload as traditional
 *   - Uses io_uring_prep_write + io_uring_prep_fsync with batching
 *   - Returns timing results in a BenchResult
 */
BenchResult aof_iouring_run(int num_ops, size_t op_size);

#endif /* AOF_H */
