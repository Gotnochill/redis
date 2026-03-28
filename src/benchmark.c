/*
 * benchmark.c — Main driver for Redis AOF io_uring benchmark
 *
 * Runs both the traditional (write+fsync) and io_uring AOF implementations
 * across multiple configurations, prints a comparison table, and writes
 * results as JSON for the HTML dashboard.
 *
 * Usage:
 *   ./benchmark                    # run default test suite
 *   ./benchmark --ops=5000         # custom operation count
 *   ./benchmark --size=256         # custom payload size
 *   ./benchmark --ops=5000 --size=256
 */

#include "aof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default test configurations: { num_ops, op_size } */
typedef struct {
    int    num_ops;
    size_t op_size;
} TestConfig;

static TestConfig default_configs[] = {
    {  500,   64 },   /* small writes, moderate count   */
    { 1000,   64 },   /* small writes, higher count     */
    { 1000,  256 },   /* medium writes                  */
    { 2000,  256 },   /* medium writes, high count      */
    { 1000, 1024 },   /* larger writes (1 KB)           */
};
static int num_default_configs = sizeof(default_configs) / sizeof(default_configs[0]);

static void print_header(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                  Redis AOF Benchmark: Traditional vs io_uring                          ║\n");
    printf("╠══════════╦═══════╦═══════════════════════════╦═══════════════════════════╦══════════════╣\n");
    printf("║  Ops     ║  Size ║     Traditional           ║       io_uring            ║  Improvement ║\n");
    printf("║          ║ (B)   ║  Time(ms)   Ops/s  Lat(μs)║  Time(ms)   Ops/s  Lat(μs)║              ║\n");
    printf("╠══════════╬═══════╬═══════════════════════════╬═══════════════════════════╬══════════════╣\n");
}

static void print_row(TestConfig *cfg, BenchResult *trad, BenchResult *uring) {
    double improvement = 0;
    if (trad->ops_per_sec > 0)
        improvement = ((uring->ops_per_sec - trad->ops_per_sec) / trad->ops_per_sec) * 100.0;

    printf("║ %8d ║ %5zu ║ %8.1f %7.0f %7.1f ║ %8.1f %7.0f %7.1f ║ %+9.1f%%   ║\n",
           cfg->num_ops, cfg->op_size,
           trad->total_time_ms, trad->ops_per_sec, trad->avg_latency_us,
           uring->total_time_ms, uring->ops_per_sec, uring->avg_latency_us,
           improvement);
}

static void print_footer(void) {
    printf("╚══════════╩═══════╩═══════════════════════════╩═══════════════════════════╩══════════════╝\n");
    printf("\n");
}

/* Write results as JSON for the dashboard */
static void write_json(TestConfig *configs, BenchResult *trad, BenchResult *uring, int n) {
    FILE *fp = fopen("results.json", "w");
    if (!fp) {
        perror("Failed to open results.json");
        return;
    }

    fprintf(fp, "{\n  \"benchmark\": \"Redis AOF: Traditional write()+fsync() vs io_uring\",\n");
    fprintf(fp, "  \"results\": [\n");

    for (int i = 0; i < n; i++) {
        double improvement = 0;
        if (trad[i].ops_per_sec > 0)
            improvement = ((uring[i].ops_per_sec - trad[i].ops_per_sec) / trad[i].ops_per_sec) * 100.0;

        fprintf(fp, "    {\n");
        fprintf(fp, "      \"label\": \"%d ops × %zu B\",\n", configs[i].num_ops, configs[i].op_size);
        fprintf(fp, "      \"num_ops\": %d,\n", configs[i].num_ops);
        fprintf(fp, "      \"op_size\": %zu,\n", configs[i].op_size);
        fprintf(fp, "      \"traditional\": {\n");
        fprintf(fp, "        \"total_time_ms\": %.2f,\n", trad[i].total_time_ms);
        fprintf(fp, "        \"ops_per_sec\": %.2f,\n", trad[i].ops_per_sec);
        fprintf(fp, "        \"avg_latency_us\": %.2f\n", trad[i].avg_latency_us);
        fprintf(fp, "      },\n");
        fprintf(fp, "      \"iouring\": {\n");
        fprintf(fp, "        \"total_time_ms\": %.2f,\n", uring[i].total_time_ms);
        fprintf(fp, "        \"ops_per_sec\": %.2f,\n", uring[i].ops_per_sec);
        fprintf(fp, "        \"avg_latency_us\": %.2f\n", uring[i].avg_latency_us);
        fprintf(fp, "      },\n");
        fprintf(fp, "      \"improvement_pct\": %.2f\n", improvement);
        fprintf(fp, "    }%s\n", (i < n - 1) ? "," : "");
    }

    fprintf(fp, "  ]\n}\n");
    fclose(fp);
    printf("📊 Results written to results.json\n");
}

static int parse_int_arg(const char *arg, const char *prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(arg, prefix, plen) == 0) {
        return atoi(arg + plen);
    }
    return -1;
}

int main(int argc, char **argv) {
    int custom_ops  = -1;
    int custom_size = -1;

    /* Parse CLI args */
    for (int i = 1; i < argc; i++) {
        int v;
        if ((v = parse_int_arg(argv[i], "--ops=")) > 0)  custom_ops = v;
        if ((v = parse_int_arg(argv[i], "--size=")) > 0)  custom_size = v;
    }

    TestConfig *configs;
    int n_configs;
    TestConfig custom_cfg;

    if (custom_ops > 0 || custom_size > 0) {
        /* Use a single custom config */
        custom_cfg.num_ops = (custom_ops > 0)  ? custom_ops  : 1000;
        custom_cfg.op_size = (custom_size > 0)  ? (size_t)custom_size : 64;
        configs   = &custom_cfg;
        n_configs = 1;
    } else {
        configs   = default_configs;
        n_configs = num_default_configs;
    }

    /* Allocate results */
    BenchResult *trad_results  = calloc(n_configs, sizeof(BenchResult));
    BenchResult *uring_results = calloc(n_configs, sizeof(BenchResult));

    printf("\n🔧 Running Redis AOF benchmark...\n");
    printf("   Each test: write() + fsync() per operation (appendfsync=always)\n\n");

    /* Run benchmarks */
    for (int i = 0; i < n_configs; i++) {
        printf("  ▶ Test %d/%d: %d ops × %zu bytes\n",
               i + 1, n_configs, configs[i].num_ops, configs[i].op_size);

        printf("    → Traditional (write + fsync)...\n");
        trad_results[i] = aof_traditional_run(configs[i].num_ops, configs[i].op_size);

        printf("    → io_uring (batched SQEs)...\n");
        uring_results[i] = aof_iouring_run(configs[i].num_ops, configs[i].op_size);

        printf("    ✓ Done\n");
    }

    /* Print results table */
    print_header();
    for (int i = 0; i < n_configs; i++) {
        print_row(&configs[i], &trad_results[i], &uring_results[i]);
    }
    print_footer();

    /* Write JSON for dashboard */
    write_json(configs, trad_results, uring_results, n_configs);

    printf("🌐 Open dashboard.html in a browser to visualize results.\n\n");

    free(trad_results);
    free(uring_results);
    return 0;
}
