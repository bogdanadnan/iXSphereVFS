/* hash_map_params_bench.c — test different (scale, granularity) combinations. */
#include "hash_map.h"
#include "var_array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int count_recursive(void* node, int height, int chunk_size) {
    if (!node) return 0;
    int count = 1;
    if (height > 0) {
        VarArrayLevel* level = (VarArrayLevel*)node;
        for (int i = 0; i < chunk_size; i++) {
            void** slot_ptr = (void**)((char*)(level + 1)) + i;
            if (*slot_ptr) count += count_recursive(*slot_ptr, height - 1, chunk_size);
        }
    }
    return count;
}

static void bench_params(int scale, int granularity, int N, int reps) {
    int64_t capacity = (int64_t)1 << scale;
    int chunk_size = (int64_t)1 << granularity;
    printf("\n=== scale=%d (capacity=2^%d=%lld), granularity=%d (chunk=2^%d=%d) ===\n",
           scale, scale, (long long)capacity, granularity, granularity, chunk_size);

    /* Measure put cycle */
    double sum_put = 0;
    int min_chunks = 0;
    for (int rep = 0; rep < reps; rep++) {
        HashMapBase* m = hash_map_base_new_cap(scale, granularity);
        double t0 = now_sec();
        for (int i = 0; i < N; i++) {
            hash_map_base_put(m, (int64_t)i, (int64_t)i);
        }
        double t1 = now_sec();
        sum_put += (t1 - t0);
        
        if (rep == 0) {
            min_chunks = count_recursive(m->slots->root, *(volatile int*)m->slots->root, m->slots->chunk_size);
        }
        hash_map_base_free(m);
    }
    double avg_put_us = (sum_put / reps) / N * 1e6;
    printf("  put cycle (N=%d): %.2f us/op, %.2f ms total\n",
           N, avg_put_us, sum_put / reps * 1000);
    printf("  nodes allocated: %d (avg %.2f nodes/insert)\n",
           min_chunks, (double)min_chunks / N);

    /* Measure lookups */
    HashMapBase* m = hash_map_base_new_cap(scale, granularity);
    for (int i = 0; i < N; i++) {
        hash_map_base_put(m, (int64_t)i, (int64_t)i);
    }
    double sum_lookup = 0;
    for (int rep = 0; rep < reps; rep++) {
        double t0 = now_sec();
        for (int i = 0; i < N; i++) {
            hash_map_base_contains(m, (int64_t)i);
        }
        double t1 = now_sec();
        sum_lookup += (t1 - t0);
    }
    double avg_lookup_ns = (sum_lookup / reps) / N * 1e9;
    printf("  lookup (hit): %.2f ns/op\n", avg_lookup_ns);
    hash_map_base_free(m);

    /* Measure miss lookups */
    m = hash_map_base_new_cap(scale, granularity);
    for (int i = 0; i < N; i++) {
        hash_map_base_put(m, (int64_t)i, (int64_t)i);
    }
    sum_lookup = 0;
    for (int rep = 0; rep < reps; rep++) {
        double t0 = now_sec();
        for (int i = 0; i < N; i++) {
            hash_map_base_contains(m, (int64_t)(i + 10000000));
        }
        double t1 = now_sec();
        sum_lookup += (t1 - t0);
    }
    double avg_miss_ns = (sum_lookup / reps) / N * 1e9;
    printf("  lookup (miss): %.2f ns/op\n", avg_miss_ns);
    hash_map_base_free(m);

    /* Measure full cycle (new + puts + lookups + free) — mimics dirchain_list */
    double sum_full = 0;
    for (int rep = 0; rep < reps; rep++) {
        double t0 = now_sec();
        HashMapBase* mm = hash_map_base_new_cap(scale, granularity);
        for (int i = 0; i < N; i++) {
            if (!hash_map_base_contains(mm, (int64_t)i)) {
                hash_map_base_put(mm, (int64_t)i, (int64_t)i);
            }
        }
        for (int i = 0; i < N; i++) {
            hash_map_base_contains(mm, (int64_t)i);
        }
        hash_map_base_free(mm);
        double t1 = now_sec();
        sum_full += (t1 - t0);
    }
    printf("  full cycle (new + N put + N contains + free): %.2f ms\n",
           sum_full / reps * 1000);
}

int main(int argc, char** argv) {
    int N = argc > 1 ? atoi(argv[1]) : 6500;
    int reps = argc > 2 ? atoi(argv[2]) : 30;

    printf("hash_map parameter sweep for N=%d, reps=%d\n\n", N, reps);

    /* Default (current): scale=20, granularity=8 */
    bench_params(20, 8, N, reps);
    /* User-requested: scale=9, granularity=16 */
    bench_params(9, 16, N, reps);
    /* Compare: scale=9, granularity=8 (smaller chunk for less sparse) */
    bench_params(9, 8, N, reps);
    /* scale=20, granularity=16 (large chunk, lots of capacity) */
    bench_params(20, 16, N, reps);
    /* scale=16, granularity=16 (medium capacity, large chunk) */
    bench_params(16, 16, N, reps);

    return 0;
}
