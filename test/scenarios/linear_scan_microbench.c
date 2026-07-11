/* linear_bench.c — profile linear scan dedup to compare. */
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

int main(int argc, char** argv) {
    int N = argc > 1 ? atoi(argv[1]) : 6500;
    int reps = argc > 2 ? atoi(argv[2]) : 100;

    printf("=== linear scan dedup microbench (N=%d, reps=%d) ===\n\n", N, reps);

    typedef struct { int64_t childNodeId; int64_t value; } Entry;

    /* Test 1: simulate dirchain_list dedup loop
       For each new entry, scan dedup->count entries. */
    {
        VarArray(Entry) dedup = var_array_new(Entry);

        /* Pre-populate with N entries */
        double t0 = now_sec();
        for (int r = 0; r < reps; r++) {
            dedup->count = 0;  /* reset */
            for (int i = 0; i < N; i++) {
                /* Linear scan */
                int found = 0;
                for (int j = 0; j < dedup->count; j++) {
                    Entry* e = var_array_lookup(dedup, j);
                    if (e && e->childNodeId == (int64_t)i) { found = 1; break; }
                }
                if (!found) {
                    Entry entry = { .childNodeId = (int64_t)i, .value = (int64_t)i };
                    var_array_append(dedup, entry);
                }
            }
        }
        double t1 = now_sec();
        printf("linear scan dedup loop (N unique keys): %.2f ms per pass (%.2f us/insert)\n",
               (t1 - t0) / reps * 1000,
               (t1 - t0) / (N * reps) * 1e6);
        var_array_delete(dedup);
    }

    /* Test 2: pure var_array_append cost */
    {
        VarArray(Entry) dedup = var_array_new(Entry);
        double t0 = now_sec();
        for (int i = 0; i < N; i++) {
            Entry entry = { .childNodeId = (int64_t)i, .value = (int64_t)i };
            var_array_append(dedup, entry);
        }
        double t1 = now_sec();
        printf("var_array_append x N: %.2f us/append\n",
               (t1 - t0) / N * 1e6);
        var_array_delete(dedup);
    }

    /* Test 3: pure var_array_lookup cost (sequential) */
    {
        VarArray(Entry) dedup = var_array_new(Entry);
        for (int i = 0; i < N; i++) {
            Entry entry = { .childNodeId = (int64_t)i, .value = (int64_t)i };
            var_array_append(dedup, entry);
        }
        double t0 = now_sec();
        uint64_t sum = 0;
        for (int r = 0; r < reps; r++) {
            for (int i = 0; i < N; i++) {
                Entry* e = var_array_lookup(dedup, i);
                if (e) sum += (uint64_t)e->childNodeId;
            }
        }
        double t1 = now_sec();
        printf("var_array_lookup x N (sequential): %.2f ns/lookup\n",
               (t1 - t0) / (N * reps) * 1e9);
        printf("  (sum=%llu to prevent optimization)\n", (unsigned long long)sum);
        var_array_delete(dedup);
    }

    return 0;
}
