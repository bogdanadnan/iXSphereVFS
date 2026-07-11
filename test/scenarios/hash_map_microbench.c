/* hash_map_bench.c — profile hash_map operations to find bottleneck. */
#include "hash_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    int N = argc > 1 ? atoi(argv[1]) : 6500;
    int reps = argc > 2 ? atoi(argv[2]) : 100;

    printf("=== hash_map microbench (N=%d, reps=%d) ===\n\n", N, reps);

    /* Test 1: hash_map_new / free overhead */
    {
        double sum = 0;
        for (int r = 0; r < reps; r++) {
            double t0 = now_sec();
            HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);
            double t1 = now_sec();
            hash_map_free(m);
            double t2 = now_sec();
            sum += (t2 - t0);
        }
        printf("hash_map_new + hash_map_free: %.2f us per call\n",
               sum / reps * 1e6);
    }

    /* Test 2: insert N keys */
    {
        HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);
        double t0 = now_sec();
        for (int i = 0; i < N; i++) {
            hash_map_put(m, (int64_t)i, (int64_t)i);
        }
        double t1 = now_sec();
        printf("insert N keys: %.2f us/insert (total %.2f ms)\n",
               (t1 - t0) / N * 1e6, (t1 - t0) * 1000);
        hash_map_free(m);
    }

    /* Test 3: lookup N keys (hit case) */
    {
        HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);
        for (int i = 0; i < N; i++) hash_map_put(m, (int64_t)i, (int64_t)i);
        double t0 = now_sec();
        for (int r = 0; r < reps; r++) {
            for (int i = 0; i < N; i++) {
                hash_map_contains(m, (int64_t)i);
            }
        }
        double t1 = now_sec();
        printf("lookup N keys (hit): %.2f ns/lookup\n",
               (t1 - t0) / (N * reps) * 1e9);
        hash_map_free(m);
    }

    /* Test 4: lookup N keys (miss case) */
    {
        HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);
        for (int i = 0; i < N; i++) hash_map_put(m, (int64_t)i, (int64_t)i);
        double t0 = now_sec();
        for (int r = 0; r < reps; r++) {
            for (int i = 0; i < N; i++) {
                hash_map_contains(m, (int64_t)(i + 10000000));
            }
        }
        double t1 = now_sec();
        printf("lookup N keys (miss): %.2f ns/lookup\n",
               (t1 - t0) / (N * reps) * 1e9);
        hash_map_free(m);
    }

    /* Test 5: pure hash function cost */
    {
        double t0 = now_sec();
        uint64_t sum = 0;
        for (int r = 0; r < reps * 100; r++) {
            for (int i = 0; i < N; i++) {
                sum += hash_key_64((int64_t)i);
            }
        }
        double t1 = now_sec();
        printf("pure hash_key_64: %.2f ns/hash\n",
               (t1 - t0) / (N * reps * 100) * 1e9);
        printf("  (sum=%llu to prevent optimization)\n", (unsigned long long)sum);
    }

    /* Test 6: just var_array_resolve_base cost (per-probe tree walk) */
    {
        HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);
        for (int i = 0; i < N; i++) hash_map_put(m, (int64_t)i, (int64_t)i);
        double t0 = now_sec();
        uint64_t sum = 0;
        for (int r = 0; r < reps; r++) {
            for (int i = 0; i < N; i++) {
                void* chunk = var_array_resolve_base(m->slots, (int64_t)i);
                if (chunk) sum += *(uint64_t*)chunk;
            }
        }
        double t1 = now_sec();
        printf("var_array_resolve_base (per-probe tree walk): %.2f ns/lookup\n",
               (t1 - t0) / (N * reps) * 1e9);
        printf("  (sum=%llu to prevent optimization)\n", (unsigned long long)sum);
        hash_map_free(m);
    }

    return 0;
}
