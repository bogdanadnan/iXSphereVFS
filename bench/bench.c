/* IXSphere VFS Benchmark — multi-threaded CLI benchmark tool.
 * Usage: vfs_bench --workload=<name> --count=N [--threads=N] [--page-size=N]
 *                   [--cache-mb=N] [--output=<path>]
 *
 * Workloads:
 *   create   — create N files under root
 *   write    — create N files, write data to each
 *   read     — open N files, read data
 *   scan     — readdir on root, open all files, read data
 *   mixed    — mix of create, write, read, delete
 *   dir      — create N directories, create files inside
 *   seqwrite — sequential page-sized writes to a single file
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
    const char* workload;   /* "create", "write", "read", "scan", "mixed", "dir" */
    int         count;      /* number of files/operations */
    int         threads;    /* number of concurrent threads */
    int         page_size;  /* VFS page size (default 8192) */
    int         cache_mb;   /* not implemented yet — placeholder */
    const char* output;     /* output file path */
} bench_opts;

static bench_opts defaults = {
    .workload  = "create",
    .count     = 1000,
    .threads   = 1,
    .page_size = 8192,
    .cache_mb  = 0,
    .output    = "/tmp/vfs_bench.vfs",
};

static void usage(void) {
    fprintf(stderr,
        "Usage: vfs_bench --workload=<name> --count=N [options]\n"
        "Options:\n"
        "  --workload=<name>   create, write, read, scan, mixed, dir, seqwrite, randread (default: create)\n"
        "  --count=N           number of files/operations (default: 1000)\n"
        "  --threads=N         number of threads (default: 1)\n"
        "  --page-size=N       VFS page size in bytes (default: 8192)\n"
        "  --cache-mb=N        page cache size in MB (default: 0 = auto)\n"
        "  --output=<path>     VFS file path (default: /tmp/vfs_bench.vfs)\n"
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
        if (parse_str(argv[i], "--output=", &opts->output)) continue;
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
    double ratio = (ct > 0) ? (double)ch / (double)ct : 0.0;
    printf("  cache: hits=%lld  total=%lld  ratio=%.1f%%\n",
           (long long)ch, (long long)ct, ratio * 100.0);
    printf("\n");
}

/* ---------------------------------------------------------------------------
 * Helper: resolve a file VirtualPtr under root by name.
 * vfs_create returns a nodeId, but vfs_write expects a VirtualPtr.
 * We walk the root's DirContent chain to find the matching entry's childPtr.
 * --------------------------------------------------------------------------- */

static int64_t resolve_child_vp(vfs_t* vfs, int64_t parent_vp, const char* name) {
    uint8_t* rs = pool_resolve(&vfs->ctx->pool, parent_vp);
    if (!rs) return 0;
    int64_t h = vfs_rd8_s(rs, DIRNODE_OFF_HEADPTR, vfs->ctx->page_size);
    int64_t w = h;
    while (w != 0) {
        uint8_t* dc = pool_resolve(&vfs->ctx->pool, w);
        if (!dc) break;
        uint32_t cc, ce;
        int64_t cp, np, nx;
        nodes_read_dircontent(dc, &cc, &ce, &cp, &np, &nx, vfs->ctx->page_size);
        (void)cc; (void)ce;
        char en[256];
        int nl = nodes_read_name(&vfs->ctx->pool, np, en, (int)sizeof(en));
        if (nl > 0 && strcmp(en, name) == 0) return cp;
        w = nx;
    }
    return 0;
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
        int ret = vfs_create(vfs, root_vp, name, 0);
        if (ret > 0) ok++;
        lat_record(now_sec() - op_t0);
    }
    double t1 = now_sec();
    report_full("create", ok, threads, t1 - t0);
    lat_destroy();
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: write N files — uses resolve_child_vp for VirtualPtr
 * --------------------------------------------------------------------------- */

static int bench_write(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    double t0 = now_sec();
    int ok = 0;
    for (int i = 0; i < count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "w%d.txt", i);
        int nid = vfs_create(vfs, root_vp, name, 0);
        if (nid <= 0) continue;
        int64_t file_vp = resolve_child_vp(vfs, root_vp, name);
        if (file_vp == 0) continue;
        char data[128];
        memset(data, 'A' + (i % 26), sizeof(data));
        int written = vfs_write(vfs, file_vp, data, 0, sizeof(data), 0);
        if (written > 0) ok++;
    }
    double t1 = now_sec();
    report("write", ok, threads, t1 - t0);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: read N files
 * --------------------------------------------------------------------------- */

static int bench_read(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    for (int i = 0; i < count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "r%d.txt", i);
        vfs_create(vfs, root_vp, name, 0);
    }
    double t0 = now_sec();
    int ok = 0;
    for (int i = 0; i < count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "r%d.txt", i);
        int64_t file_vp = resolve_child_vp(vfs, root_vp, name);
        if (file_vp == 0) continue;
        char buf[128];
        int r = vfs_read(vfs, file_vp, buf, 0, sizeof(buf), 0);
        if (r > 0) ok++;
    }
    double t1 = now_sec();
    report("read", ok, threads, t1 - t0);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: scan — create, readdir, read all.
 * NOTE: limited to 1024 entries (DENTRY_CACHE_MAX) by VFS readdir.
 * --------------------------------------------------------------------------- */

static int bench_scan(vfs_t* vfs, int* count, int threads, const char* path) {
    (void)path;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    int max_entries = 1024;
    if (*count > max_entries) {
        fprintf(stderr, "warning: scan workload capped at %d entries (VFS readdir limit)\n", max_entries);
        *count = max_entries;
    }
    int capped = *count;
    for (int i = 0; i < capped; i++) {
        char name[64];
        snprintf(name, sizeof(name), "s%d.txt", i);
        vfs_create(vfs, root_vp, name, 0);
    }
    double t0 = now_sec();
    int ok = 0;

    vfs_dirent_t entries[1024];  /* matches DENTRY_CACHE_MAX */
    int n = vfs_readdir(vfs, root_vp, entries, 1024, 0);
    if (n < 0) n = 0;

    for (int i = 0; i < n; i++) {
        int64_t file_vp = resolve_child_vp(vfs, root_vp, entries[i].name);
        if (file_vp == 0) continue;
        char buf[128];
        int r = vfs_read(vfs, file_vp, buf, 0, sizeof(buf), 0);
        if (r > 0) ok++;
    }
    double t1 = now_sec();
    report("scan", ok, threads, t1 - t0);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: mixed — create, write, read, delete
 * --------------------------------------------------------------------------- */

static int bench_mixed(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    double t0 = now_sec();
    int ok = 0;

    for (int i = 0; i < count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "m%d.txt", i);

        int nid = vfs_create(vfs, root_vp, name, 0);
        if (nid <= 0) continue;

        int64_t file_vp = resolve_child_vp(vfs, root_vp, name);
        if (file_vp == 0) continue;

        char data[64];
        memset(data, 'X', sizeof(data));
        if (vfs_write(vfs, file_vp, data, 0, sizeof(data), 0) > 0) ok++;

        char buf[64];
        if (vfs_read(vfs, file_vp, buf, 0, sizeof(buf), 0) > 0) ok++;

        if (vfs_delete(vfs, root_vp, name, 0) == VFS_OK) ok++;
    }
    double t1 = now_sec();
    report("mixed", ok, threads, t1 - t0);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: dir — create directories and files inside
 * --------------------------------------------------------------------------- */

static int bench_dir(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    double t0 = now_sec();
    int ok = 0;

    for (int i = 0; i < count; i++) {
        char dname[64];
        snprintf(dname, sizeof(dname), "d%d", i);

        int dr = vfs_mkdir(vfs, root_vp, dname, 0);
        if (dr != VFS_OK) continue;

        int64_t dir_vp = resolve_child_vp(vfs, root_vp, dname);
        if (dir_vp <= 0) continue;

        for (int j = 0; j < 5; j++) {
            char fname[64];
            snprintf(fname, sizeof(fname), "f%d.txt", j);
            int nid = vfs_create(vfs, dir_vp, fname, 0);
            if (nid > 0) ok++;
        }
    }
    double t1 = now_sec();
    report("dir", ok, threads, t1 - t0);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: seqwrite — sequential page-sized writes to a single file.
 * Phase 1 (untimed): pre-allocate all pages.
 * Phase 2 (timed): overwrite pages, measuring per-page latency.
 * --------------------------------------------------------------------------- */

static int bench_seqwrite(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    (void)threads;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    const int page_sz = vfs->ctx->page_size;

    /* Create a single file and resolve its VirtualPtr */
    int nid = vfs_create(vfs, root_vp, "seqwrite.dat", 0);
    if (nid <= 0) return 0;
    int64_t file_vp = resolve_child_vp(vfs, root_vp, "seqwrite.dat");
    if (file_vp == 0) return 0;

    uint8_t* buf = (uint8_t*)malloc((size_t)page_sz);
    if (!buf) return 0;

    /* Phase 1: Pre-allocate all pages (untimed).
       Write at each offset to force page allocation, then read the data
       back to warm the cache.  This separates allocation cost from the
       timed write throughput measurement. */
    memset(buf, 0, (size_t)page_sz);
    for (int i = 0; i < count; i++) {
        int64_t offset = (int64_t)i * (int64_t)page_sz;
        vfs_write(vfs, file_vp, buf, offset, page_sz, 0);
    }

    /* Phase 2: Timed sequential writes (pages already exist, in-place). */
    memset(buf, 'W', (size_t)page_sz);
    lat_init(count);
    vfs_cache_reset();
    double t0 = now_sec();
    int ok = 0;
    for (int i = 0; i < count; i++) {
        double op_t0 = now_sec();
        int64_t offset = (int64_t)i * (int64_t)page_sz;
        int written = vfs_write(vfs, file_vp, buf, offset, page_sz, 0);
        if (written == page_sz) ok++;
        lat_record(now_sec() - op_t0);
    }
    double t1 = now_sec();
    free(buf);

    report_full("seqwrite", ok, threads, t1 - t0);
    lat_destroy();
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: randread — random page reads from a pre-populated file.
 * Pre-populates `file_pages` pages via seqwrite, then randomly reads `count`
 * pages measuring per-read latency and cache hit rate.  Reads at least 2×
 * the cache capacity (estimated from count) to force compulsory misses.
 * --------------------------------------------------------------------------- */

static int bench_randread(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    (void)threads;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    const int page_sz = vfs->ctx->page_size;

    /* Pre-populate a file with enough pages to exceed default cache size.
       The page cache has ~1024 entries — we populate 2× that to force
       compulsory cache misses during random reads. */
    int64_t file_vp = 0;
    {
        BENCH_CHECK(vfs_create(vfs, root_vp, "randfile.dat", 0) > 0);
        file_vp = resolve_child_vp(vfs, root_vp, "randfile.dat");
        BENCH_CHECK(file_vp != 0);
        int file_pages = count < 2048 ? 2048 : count * 2;
        uint8_t* zbuf = (uint8_t*)calloc(1, (size_t)page_sz);
        BENCH_CHECK(zbuf != NULL);
        for (int i = 0; i < file_pages; i++)
            vfs_write(vfs, file_vp, zbuf, (int64_t)i * page_sz, page_sz, 0);
        free(zbuf);
    }

    /* Seeded pseudo-random permutation of page offsets */
    int* order = (int*)calloc((size_t)count, sizeof(int));
    BENCH_CHECK(order != NULL);
    for (int i = 0; i < count; i++) order[i] = i;
    unsigned seed = 42;
    for (int i = count - 1; i > 0; i--) {
        int j = rand_r(&seed) % (i + 1);
        int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }

    /* Timed phase: random reads */
    uint8_t* buf = (uint8_t*)malloc((size_t)page_sz);
    BENCH_CHECK(buf != NULL);
    lat_init(count);
    vfs_cache_reset();
    double t0 = now_sec();
    int ok = 0;
    for (int i = 0; i < count; i++) {
        double op_t0 = now_sec();
        int64_t offset = (int64_t)order[i] * (int64_t)page_sz;
        int r = vfs_read(vfs, file_vp, buf, offset, page_sz, 0);
        if (r == page_sz) ok++;
        lat_record(now_sec() - op_t0);
    }
    double t1 = now_sec();
    free(buf);
    free(order);

    report_full("randread", ok, threads, t1 - t0);
    lat_destroy();
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
    unlink(opts.output);

    if (opts.threads > 1)
        fprintf(stderr, "warning: --threads not yet implemented, running single-threaded\n");

    vfs_t* vfs = vfs_open(opts.output, opts.page_size);
    if (!vfs) {
        fprintf(stderr, "Failed to open VFS file: %s\n", opts.output);
        return 1;
    }

    /* Reset cache performance counters before running the workload */
    vfs_cache_reset();

    int ok = 0;
    int scan_count = opts.count;
    if (strcmp(opts.workload, "create") == 0) {
        ok = bench_create(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "write") == 0) {
        ok = bench_write(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "read") == 0) {
        ok = bench_read(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "scan") == 0) {
        ok = bench_scan(vfs, &scan_count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "mixed") == 0) {
        ok = bench_mixed(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "dir") == 0) {
        ok = bench_dir(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "seqwrite") == 0) {
        ok = bench_seqwrite(vfs, opts.count, opts.threads, opts.output);
    } else if (strcmp(opts.workload, "randread") == 0) {
        ok = bench_randread(vfs, opts.count, opts.threads, opts.output);
    } else {
        fprintf(stderr, "Unknown workload: %s\n", opts.workload);
        usage();
        vfs_close(vfs);
        return 1;
    }

    int total = (strcmp(opts.workload, "scan") == 0) ? scan_count
              : (strcmp(opts.workload, "mixed") == 0) ? opts.count * 3
              : (strcmp(opts.workload, "dir") == 0) ? opts.count * 5
              : opts.count;
    printf("  completed: %d / %d operations\n", ok, total);

    vfs_close(vfs);
    return 0;
}
