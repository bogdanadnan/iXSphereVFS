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
    int         cache_mb;   /* 0 = use default */
    int         read_ratio; /* for mixed workload, default 80 */
    const char* output;     /* output file path */
} bench_opts;

static bench_opts defaults = {
    .workload  = "create",
    .count     = 1000,
    .threads   = 1,
    .page_size = 8192,
    .cache_mb  = 0,
    .read_ratio = 80,
    .output    = "/tmp/vfs_bench.vfs",
};

static void usage(void) {
    fprintf(stderr,
        "Usage: vfs_bench --workload=<name> --count=N [options]\n"
        "Options:\n"
        "  --workload=<name>   create, write, read, scan, mixed, dir, seqwrite, randread, seqread (default: create)\n"
        "  --count=N           number of files/operations (default: 1000)\n"
        "  --threads=N         number of threads (default: 1)\n"
        "  --page-size=N       VFS page size in bytes (default: 8192)\n"
        "  --cache-mb=N        page cache size in MB (default: 0 = auto)\n"
        "  --read-ratio=N      mixed workload: %% reads (default: 80)\n"
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
        if (parse_int(argv[i], "--read-ratio=", &opts->read_ratio)) continue;
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

/* ---------------------------------------------------------------------------
 * Multi-threaded benchmark infrastructure
 * --------------------------------------------------------------------------- */

typedef struct {
    vfs_t*    vfs;
    int       tid;
    int       count;
    int       ok;
} bench_thread_ctx;

static int64_t resolve_child_vp(vfs_t* vfs, int64_t parent_vp, const char* name);

static void* bench_write_worker(void* arg) {
    bench_thread_ctx* c = (bench_thread_ctx*)arg;
    int64_t root_vp = c->vfs->ctx->rootNodeOffset;
    for (int i = 0; i < c->count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "t%d_w%d.txt", c->tid, i);
        int64_t nid = vfs_create(c->vfs, root_vp, name, 0);
        if (nid <= 0) continue;
        int64_t file_vp = resolve_child_vp(c->vfs, root_vp, name);
        if (file_vp == 0) continue;
        char data[128];
        memset(data, 'A' + c->tid, 128);
        if (vfs_write(c->vfs, file_vp, data, 0, 128, 0) > 0) c->ok++;
    }
    return NULL;
}

static void* bench_seqwrite_worker(void* arg) {
    bench_thread_ctx* c = (bench_thread_ctx*)arg;
    int64_t root_vp = c->vfs->ctx->rootNodeOffset;
    const int page_sz = c->vfs->ctx->page_size;
    char fname[64];
    snprintf(fname, sizeof(fname), "seqwrite_t%d.dat", c->tid);
    if (vfs_create(c->vfs, root_vp, fname, 0) <= 0) return NULL;
    int64_t fvp = resolve_child_vp(c->vfs, root_vp, fname);
    if (fvp == 0) return NULL;
    uint8_t* buf = calloc(1, (size_t)page_sz);
    if (!buf) return NULL;
    for (int i = 0; i < c->count; i++) {
        if (vfs_write(c->vfs, fvp, buf, (int64_t)i * page_sz, page_sz, 0) == page_sz) c->ok++;
    }
    free(buf);
    return NULL;
}

static int bench_run_threads(vfs_t* vfs, int thread_count, int ops_per_thread,
                              void* (*worker)(void*)) {
    pthread_t* th = malloc((size_t)thread_count * sizeof(pthread_t));
    bench_thread_ctx* ctx = calloc((size_t)thread_count, sizeof(bench_thread_ctx));
    for (int i = 0; i < thread_count; i++) {
        ctx[i].vfs = vfs;
        ctx[i].tid = i;
        ctx[i].count = ops_per_thread;
        ctx[i].ok = 0;
        pthread_create(&th[i], NULL, worker, &ctx[i]);
    }
    int total = 0;
    for (int i = 0; i < thread_count; i++) {
        pthread_join(th[i], NULL);
        total += ctx[i].ok;
    }
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
 * Workload: write N files — uses resolve_child_vp for VirtualPtr
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
            int64_t nid = vfs_create(vfs, root_vp, name, 0);
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
    double t0 = now_sec();
    int ops = count / threads;
    int64_t cache_max = (int64_t)vfs_cache_get_max_entries(vfs->ctx->sb);
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
        int64_t nid = vfs_create(vfs, root_vp, name, 0);
        if (nid > 0) file_vps[i] = resolve_child_vp(vfs, root_vp, name);

        /* Write 128 bytes to each file so reads exercise version chain traversal */
        if (file_vps[i] != 0) {
            char data[128];
            memset(data, 'A' + (i % 26), 128);
            vfs_write(vfs, file_vps[i], data, 0, 128, 0);
        }
    }

    double t0 = now_sec();
    int ok = 0;
    for (int i = 0; i < count; i++) {
        if (file_vps[i] == 0) continue;
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

/* ---------------------------------------------------------------------------
 * Workload: mixed — configurable read/write ratio on pre-populated files.
 * Pre-populates count files with page-sized data, then runs count operations
 * with --read-ratio (default 80) controlling read vs write mix at random offsets.
 * --------------------------------------------------------------------------- */

static int bench_mixed(vfs_t* vfs, int count, int threads, const char* path,
                        int read_ratio) {
    (void)path;
    if (read_ratio < 0) read_ratio = 80; /* default 80% reads */
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    const int page_sz = vfs->ctx->page_size;
    int nfiles = count > 1000 ? 1000 : count;
    int64_t* file_vps = (int64_t*)calloc((size_t)nfiles, sizeof(int64_t));

    /* Pre-populate nfiles with data */
    int populated = 0;
    for (int i = 0; i < nfiles && populated < nfiles; i++) {
        char name[64];
        snprintf(name, sizeof(name), "m%d.txt", i);
        if (vfs_create(vfs, root_vp, name, 0) <= 0) continue;
        file_vps[i] = resolve_child_vp(vfs, root_vp, name);
        if (file_vps[i] == 0) continue;
        /* Write 4 pages per file */
        uint8_t* zbuf = (uint8_t*)calloc(1, (size_t)page_sz);
        if (!zbuf) break;
        for (int p = 0; p < 4; p++)
            vfs_write(vfs, file_vps[i], zbuf, (int64_t)p * page_sz, page_sz, 0);
        free(zbuf);
        populated++;
    }
    if (populated == 0) { free(file_vps); return 0; }

    double t0 = now_sec();
    int ok = 0;
    unsigned rseed = 42;
    uint8_t* buf = (uint8_t*)malloc((size_t)page_sz);
    if (!buf) { free(file_vps); return 0; }
    memset(buf, 'Z', (size_t)page_sz);

    for (int i = 0; i < count; i++) {
        int fi = rand_r(&rseed) % populated;
        if (file_vps[fi] == 0) continue;
        int is_read = (rand_r(&rseed) % 100) < read_ratio;
        int64_t off = (int64_t)(rand_r(&rseed) % 4) * page_sz;
        if (is_read) {
            int r = vfs_read(vfs, file_vps[fi], buf, off, page_sz, 0);
            if (r == page_sz) ok++;
        } else {
            int w = vfs_write(vfs, file_vps[fi], buf, off, page_sz, 0);
            if (w == page_sz) ok++;
        }
    }
    double t1 = now_sec();
    free(buf);
    free(file_vps);
    report("mixed", ok, threads, t1 - t0);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Workload: dir — create directories and files inside
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Workload: dir — create/list/delete cycles on subdirectories.
 * Each cycle: mkdir → create 5 files → readdir → delete 5 files → rmdir.
 * --------------------------------------------------------------------------- */

static int bench_dir(vfs_t* vfs, int count, int threads, const char* path) {
    (void)path;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    double t0 = now_sec();
    int ok = 0;

    for (int i = 0; i < count; i++) {
        char dname[64];
        snprintf(dname, sizeof(dname), "d%d", i);

        /* mkdir */
        if (vfs_mkdir(vfs, root_vp, dname, 0) <= 0) continue;
        int64_t dir_vp = resolve_child_vp(vfs, root_vp, dname);
        if (dir_vp <= 0) continue;
        ok++;

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
    if (threads <= 1) {
        int64_t root_vp = vfs->ctx->rootNodeOffset;
        const int page_sz = vfs->ctx->page_size;
        int64_t nid = vfs_create(vfs, root_vp, "seqwrite.dat", 0);
        if (nid <= 0) return 0;
        int64_t file_vp = resolve_child_vp(vfs, root_vp, "seqwrite.dat");
        if (file_vp == 0) return 0;
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
    int64_t cache_max = (int64_t)vfs_cache_get_max_entries(vfs->ctx->sb);
    int total = bench_run_threads(vfs,
                                   threads, ops, bench_seqwrite_worker);
    double t1 = now_sec();
    report("seqwrite", total, threads, t1 - t0);
    return total;
}

/* ---------------------------------------------------------------------------
 * Workload: randread — random page reads from a pre-populated file.
 * Pre-populates `file_pages` pages via seqwrite, then randomly reads `count`
 * pages measuring per-read latency and cache hit rate.  Reads at least 2×
 * the cache capacity (estimated from count) to force compulsory misses.
 * --------------------------------------------------------------------------- */

static int bench_randread(vfs_t* vfs, int count, int threads, const char* path) {
    (void)threads;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    const int page_sz = vfs->ctx->page_size;
    int64_t cache_cap = vfs_cache_get_max_entries(vfs->ctx->sb);

    /* File is 2× cache capacity.  Reads exceed file size to mix hits/misses. */
    int reads_needed = count > 0 ? count : (int)(2 * cache_cap);
    int file_pages = reads_needed;
    if (file_pages > 65536) file_pages = 65536;

    /* ── Pre-populate (untimed) ── */
    int writes_ok = 0;
    {
        if (vfs_create(vfs, root_vp, "randfile.dat", 0) <= 0) return 0;
        int64_t fvp = resolve_child_vp(vfs, root_vp, "randfile.dat");
        if (fvp == 0) return 0;
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
    int64_t file_vp = resolve_child_vp(vfs, root_vp, "randfile.dat");
    if (file_vp == 0) { vfs_unmount(vfs); return 0; }

    /* ── Timed: truly random reads, each page picked independently ── */
    uint8_t* buf = (uint8_t*)malloc((size_t)page_sz);
    if (!buf) { vfs_unmount(vfs); return 0; }
    lat_init(reads_needed);
    vfs_cache_reset();
    double t0 = now_sec();
    int ok = 0;
    unsigned rseed = 42;
    for (int i = 0; i < reads_needed; i++) {
        int64_t offset = (int64_t)(rand_r(&rseed) % (unsigned)file_pages) * page_sz;
        int r = vfs_read(vfs, file_vp, buf, offset, page_sz, 0);
        if (r == page_sz) ok++;
        lat_record(now_sec() - t0);
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
    const int64_t page_sz = vfs->ctx->page_size;
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    int file_pages = count > 0 ? count : 65536;

    /* ── Write phase ── */
    if (!phase || strcmp(phase, "write") == 0) {
        printf("=== seqread:write (%d pages) ===\n", file_pages);
        int writes_ok = 0;
        if (vfs_create(vfs, root_vp, "seqfile.dat", 0) <= 0) return 0;
        int64_t fvp = resolve_child_vp(vfs, root_vp, "seqfile.dat");
        if (fvp == 0) return 0;
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
    int64_t file_vp = resolve_child_vp(vfs, root_vp, "seqfile.dat");
    if (file_vp == 0) { vfs_unmount(vfs); return 0; }

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

    int total = (strcmp(opts.workload, "scan") == 0) ? scan_count
              : (strcmp(opts.workload, "mixed") == 0) ? opts.count * 3
              : (strcmp(opts.workload, "dir") == 0) ? opts.count * 13
              : opts.count;
    printf("  completed: %d / %d operations\n", ok, total);

    if (strcmp(opts.workload, "randread") != 0 &&
        strncmp(opts.workload, "seqread", 7) != 0)
        vfs_unmount(vfs);
    return 0;
}
