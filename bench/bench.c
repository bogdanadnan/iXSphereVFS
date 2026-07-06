/* IXSphere VFS Benchmark — multi-threaded CLI benchmark tool.
 * Usage: vfs_bench --workload=<name> --count=N [--threads=N] [--page-size=N]
 *                   [--cache-mb=N] [--output=<path>]
 *
 * Workloads:
 *   create   — create N files under root
 *   write    — create N files, write data to each
 *   read     — open N files, read data
 *   scan     — sequential full-file read from byte 0 to end, measure bandwidth
 *   mixed    — configurable read/write ratio on random offsets over pre-populated files
 *   dir      — create/list/delete cycles on N subdirectories, measure directory-operation throughput
 *   seqwrite — sequential page writes to a single large file
 *   randread — random page reads from a pre-populated file
 */

/* Simple fail-on-error macro for benchmark setup */
#define BENCH_CHECK(expr) do { \
    if (!(expr)) { fprintf(stderr, "FATAL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); return 0; } \
} while(0)

#include "ixsphere/vfs.h"
#include "ixsphere/vfs_internal.h"
#include "nodes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * CLI argument parsing
 * --------------------------------------------------------------------------- */

typedef struct {
    const char* workload;   /* workload name */
    int         count;      /* number of files/operations */
    int         threads;    /* number of concurrent threads */
    int         page_size;  /* VFS page size (default 8192) */
    int         cache_mb;   /* 0 = use default */
    int         read_ratio; /* for mixed workload, default 80 */
    const char* output;     /* VFS file path */
    const char* csv_path;   /* CSV output path (NULL = no CSV) */
} bench_opts;

static bench_opts defaults = {
    .workload  = "create",
    .count     = 1000,
    .threads   = 1,
    .page_size = 8192,
    .cache_mb  = 0,
    .read_ratio = 80,
    .output    = "/tmp/vfs_bench.vfs",
    .csv_path  = NULL,
};

static void usage(void) {
    fprintf(stderr,
        "Usage: vfs_bench --workload=<name> --count=N [options]\n"
        "Options:\n"
        "  --workload=<name>   create, small_create, write, read, scan, mixed, dir, seqwrite, randread, seqread, sparse_rand_write (default: create)\n"
        "  --count=N           number of files/operations (default: 1000)\n"
        "  --threads=N         number of threads (default: 1)\n"
        "  --page-size=N       VFS page size in bytes (default: 8192)\n"
        "  --cache-mb=N        page cache size in MB (default: 0 = auto)\n"
        "  --read-ratio=N      mixed workload: %% reads (default: 80)\n"
        "  --output=<path>     VFS file path (default: /tmp/vfs_bench.vfs)\n"
        "  --csv=<path>        CSV output file path (optional)\n"
    );
}

static int parse_int(const char* arg, const char* prefix, int* out) {
    size_t plen = strlen(prefix);
    if (strncmp(arg, prefix, plen) == 0) {
        *out = atoi(arg + plen);
        return 1;
    }
    return 0;
}

static int parse_str(const char* arg, const char* prefix, const char** out) {
    size_t plen = strlen(prefix);
    if (strncmp(arg, prefix, plen) == 0) {
        *out = arg + plen;
        return 1;
    }
    return 0;
}

static int parse_args(int argc, char** argv, bench_opts* opts) {
    *opts = defaults;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
        if (parse_str(argv[i], "--workload=", &opts->workload)) continue;
        if (parse_int(argv[i], "--count=", &opts->count)) continue;
        if (parse_int(argv[i], "--threads=", &opts->threads)) continue;
        if (parse_int(argv[i], "--page-size=", &opts->page_size)) continue;
        if (parse_int(argv[i], "--cache-mb=", &opts->cache_mb)) continue;
        if (parse_int(argv[i], "--read-ratio=", &opts->read_ratio)) continue;
        if (parse_str(argv[i], "--output=", &opts->output)) continue;
        if (parse_str(argv[i], "--csv=", &opts->csv_path)) continue;
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        usage();
        return -1;
    }
    if (opts->count <= 0 || opts->threads <= 0 || opts->page_size <= 0) {
        fprintf(stderr, "count, threads, and page-size must be positive\n");
        return -1;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Timing helpers
 * --------------------------------------------------------------------------- */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------------------
 * Multi-threaded benchmark infrastructure
 * --------------------------------------------------------------------------- */

/* Portable barrier using mutex + condvar (macOS lacks pthread_barrier_t) */
typedef struct {
    int              count;
    int              waiting;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
} bench_barrier_t;

static void bench_barrier_init(bench_barrier_t* b, int count) {
    b->count = count;
    b->waiting = 0;
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->cond, NULL);
}

static void bench_barrier_wait(bench_barrier_t* b) {
    pthread_mutex_lock(&b->mutex);
    b->waiting++;
    if (b->waiting >= b->count) {
        b->waiting = 0;
        pthread_cond_broadcast(&b->cond);
    } else {
        pthread_cond_wait(&b->cond, &b->mutex);
    }
    pthread_mutex_unlock(&b->mutex);
}

static void bench_barrier_destroy(bench_barrier_t* b) {
    pthread_mutex_destroy(&b->mutex);
    pthread_cond_destroy(&b->cond);
}

typedef struct {
    vfs_t*    vfs;
    int       tid;
    int       count;
    int       ok;
    double    elapsed;              /* per-thread elapsed time (set by worker) */
    bench_barrier_t* barrier;       /* startup barrier, NULL when not used */
} bench_thread_ctx;

static void* bench_write_worker(void* arg) {
    bench_thread_ctx* c = (bench_thread_ctx*)arg;
    if (c->barrier) bench_barrier_wait(c->barrier);
    double t0 = now_sec();
    int64_t root_vp = c->vfs->ctx->rootNodeOffset;
    for (int i = 0; i < c->count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "t%d_w%d.txt", c->tid, i);
        int64_t file_vp = vfs_create(c->vfs, root_vp, name, 0);
        if (file_vp <= 0) continue;
        char data[128];
        memset(data, 'A' + c->tid, 128);
        if (vfs_write(c->vfs, file_vp, data, 0, 128, 0) > 0) c->ok++;
    }
    c->elapsed = now_sec() - t0;
    return NULL;
}

static void* bench_seqwrite_worker(void* arg) {
    bench_thread_ctx* c = (bench_thread_ctx*)arg;
    if (c->barrier) bench_barrier_wait(c->barrier);
    double t0 = now_sec();
    int64_t root_vp = c->vfs->ctx->rootNodeOffset;
    const int page_sz = c->vfs->ctx->page_size;
    char fname[64];
    snprintf(fname, sizeof(fname), "seqwrite_t%d.dat", c->tid);
    int64_t fvp = vfs_create(c->vfs, root_vp, fname, 0);
    if (fvp <= 0) return NULL;
    uint8_t* buf = calloc(1, (size_t)page_sz);
    if (!buf) return NULL;
    for (int i = 0; i < c->count; i++) {
        if (vfs_write(c->vfs, fvp, buf, (int64_t)i * page_sz, page_sz, 0) == page_sz) c->ok++;
    }
    free(buf);
    c->elapsed = now_sec() - t0;
    return NULL;
}

static int bench_run_threads(vfs_t* vfs, int thread_count, int ops_per_thread,
                              void* (*worker)(void*)) {
    pthread_t* th = malloc((size_t)thread_count * sizeof(pthread_t));
    bench_thread_ctx* ctx = calloc((size_t)thread_count, sizeof(bench_thread_ctx));

    bench_barrier_t barrier;
    bench_barrier_init(&barrier, thread_count);

    for (int i = 0; i < thread_count; i++) {
        ctx[i].vfs = vfs;
        ctx[i].tid = i;
        ctx[i].count = ops_per_thread;
        ctx[i].ok = 0;
        ctx[i].elapsed = 0.0;
        ctx[i].barrier = &barrier;
        pthread_create(&th[i], NULL, worker, &ctx[i]);
    }

    double total_elapsed = 0.0;
    double min_elapsed = 1e9;
    double max_elapsed = 0.0;
    int total = 0;
    for (int i = 0; i < thread_count; i++) {
        pthread_join(th[i], NULL);
        total += ctx[i].ok;
        total_elapsed += ctx[i].elapsed;
        if (ctx[i].elapsed < min_elapsed) min_elapsed = ctx[i].elapsed;
        if (ctx[i].elapsed > max_elapsed) max_elapsed = ctx[i].elapsed;
    }

    bench_barrier_destroy(&barrier);

    printf("  threads=%d  ops=%d  avg_time=%.4fs  min=%.4fs  max=%.4fs\n",
           thread_count, total,
           total_elapsed / (double)thread_count,
           min_elapsed, max_elapsed);

    free(ctx);
    free(th);
    return total;
}

static void report(const char* name, int count, int threads, double elapsed) {
    double ops_per_sec = (double)count / elapsed;
    printf("%-8s  count=%d  threads=%d  elapsed=%.3fs  ops/sec=%.0f\n",
           name, count, threads, elapsed, ops_per_sec);
}

/* ---------------------------------------------------------------------------
 * Per-operation latency recording and percentile computation.
 * --------------------------------------------------------------------------- */

typedef struct {
    double* latencies;
    int     capacity;
    int     count;
} bench_latency;

static bench_latency g_lat = {NULL, 0, 0};

static void lat_init(int max_ops) {
    g_lat.count = 0;
    g_lat.capacity = max_ops > 0 ? max_ops : 1024;
    g_lat.latencies = (double*)calloc((size_t)g_lat.capacity, sizeof(double));
}

static void lat_record(double sec) {
    if (g_lat.count < g_lat.capacity)
        g_lat.latencies[g_lat.count++] = sec;
}

static int lat_cmp(const void* a, const void* b) {
    double da = *(const double*)a;
    double db = *(const double*)b;
    return (da > db) - (da < db);
}

static double lat_percentile(double p) {
    if (g_lat.count == 0) return 0.0;
    qsort(g_lat.latencies, (size_t)g_lat.count, sizeof(double), lat_cmp);
    int idx = (int)((p / 100.0) * (double)g_lat.count);
    if (idx >= g_lat.count) idx = g_lat.count - 1;
    if (idx < 0) idx = 0;
    return g_lat.latencies[idx];
}

static void lat_destroy(void) {
    free(g_lat.latencies);
    g_lat.latencies = NULL;
    g_lat.count = 0;
    g_lat.capacity = 0;
}

/* ---------------------------------------------------------------------------
 * Enhanced report with latencies and cache stats.
 * --------------------------------------------------------------------------- */

static void report_full(const char* name, int count, int threads, double elapsed) {
    printf("\n=== %s ===\n", name);
    printf("  count=%d  threads=%d  elapsed=%.3fs\n", count, threads, elapsed);
    double ops = (double)count / elapsed;
    printf("  ops/sec: %.0f\n", ops);

    if (g_lat.count > 0) {
        double p50 = lat_percentile(50.0);
        double p95 = lat_percentile(95.0);
        double p99 = lat_percentile(99.0);
        printf("  latency (usec): p50=%.0f  p95=%.0f  p99=%.0f\n",
               p50 * 1e6, p95 * 1e6, p99 * 1e6);
    }

    int64_t ct = vfs_cache_total();
    int64_t ch = vfs_cache_hits();
    int64_t dt = vfs_data_total();
    int64_t dh = vfs_data_hits();
    double ratio = (ct > 0) ? (double)ch / (double)ct : 0.0;
    double dratio = (dt > 0) ? (double)dh / (double)dt : 0.0;
    printf("  cache: hits=%lld  total=%lld  ratio=%.1f%%\n",
           (long long)ch, (long long)ct, ratio * 100.0);
    printf("  data:  hits=%lld  total=%lld  ratio=%.1f%%\n",
           (long long)dh, (long long)dt, dratio * 100.0);
    printf("\n");
}

/* ---------------------------------------------------------------------------
 * CSV output — write a result row to an append-only CSV file.
 * Writes a header line on first open (empty file).
 * --------------------------------------------------------------------------- */

static void write_csv(const char* csv_path, const char* workload,
                       int count, int threads, int page_size, int cache_mb,
                       int read_ratio, int elapsed_us, double ops_per_sec,
                       double cache_hit_pct, double data_hit_pct,
                       int completed, int total) {
    if (!csv_path) return;
    int needs_header = 0;
    FILE* f = fopen(csv_path, "r");
    if (!f) {
        needs_header = 1;
    } else {
        if (ftell(f) == 0) needs_header = 1;
        fclose(f);
    }
    f = fopen(csv_path, "a");
    if (!f) {
        fprintf(stderr, "warning: cannot open CSV file: %s\n", csv_path);
        return;
    }
    if (needs_header) {
        fprintf(f, "workload,count,threads,page_size,cache_mb,read_ratio,"
                   "elapsed_us,ops_per_sec,cache_hit_pct,data_hit_pct,"
                   "completed,total\n");
    }
    fprintf(f, "%s,%d,%d,%d,%d,%d,%d,%.1f,%.1f,%.1f,%d,%d\n",
            workload, count, threads, page_size, cache_mb, read_ratio,
            elapsed_us, ops_per_sec, cache_hit_pct, data_hit_pct,
            completed, total);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * Workload: create N files
 * --------------------------------------------------------------------------- */

static int bench_create(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    lat_init(count);
    double t0 = now_sec();
    int ok = 0;
    for (int i = 0; i < count; i++) {
        double op_t0 = now_sec();
        char name[64];
        snprintf(name, sizeof(name), "f%d.txt", i);
        int64_t ret = vfs_create(vfs, root_vp, name, 0);
        if (ret > 0) ok++;
        lat_record(now_sec() - op_t0);
    }
    double t1 = now_sec();
    report_full("create", ok, threads, t1 - t0);
    lat_destroy();
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: small file create — create + 128-byte write, measure latency
 * --------------------------------------------------------------------------- */

static int bench_small_file_create(vfs_t* vfs, int count, int threads,
                                   const char* path) {
    (void)path;
    (void)threads;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    lat_init(count);
    double t0 = now_sec();
    int ok = 0;
    char data[128];
    memset(data, 'A', sizeof(data));
    for (int i = 0; i < count; i++) {
        double op_t0 = now_sec();
        char name[64];
        snprintf(name, sizeof(name), "s%d.txt", i);
        int64_t file_vp = vfs_create(vfs, root_vp, name, 0);
        if (file_vp <= 0) continue;
        int written = vfs_write(vfs, file_vp, data, 0, sizeof(data), 0);
        if (written == (int)sizeof(data)) ok++;
        lat_record(now_sec() - op_t0);
    }
    double t1 = now_sec();
    report_full("small_create", ok, threads, t1 - t0);
    lat_destroy();
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: sparse random writes — pre-create file with count pages,
 * then random 128B writes to random pages in 0..count-1, measuring
 * sparse chain performance with optional multi-threading
 * --------------------------------------------------------------------------- */

static void* bench_sparse_rand_worker(void* arg) {
    bench_thread_ctx* c = (bench_thread_ctx*)arg;
    if (c->barrier) bench_barrier_wait(c->barrier);
    double t0 = now_sec();
    const int page_sz = c->vfs->ctx->page_size;
    int64_t root_vp = c->vfs->ctx->rootNodeOffset;
    char fname[64];
    snprintf(fname, sizeof(fname), "sparse_t%d.dat", c->tid);
    int64_t fvp = vfs_create(c->vfs, root_vp, fname, 0);
    if (fvp <= 0) return NULL;
    /* Pre-populate c->count pages (untimed) */
    uint8_t* zbuf = calloc(1, (size_t)page_sz);
    if (!zbuf) return NULL;
    for (int i = 0; i < c->count; i++)
        vfs_write(c->vfs, fvp, zbuf, (int64_t)i * page_sz, page_sz, 0);
    free(zbuf);
    /* Timed random writes */
    char data[128];
    memset(data, 'X', sizeof(data));
    unsigned rseed = 42 + c->tid;
    for (int i = 0; i < c->count; i++) {
        int64_t page = (int64_t)(rand_r(&rseed) % c->vfs->ctx->segment_size);
        int written = vfs_write(c->vfs, fvp, data, page * page_sz, sizeof(data), 0);
        if (written == (int)sizeof(data)) c->ok++;
    }
    c->elapsed = now_sec() - t0;
    return NULL;
}

static int bench_sparse_random_writes(vfs_t* vfs, int count, int threads,
                                      const char* path) {
    (void)path;
    if (threads <= 1) {
        const int page_sz = vfs->ctx->page_size;
        int64_t root_vp = vfs->ctx->rootNodeOffset;
        int64_t file_vp = vfs_create(vfs, root_vp, "sparse_rand.dat", 0);
        if (file_vp <= 0) return 0;

        /* Pre-populate count pages (untimed) */
        uint8_t* zbuf = calloc(1, (size_t)page_sz);
        if (!zbuf) return 0;
        for (int i = 0; i < count; i++)
            vfs_write(vfs, file_vp, zbuf, (int64_t)i * page_sz, page_sz, 0);
        free(zbuf);
        vfs_cache_reset();

        lat_init(count);
        char data[128];
        memset(data, 'X', sizeof(data));
        unsigned rseed = 42;
        double t0 = now_sec();
        int ok = 0;
        for (int i = 0; i < count; i++) {
            double op_t0 = now_sec();
            int64_t page = (int64_t)(rand_r(&rseed) % vfs->ctx->segment_size);
            int written = vfs_write(vfs, file_vp, data, page * page_sz, sizeof(data), 0);
            if (written == (int)sizeof(data)) ok++;
            lat_record(now_sec() - op_t0);
        }
        double t1 = now_sec();
        report_full("sparse_rand_write", ok, threads, t1 - t0);
        lat_destroy();
        return ok;
    }
    /* Multi-threaded: each thread writes to its own file */
    double t0 = now_sec();
    int ops = count / threads;
    int total = bench_run_threads(vfs, threads, ops, bench_sparse_rand_worker);
    double t1 = now_sec();
    report("sparse_rand_write", total, threads, t1 - t0);
    return total;
}

/* ---------------------------------------------------------------------------
 * Workload: write N files
 * --------------------------------------------------------------------------- */

static int bench_write(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    if (threads <= 1) {
        int64_t root_vp = vfs->ctx->rootNodeOffset;
        double t0 = now_sec();
        int ok = 0;
        for (int i = 0; i < count; i++) {
            char name[64];
            snprintf(name, sizeof(name), "w%d.txt", i);
            int64_t file_vp = vfs_create(vfs, root_vp, name, 0);
            if (file_vp <= 0) continue;
            char data[128];
            memset(data, 'A' + (i % 26), sizeof(data));
            int written = vfs_write(vfs, file_vp, data, 0, sizeof(data), 0);
            if (written > 0) ok++;
        }
        double t1 = now_sec();
        report("write", ok, threads, t1 - t0);
        return ok;
    }
    double t0 = now_sec();
    int ops = count / threads;
    int total = bench_run_threads(vfs,
                                   threads, ops, bench_write_worker);
    double t1 = now_sec();
    report("write", total, threads, t1 - t0);
    return total;
}

/* ---------------------------------------------------------------------------
 * Workload: read N files
 * --------------------------------------------------------------------------- */

static int bench_read(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    int64_t* file_vps = (int64_t*)calloc((size_t)count, sizeof(int64_t));
    if (!file_vps) return 0;

    for (int i = 0; i < count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "r%d.txt", i);
        int64_t file_vp = vfs_create(vfs, root_vp, name, 0);
        file_vps[i] = file_vp;

        /* Write 128 bytes to each file so reads exercise version chain traversal */
        if (file_vps[i] > 0) {
            char data[128];
            memset(data, 'A' + (i % 26), 128);
            vfs_write(vfs, file_vps[i], data, 0, 128, 0);
        }
    }

    double t0 = now_sec();
    int ok = 0;
    for (int i = 0; i < count; i++) {
        if (file_vps[i] <= 0) continue;
        char buf[128];
        int r = vfs_read(vfs, file_vps[i], buf, 0, sizeof(buf), 0);
        if (r > 0) ok++;
    }
    double t1 = now_sec();
    free(file_vps);
    report("read", ok, threads, t1 - t0);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: scan — sequential full-file read from byte 0 to end.
 * Pre-populates a single file with `count` pages, then reads it back
 * sequentially in page-sized chunks, measuring bandwidth (MB/s).
 * --------------------------------------------------------------------------- */

static int bench_scan(vfs_t* vfs, int count, int threads, const char* path) {
    (void)threads;
    (void)path;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    const int page_sz = vfs->ctx->page_size;

    /* ── Pre-populate a single file with count pages (untimed) ── */
    int64_t fvp = vfs_create(vfs, root_vp, "scanfile.dat", 0);
    if (fvp <= 0) return 0;

    uint8_t* zbuf = (uint8_t*)calloc(1, (size_t)page_sz);
    if (!zbuf) return 0;
    for (int i = 0; i < count; i++) {
        if (vfs_write(vfs, fvp, zbuf, (int64_t)i * page_sz, page_sz, 0) != page_sz) {
            free(zbuf);
            return 0;
        }
    }
    free(zbuf);

    /* ── Timed: sequential read from byte 0 to end in page-sized chunks ── */
    uint8_t* buf = (uint8_t*)malloc((size_t)page_sz);
    if (!buf) return 0;

    vfs_cache_reset();
    double t0 = now_sec();
    int ok = 0;
    for (int i = 0; i < count; i++) {
        int r = vfs_read(vfs, fvp, buf, (int64_t)i * page_sz, page_sz, 0);
        if (r == page_sz) ok++;
    }
    double elapsed = now_sec() - t0;
    free(buf);

    /* ── Report results with bandwidth ── */
    int64_t total_bytes = (int64_t)count * page_sz;
    double mb = (double)total_bytes / (1024.0 * 1024.0);
    printf("\n=== scan ===\n");
    printf("  pages=%d  size=%.1f MB  elapsed=%.3fs\n", count, mb, elapsed);
    printf("  throughput: %.1f MB/s\n", mb / elapsed);
    printf("  ops/sec: %.0f\n", (double)count / elapsed);
    int64_t ct = vfs_cache_total();
    int64_t ch = vfs_cache_hits();
    int64_t dt = vfs_data_total();
    int64_t dh = vfs_data_hits();
    printf("  cache: hits=%lld total=%lld ratio=%.1f%%\n",
           (long long)ch, (long long)ct, (ct > 0 ? 100.0 * ch / ct : 0));
    printf("  data:  hits=%lld total=%lld ratio=%.1f%%\n",
           (long long)dh, (long long)dt, (dt > 0 ? 100.0 * dh / dt : 0));
    printf("\n");
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: mixed — configurable read/write ratio on pre-populated files.
 * Pre-populates up to 1000 files with page-sized data (4 pages each), then
 * runs `count` operations with --read-ratio (default 80) controlling the
 * read vs write mix at random offsets.  Reports per-operation latency and
 * cache/data hit statistics.
 * --------------------------------------------------------------------------- */

static int bench_mixed(vfs_t* vfs, int count, int threads, const char* path,
                        int read_ratio) {
    (void)path;
    (void)threads;
    if (read_ratio < 0) read_ratio = 80;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    const int page_sz = vfs->ctx->page_size;
    int nfiles = count > 1000 ? 1000 : count;
    int64_t* file_vps = (int64_t*)calloc((size_t)nfiles, sizeof(int64_t));

    /* Pre-populate nfiles with data */
    int populated = 0;
    for (int i = 0; i < nfiles && populated < nfiles; i++) {
        char name[64];
        snprintf(name, sizeof(name), "m%d.txt", i);
        file_vps[populated] = vfs_create(vfs, root_vp, name, 0);
        if (file_vps[populated] <= 0) continue;
        /* Write 4 pages per file */
        uint8_t* zbuf = (uint8_t*)calloc(1, (size_t)page_sz);
        if (!zbuf) break;
        for (int p = 0; p < 4; p++)
            vfs_write(vfs, file_vps[populated], zbuf, (int64_t)p * page_sz, page_sz, 0);
        free(zbuf);
        populated++;
    }
    if (populated == 0) { free(file_vps); return 0; }

    lat_init(count);
    vfs_cache_reset();
    double t0 = now_sec();
    int ok = 0;
    unsigned rseed = 42;
    uint8_t* buf = (uint8_t*)malloc((size_t)page_sz);
    if (!buf) { free(file_vps); return 0; }
    memset(buf, 'Z', (size_t)page_sz);

    for (int i = 0; i < count; i++) {
        double op_t0 = now_sec();
        int fi = rand_r(&rseed) % populated;
        if (file_vps[fi] <= 0) continue;
        int is_read = (rand_r(&rseed) % 100) < read_ratio;
        int64_t off = (int64_t)(rand_r(&rseed) % 4) * page_sz;
        if (is_read) {
            int r = vfs_read(vfs, file_vps[fi], buf, off, page_sz, 0);
            if (r == page_sz) ok++;
        } else {
            int w = vfs_write(vfs, file_vps[fi], buf, off, page_sz, 0);
            if (w == page_sz) ok++;
        }
        lat_record(now_sec() - op_t0);
    }
    double t1 = now_sec();
    free(buf);
    free(file_vps);
    report_full("mixed", ok, 1, t1 - t0);
    lat_destroy();
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: dir — create/list/delete cycles on N subdirectories.
 * Each cycle: mkdir → create 5 files → readdir → delete 5 files → rmdir.
 * Reports per-operation latency, cache/data hit statistics, and overall
 * directory-operation throughput.
 * --------------------------------------------------------------------------- */

static int bench_dir(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    (void)threads;
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    lat_init(count * 13);
    vfs_cache_reset();
    double t0 = now_sec();
    int ok = 0;

    for (int i = 0; i < count; i++) {
        double op_t0 = now_sec();
        char dname[64];
        snprintf(dname, sizeof(dname), "d%d", i);

        /* mkdir */
        int64_t dir_vp = vfs_mkdir(vfs, root_vp, dname, 0);
        if (dir_vp <= 0) continue;
        ok++;
        lat_record(now_sec() - op_t0);

        /* create 5 files */
        int created = 0;
        for (int j = 0; j < 5; j++) {
            char fname[64];
            snprintf(fname, sizeof(fname), "f%d.txt", j);
            if (vfs_create(vfs, dir_vp, fname, 0) > 0) created++;
        }
        ok += created;

        /* readdir */
        vfs_dirent_t ents[16];
        int n = vfs_readdir(vfs, dir_vp, ents, 16, 0);
        if (n == created) ok++;

        /* delete files */
        for (int j = 0; j < 5; j++) {
            char fname[64];
            snprintf(fname, sizeof(fname), "f%d.txt", j);
            if (vfs_delete(vfs, dir_vp, fname, 0) == VFS_OK) ok++;
        }

        /* rmdir */
        if (vfs_rmdir(vfs, root_vp, dname, 0) == VFS_OK) ok++;
    }
    double t1 = now_sec();
    report_full("dir", ok, 1, t1 - t0);
    lat_destroy();
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: seqwrite — sequential page-sized writes to a single file.
 * Phase 1 (untimed): pre-allocate all pages.
 * Phase 2 (timed): overwrite pages, measuring per-page latency.
 * --------------------------------------------------------------------------- */

static int bench_seqwrite(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    if (threads <= 1) {
        int64_t root_vp = vfs->ctx->rootNodeOffset;
        const int page_sz = vfs->ctx->page_size;
        int64_t file_vp = vfs_create(vfs, root_vp, "seqwrite.dat", 0);
        if (file_vp <= 0) return 0;
        uint8_t* buf = (uint8_t*)malloc((size_t)page_sz);
        if (!buf) return 0;
        memset(buf, 0, (size_t)page_sz);
        for (int i = 0; i < count; i++)
            vfs_write(vfs, file_vp, buf, (int64_t)i * page_sz, page_sz, 0);
        memset(buf, 'W', (size_t)page_sz);
        lat_init(count);
        vfs_cache_reset();
        double t0 = now_sec();
        int ok = 0;
        for (int i = 0; i < count; i++) {
            double op_t0 = now_sec();
            int written = vfs_write(vfs, file_vp, buf, (int64_t)i * page_sz, page_sz, 0);
            if (written == page_sz) ok++;
            lat_record(now_sec() - op_t0);
        }
        double t1 = now_sec();
        free(buf);
        report_full("seqwrite", ok, threads, t1 - t0);
        lat_destroy();
        return ok;
    }
    /* Multi-threaded: each thread writes to its own file */
    double t0 = now_sec();
    int ops = count / threads;
    int total = bench_run_threads(vfs,
                                   threads, ops, bench_seqwrite_worker);
    double t1 = now_sec();
    report("seqwrite", total, threads, t1 - t0);
    return total;
}

/* ---------------------------------------------------------------------------
 * Workload: randread — random page reads from a single pre-populated file.
 *
 * The file is sized at exactly 2× the cache capacity so the cache can only
 * hold half the file, producing a measurable mix of cache hits and misses.
 *
 * Cold-cache behavior: the file is fully written, then closed and reopened,
 * clearing all in-memory cached pages (the VFS is not persisted between
 * mount/unmount, so all pages are cold on the second open).
 *
 * Each read picks a truly random page from [0, file_pages), independent of
 * previous reads.  Data-cache hit tracking reports how many page reads
 * were served from the data cache versus from storage.
 * --------------------------------------------------------------------------- */

static int bench_randread(vfs_t* vfs, int count, int threads, const char* path) {
    (void)threads;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    const int page_sz = vfs->ctx->page_size;
    int64_t cache_cap = vfs_cache_get_max_entries(vfs->ctx->sb);

    /* File is 2× cache capacity so the cache holds at most half the data */
    int file_pages = (int)(2 * cache_cap);
    if (file_pages < 1024) file_pages = 1024;  /* minimum reasonable size */

    /* Number of read operations — independent of file size */
    int reads_needed = count > 0 ? count : file_pages;

    /* ── Pre-populate (untimed) ── */
    int writes_ok = 0;
    int64_t fvp = 0;
    {
        fvp = vfs_create(vfs, root_vp, "randfile.dat", 0);
        if (fvp <= 0) return 0;
        uint8_t* zbuf = (uint8_t*)calloc(1, (size_t)page_sz);
        if (!zbuf) return 0;
        for (int i = 0; i < file_pages; i++) {
            if (vfs_write(vfs, fvp, zbuf, (int64_t)i * page_sz, page_sz, 0) == page_sz)
                writes_ok++;
        }
        free(zbuf);
        if (writes_ok < file_pages / 2) return 0;
    }

    /* ── Close & reopen — clears cache, forces cold reads ── */
    int saved_max_entries = (int)cache_cap;
    vfs_unmount(vfs);
    vfs = vfs_mount(path, page_sz);
    if (!vfs) return 0;
    /* Restore cache size from first open (--cache-mb override) */
    vfs->ctx->sb->cache.max_entries = saved_max_entries;
    vfs->ctx->sb->cache.writeback_threshold = saved_max_entries / 4;
    root_vp = vfs->ctx->rootNodeOffset;

    /* ── Timed: truly random reads, each page picked independently ── */
    uint8_t* buf = (uint8_t*)malloc((size_t)page_sz);
    if (!buf) { vfs_unmount(vfs); return 0; }
    lat_init(reads_needed);
    vfs_cache_reset();
    double t0 = now_sec();
    int ok = 0;
    unsigned rseed = 42;
    for (int i = 0; i < reads_needed; i++) {
        double op_t0 = now_sec();
        int64_t offset = (int64_t)(rand_r(&rseed) % (unsigned)file_pages) * page_sz;
        int r = vfs_read(vfs, fvp, buf, offset, page_sz, 0);
        if (r == page_sz) ok++;
        lat_record(now_sec() - op_t0);
    }
    double elapsed = now_sec() - t0;
    free(buf);

    report_full("randread", ok, 1, elapsed);
    lat_destroy();
    vfs_unmount(vfs);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: seqread — sequential read of a file, measure MB/s
 * Two phases:
 *   seqread:write  — pre-populate file, flush, exit (run once)
 *   seqread:read   — open existing file, read sequentially, measure
 * Drop OS caches between phases for real disk throughput:
 *   macOS: sudo purge
 *   Linux: echo 3 | sudo tee /proc/sys/vm/drop_caches
 * --------------------------------------------------------------------------- */

static int bench_seqread(vfs_t* vfs, int count, int threads, const char* path,
                          const char* phase) {
    (void)threads;
    (void)path;
    const int64_t page_sz = vfs->ctx->page_size;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    int file_pages = count > 0 ? count : 65536;

    /* ── Write phase ── */
    if (!phase || strcmp(phase, "write") == 0) {
        printf("=== seqread:write (%d pages) ===\n", file_pages);
        int writes_ok = 0;
        int64_t fvp = vfs_create(vfs, root_vp, "seqfile.dat", 0);
        if (fvp <= 0) return 0;
        uint8_t* zbuf = (uint8_t*)calloc(1, (size_t)page_sz);
        if (!zbuf) return 0;
        double t0 = now_sec();
        for (int i = 0; i < file_pages; i++) {
            if (vfs_write(vfs, fvp, zbuf, (int64_t)i * page_sz, page_sz, 0) == page_sz)
                writes_ok++;
        }
        double elapsed = now_sec() - t0;
        free(zbuf);
        storage_flush(vfs->ctx->sb, -1);
        double mb = (double)file_pages * page_sz / (1024.0 * 1024.0);
        printf("  wrote %d/%d pages (%.1f MB) in %.3fs (%.1f MB/s)\n\n",
               writes_ok, file_pages, mb, elapsed, mb / elapsed);
        vfs_unmount(vfs);
        return writes_ok;
    }

    /* ── Read phase ── */
    root_vp = vfs->ctx->rootNodeOffset;
    int64_t file_vp = 0;
    {
        vfs_dirent_t dirents[16];
        int nd = vfs_readdir(vfs, root_vp, dirents, 16, 0);
        for (int di = 0; di < nd && file_vp <= 0; di++) {
            if (strcmp(dirents[di].name, "seqfile.dat") == 0)
                file_vp = dirents[di].vp;
        }
    }
    if (file_vp <= 0) { vfs_unmount(vfs); return 0; }

    int reads = file_pages;
    uint8_t* buf = (uint8_t*)malloc((size_t)page_sz);
    if (!buf) { vfs_unmount(vfs); return 0; }
    vfs_cache_reset();
    double t0 = now_sec();
    int ok = 0;
    for (int i = 0; i < reads; i++) {
        int r = vfs_read(vfs, file_vp, buf, (int64_t)i * page_sz, page_sz, 0);
        if (r == page_sz) ok++;
    }
    double elapsed = now_sec() - t0;
    free(buf);

    int64_t total_bytes = (int64_t)reads * page_sz;
    double mb = (double)total_bytes / (1024.0 * 1024.0);
    printf("\n=== seqread:read ===\n");
    printf("  pages=%d  size=%.1f MB  elapsed=%.3fs\n", reads, mb, elapsed);
    printf("  throughput: %.1f MB/s\n", mb / elapsed);
    printf("  ops/sec: %.0f\n", (double)reads / elapsed);
    int64_t ct = vfs_cache_total();
    int64_t ch = vfs_cache_hits();
    int64_t dt = vfs_data_total();
    int64_t dh = vfs_data_hits();
    printf("  cache: hits=%lld total=%lld ratio=%.1f%%\n",
           (long long)ch, (long long)ct, (ct > 0 ? 100.0*ch/ct : 0));
    printf("  data:  hits=%lld total=%lld ratio=%.1f%%\n",
           (long long)dh, (long long)dt, (dt > 0 ? 100.0*dh/dt : 0));
    printf("  completed: %d / %d operations\n\n", ok, reads);
    vfs_unmount(vfs);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Main — dispatch to workload
 * --------------------------------------------------------------------------- */

int main(int argc, char** argv) {
    bench_opts opts;
    int ret = parse_args(argc, argv, &opts);
    if (ret <= 0) return (ret == 0) ? 0 : 1;

    /* Open VFS file */
    /* For write-only phases, start clean.  For read phases, keep existing. */
    if (strncmp(opts.workload, "seqread", 7) != 0 ||
        strstr(opts.workload, ":read") == NULL)
        unlink(opts.output);


    vfs_t* vfs = vfs_mount(opts.output, opts.page_size);
    if (!vfs) {
        fprintf(stderr, "Failed to open VFS file: %s\n", opts.output);
        return 1;
    }

    /* Reset cache performance counters before running the workload */
    vfs_cache_reset();

    /* Apply --cache-mb override if specified */
    if (opts.cache_mb > 0) {
        int64_t pg = vfs->ctx->page_size;
        int64_t entries = ((int64_t)opts.cache_mb * 1024 * 1024) / pg;
        if (entries < 256) entries = 256;
        vfs->ctx->sb->cache.max_entries = (int)entries;
        vfs->ctx->sb->cache.writeback_threshold = (int)(entries / 4);
    }

    int ok = 0;
    double t0 = now_sec();
    if (strcmp(opts.workload, "create") == 0) {
        ok = bench_create(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "small_create") == 0) {
        ok = bench_small_file_create(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "sparse_rand_write") == 0) {
        ok = bench_sparse_random_writes(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "write") == 0) {
        ok = bench_write(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "read") == 0) {
        ok = bench_read(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "scan") == 0) {
        ok = bench_scan(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "mixed") == 0) {
        ok = bench_mixed(vfs, opts.count, opts.threads, opts.output,
                         opts.read_ratio);
    } else if (strcmp(opts.workload, "dir") == 0) {
        ok = bench_dir(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "seqwrite") == 0) {
        ok = bench_seqwrite(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "randread") == 0) {
        ok = bench_randread(vfs, opts.count, opts.threads, opts.output);
    } else if (strncmp(opts.workload, "seqread", 7) == 0) {
        const char* ph = strchr(opts.workload, ':');
        ok = bench_seqread(vfs, opts.count, opts.threads, opts.output,
                           ph ? ph + 1 : NULL);
    } else {
        fprintf(stderr, "Unknown workload: %s\n", opts.workload);
        usage();
        vfs_unmount(vfs);
        return 1;
    }

    double elapsed = now_sec() - t0;

    int total = (strcmp(opts.workload, "scan") == 0) ? opts.count
              : (strcmp(opts.workload, "dir") == 0) ? opts.count * 13
              : opts.count;
    printf("  completed: %d / %d operations\n", ok, total);

    if (opts.csv_path) {
        int64_t ct = vfs_cache_total();
        int64_t ch = vfs_cache_hits();
        int64_t dt = vfs_data_total();
        int64_t dh = vfs_data_hits();
        double ops_per_sec = (elapsed > 0) ? (double)ok / elapsed : 0.0;
        double cache_hit_pct = (ct > 0) ? 100.0 * (double)ch / (double)ct : 0.0;
        double data_hit_pct = (dt > 0) ? 100.0 * (double)dh / (double)dt : 0.0;
        write_csv(opts.csv_path, opts.workload, opts.count, opts.threads,
                  opts.page_size, opts.cache_mb, opts.read_ratio,
                  (int)(elapsed * 1e6), ops_per_sec,
                  cache_hit_pct, data_hit_pct, ok, total);
    }

    if (strcmp(opts.workload, "randread") != 0 &&
        strncmp(opts.workload, "seqread", 7) != 0)
        vfs_unmount(vfs);
    return 0;
}
