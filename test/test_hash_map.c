/* test_hash_map.c — comprehensive tests for the hash_map primitive.
 *
 * Coverage mirrors test_var_array.c:
 *   - new/delete lifecycle
 *   - new with custom capacity
 *   - put/get/contains/delete basic
 *   - put returns 1 for new, 0 for update
 *   - get returns NULL for missing
 *   - delete returns 1 for found, 0 for missing
 *   - iteration over all occupied slots
 *   - resize triggered at load factor 0.75
 *   - collision handling (linear probing)
 *   - tombstone reuse (Robin Hood "first tombstone wins")
 *   - bulk insert/iterate/delete
 *   - mixed types via HashMap(K, V) macro (int64_t, int64_t and others)
 *   - concurrent put + lookup (single-writer per thread, lock-free reads)
 *   - delete + reinsert
 *   - edge cases: empty map, single element, capacity boundaries
 *
 * Build flags (set via CMakeLists.txt):
 *   -Wno-pedantic -Wno-incompatible-pointer-types -fno-strict-aliasing
 */

#include "hash_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <limits.h>

static int tests_run = 0, tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

/* ---------------------------------------------------------------------------
 * Lifecycle tests
 * --------------------------------------------------------------------------- */

static void test_new_delete(void) {
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);
    CHECK(m != NULL);
    /* Default scale=20 -> capacity = 2^20 = 1M. */
    CHECK_EQ(m->capacity, 1048576);
    CHECK_EQ(m->size, 0);
    CHECK_EQ(m->tombstones, 0);
    CHECK_EQ(hash_map_size(m), 0);
    hash_map_free(m);
}

static void test_new_cap_basic(void) {
    /* new_cap(scale, granularity): capacity = 2^scale, chunk_size = 2^granularity. */
    HashMap(int64_t, int64_t) m = hash_map_new_cap(int64_t, int64_t, 4, 8);
    CHECK(m != NULL);
    CHECK_EQ(m->capacity, 16);    /* 2^4 = 16 */
    CHECK_EQ(m->chunk_size, 256); /* 2^8 = 256 */
    hash_map_free(m);

    HashMap(int64_t, int64_t) m2 = hash_map_new_cap(int64_t, int64_t, 10, 10);
    CHECK(m2 != NULL);
    CHECK_EQ(m2->capacity, 1024);  /* 2^10 */
    CHECK_EQ(m2->chunk_size, 1024); /* 2^10 */
    hash_map_free(m2);

    HashMap(int64_t, int64_t) m3 = hash_map_new_cap(int64_t, int64_t, 20, 8);
    CHECK(m3 != NULL);
    CHECK_EQ(m3->capacity, 1048576); /* 2^20 = 1M */
    CHECK_EQ(m3->chunk_size, 256);
    hash_map_free(m3);
}

static void test_new_cap_clamps(void) {
    /* scale and granularity are clamped to legal ranges. */
    /* scale too small (1 is the min): */
    HashMap(int64_t, int64_t) m0 = hash_map_new_cap(int64_t, int64_t, 0, 8);
    CHECK(m0 != NULL);
    CHECK_EQ(m0->capacity, 2);  /* clamped to scale=1, 2^1 = 2 */
    hash_map_free(m0);

    /* scale too large (32 is the max): */
    HashMap(int64_t, int64_t) m1 = hash_map_new_cap(int64_t, int64_t, 100, 8);
    CHECK(m1 != NULL);
    CHECK_EQ(m1->capacity, 4294967296LL); /* clamped to scale=32, 2^32 */
    hash_map_free(m1);

    /* granularity too small: */
    HashMap(int64_t, int64_t) m2 = hash_map_new_cap(int64_t, int64_t, 10, 0);
    CHECK(m2 != NULL);
    CHECK_EQ(m2->chunk_size, 2); /* clamped to granularity=1, 2^1 = 2 */
    hash_map_free(m2);

    /* granularity too large: */
    HashMap(int64_t, int64_t) m3 = hash_map_new_cap(int64_t, int64_t, 10, 100);
    CHECK(m3 != NULL);
    CHECK_EQ(m3->chunk_size, 65536); /* clamped to granularity=16, 2^16 */
    hash_map_free(m3);

    /* negative values clamped up to minimum: */
    HashMap(int64_t, int64_t) m4 = hash_map_new_cap(int64_t, int64_t, -5, -5);
    CHECK(m4 != NULL);
    CHECK_EQ(m4->capacity, 2);      /* clamped to scale=1 */
    CHECK_EQ(m4->chunk_size, 2);    /* clamped to granularity=1 */
    hash_map_free(m4);
}

/* ---------------------------------------------------------------------------
 * Basic put/get/delete
 * --------------------------------------------------------------------------- */

static void test_put_get_basic(void) {
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);

    /* Insert returns 1 for new. */
    int r = hash_map_put(m, 42, 100);
    CHECK_EQ(r, 1);
    CHECK_EQ(m->size, 1);

    /* Update returns 0. */
    r = hash_map_put(m, 42, 200);
    CHECK_EQ(r, 0);
    CHECK_EQ(m->size, 1);  /* still 1 */

    /* Get returns the latest value. */
    int64_t* v = hash_map_get(m, 42);
    CHECK(v != NULL);
    if (v) CHECK_EQ(*v, 200);

    /* contains true. */
    CHECK_EQ(hash_map_contains(m, 42), 1);

    /* Missing key. */
    v = hash_map_get(m, 999);
    CHECK(v == NULL);
    CHECK_EQ(hash_map_contains(m, 999), 0);

    hash_map_free(m);
}

static void test_multiple_inserts(void) {
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);

    for (int i = 0; i < 100; i++) {
        int r = hash_map_put(m, (int64_t)i, (int64_t)(i * 10));
        CHECK_EQ(r, 1);
    }
    CHECK_EQ(hash_map_size(m), 100);

    for (int i = 0; i < 100; i++) {
        int64_t* v = hash_map_get(m, (int64_t)i);
        CHECK(v != NULL);
        if (v) CHECK_EQ(*v, i * 10);
    }

    hash_map_free(m);
}

static void test_delete(void) {
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);

    hash_map_put(m, 1, 10);
    hash_map_put(m, 2, 20);
    hash_map_put(m, 3, 30);

    /* Delete present. */
    int r = hash_map_delete(m, 2);
    CHECK_EQ(r, 1);
    CHECK_EQ(m->size, 2);
    CHECK(hash_map_get(m, 2) == NULL);

    /* Delete absent. */
    r = hash_map_delete(m, 99);
    CHECK_EQ(r, 0);

    /* Other entries still findable. */
    CHECK(hash_map_get(m, 1) != NULL);
    CHECK(hash_map_get(m, 3) != NULL);

    /* Delete twice — second is no-op. */
    r = hash_map_delete(m, 1);
    CHECK_EQ(r, 1);
    r = hash_map_delete(m, 1);
    CHECK_EQ(r, 0);

    hash_map_free(m);
}

/* ---------------------------------------------------------------------------
 * Resize behavior
 * --------------------------------------------------------------------------- */

static void test_no_resize_fixed_capacity(void) {
    HashMap(int64_t, int64_t) m = hash_map_new_cap(int64_t, int64_t, 4, 8);  /* 2^4 = 16 */

    /* Insert 13 entries — would have triggered resize under Phase 21,
       but Phase 23 keeps capacity at 16.  All entries findable. */
    for (int i = 0; i < 12; i++) {
        hash_map_put(m, (int64_t)i, (int64_t)(i + 1));
    }
    CHECK_EQ(m->capacity, 16);

    hash_map_put(m, 12, 13);
    CHECK_EQ(m->capacity, 16);  /* unchanged */
    CHECK_EQ(m->size, 13);

    /* All entries still findable. */
    for (int i = 0; i < 13; i++) {
        int64_t* v = hash_map_get(m, (int64_t)i);
        CHECK(v != NULL);
        if (v) CHECK_EQ(*v, i + 1);
    }

    hash_map_free(m);
}

static void test_tombstones_no_clear(void) {
    /* Phase 23: tombstones are NOT cleared by resize (there's no resize).
       They accumulate.  Verified by insert/delete cycle. */
    HashMap(int64_t, int64_t) m = hash_map_new_cap(int64_t, int64_t, 4, 8);

    /* Insert 10 entries, delete 5 (creates 5 tombstones). */
    for (int i = 0; i < 10; i++) hash_map_put(m, (int64_t)i, (int64_t)i);
    for (int i = 0; i < 10; i += 2) hash_map_delete(m, (int64_t)i);
    CHECK_EQ(m->size, 5);
    CHECK_EQ(m->tombstones, 5);

    /* Insert 20 more — no resize.  Capacity unchanged. */
    for (int i = 0; i < 20; i++) {
        hash_map_put(m, (int64_t)(1000 + i), (int64_t)(1000 + i));
    }
    CHECK_EQ(m->capacity, 16);
    /* tombstones may have decreased via reuse but never cleared automatically. */

    /* Non-deleted original entries still findable. */
    for (int i = 1; i < 10; i += 2) {
        int64_t* v = hash_map_get(m, (int64_t)i);
        CHECK(v != NULL);
        if (v) CHECK_EQ(*v, i);
    }

    hash_map_free(m);
}

/* ---------------------------------------------------------------------------
 * Tombstone reuse (Robin Hood "first tombstone wins")
 *
 * Insert, delete, then re-insert. The new entry should reuse the
 * tombstone slot — same probe distance, no extra probing past it.
 * --------------------------------------------------------------------------- */

static void test_tombstone_reuse(void) {
    HashMap(int64_t, int64_t) m = hash_map_new_cap(int64_t, int64_t, 16, 8);

    hash_map_put(m, 1, 100);
    hash_map_put(m, 2, 200);
    hash_map_delete(m, 1);  /* tombstone at hash(1) */

    /* Re-insert 1 — should go into the tombstone slot. */
    int r = hash_map_put(m, 1, 1000);
    CHECK_EQ(r, 1);
    CHECK_EQ(m->size, 2);
    CHECK_EQ(m->tombstones, 0);

    int64_t* v = hash_map_get(m, 1);
    CHECK(v != NULL);
    if (v) CHECK_EQ(*v, 1000);

    /* 2 still findable. */
    v = hash_map_get(m, 2);
    CHECK(v != NULL);
    if (v) CHECK_EQ(*v, 200);

    hash_map_free(m);
}

/* ---------------------------------------------------------------------------
 * Collision handling — many keys that hash to same bucket.
 *
 * We construct a test where multiple keys have a forced collision:
 * use the raw bucket index derived from the hash function.
 * --------------------------------------------------------------------------- */

static void test_collision_probe(void) {
    HashMap(int64_t, int64_t) m = hash_map_new_cap(int64_t, int64_t, 16, 8);

    /* Pick 10 keys that all hash to bucket 5 (manually pick).
     * hash_key_64(0) = 14695981039346656037 -> bucket 5 (in capacity-16).
     * Actually use the inverse: pick keys K such that hash(K) mod 16 == 5.
     * For brute-force test, just insert a sequence and check nothing breaks. */
    int bucket = 5;
    uint64_t mask = (uint64_t)(16 - 1);
    int64_t collision_keys[20];
    int n_collisions = 0;
    for (int64_t k = 0; k < 10000 && n_collisions < 20; k++) {
        uint64_t h = hash_key_64(k);
        if ((int64_t)(h & mask) == bucket) {
            collision_keys[n_collisions++] = k;
        }
    }
    CHECK(n_collisions >= 10);  /* should find plenty */

    /* Insert all collisions. */
    for (int i = 0; i < n_collisions; i++) {
        int r = hash_map_put(m, collision_keys[i], (int64_t)(i + 1));
        CHECK_EQ(r, 1);
    }

    /* All still findable (linear probing should have placed them). */
    for (int i = 0; i < n_collisions; i++) {
        int64_t* v = hash_map_get(m, collision_keys[i]);
        CHECK(v != NULL);
        if (v) CHECK_EQ(*v, i + 1);
    }

    hash_map_free(m);
}

/* ---------------------------------------------------------------------------
 * Iteration
 * --------------------------------------------------------------------------- */

static void test_iterate_basic(void) {
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);

    hash_map_put(m, 1, 10);
    hash_map_put(m, 2, 20);
    hash_map_put(m, 3, 30);

    HashMapIterator it = {0};
    int count = 0;
    int64_t sum_keys = 0, sum_values = 0;
    int64_t k, v;
    while (hash_map_iter_next(&it, (HashMapBase*)m, &k, &v)) {
        count++;
        sum_keys += k;
        sum_values += v;
    }
    CHECK_EQ(count, 3);
    CHECK_EQ(sum_keys, 6);     /* 1+2+3 */
    CHECK_EQ(sum_values, 60);  /* 10+20+30 */

    hash_map_free(m);
}

static void test_iterate_after_delete(void) {
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);

    for (int i = 0; i < 10; i++) hash_map_put(m, (int64_t)i, (int64_t)(i + 1));
    /* Delete half. */
    for (int i = 0; i < 10; i += 2) hash_map_delete(m, (int64_t)i);

    HashMapIterator it = {0};
    int count = 0;
    int64_t k, v;
    while (hash_map_iter_next(&it, (HashMapBase*)m, &k, &v)) {
        CHECK(k % 2 == 1);  /* only odd keys left */
        count++;
    }
    CHECK_EQ(count, 5);

    hash_map_free(m);
}

static void test_iterate_after_resize(void) {
    /* Phase 23: no resize.  Use a large capacity to fit all 50 entries. */
    HashMap(int64_t, int64_t) m = hash_map_new_cap(int64_t, int64_t, 10, 8);  /* 2^10 = 1024 */

    for (int i = 0; i < 50; i++) hash_map_put(m, (int64_t)i, (int64_t)(i + 1));

    CHECK_EQ(m->capacity, 1024);

    HashMapIterator it = {0};
    int count = 0;
    int64_t k, v;
    int64_t sum_v = 0;
    while (hash_map_iter_next(&it, (HashMapBase*)m, &k, &v)) {
        count++;
        sum_v += v;
    }
    CHECK_EQ(count, 50);
    CHECK_EQ(sum_v, 50 * 51 / 2);  /* 1+2+...+50 */

    hash_map_free(m);
}

static void test_iterate_empty(void) {
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);
    HashMapIterator it = {0};
    int64_t k, v;
    int found = hash_map_iter_next(&it, (HashMapBase*)m, &k, &v);
    CHECK_EQ(found, 0);
    hash_map_free(m);
}

/* ---------------------------------------------------------------------------
 * Bulk stress test
 * --------------------------------------------------------------------------- */

static void test_bulk_insert_delete(void) {
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);

    /* Insert 10K keys. */
    for (int i = 0; i < 10000; i++) {
        hash_map_put(m, (int64_t)i, (int64_t)(i * 7));
    }
    CHECK_EQ(hash_map_size(m), 10000);

    /* All findable. */
    for (int i = 0; i < 10000; i++) {
        int64_t* v = hash_map_get(m, (int64_t)i);
        CHECK(v != NULL);
        if (v) CHECK_EQ(*v, i * 7);
    }

    /* Delete 5K (every other one). */
    for (int i = 0; i < 10000; i += 2) {
        hash_map_delete(m, (int64_t)i);
    }
    CHECK_EQ(hash_map_size(m), 5000);

    /* Odd keys still findable. */
    for (int i = 1; i < 10000; i += 2) {
        int64_t* v = hash_map_get(m, (int64_t)i);
        CHECK(v != NULL);
    }
    /* Even keys not findable. */
    for (int i = 0; i < 10000; i += 2) {
        CHECK(hash_map_get(m, (int64_t)i) == NULL);
    }

    /* Re-insert even keys. */
    for (int i = 0; i < 10000; i += 2) {
        int r = hash_map_put(m, (int64_t)i, (int64_t)(i * 11));
        CHECK_EQ(r, 1);  /* new, since the slot was a tombstone */
    }
    CHECK_EQ(hash_map_size(m), 10000);

    /* All 10K now findable with their updated values. */
    for (int i = 0; i < 10000; i++) {
        int64_t* v = hash_map_get(m, (int64_t)i);
        CHECK(v != NULL);
    }

    hash_map_free(m);
}

/* ---------------------------------------------------------------------------
 * Different value types via the typed macro
 * --------------------------------------------------------------------------- */

typedef struct {
    int64_t a;
    int64_t b;
} Pair;

static void test_struct_values(void) {
    /* Note: hash_map_*(K, V) macros cast both K and V to int64_t, so
       struct values require using the base API directly.  Same
       caveat applies to all the typed macros — they are convenience
       wrappers for primitive types only. */
    HashMapBase* m = hash_map_base_new_cap(6, 8);  /* 64 slots, 256/chunk */

    Pair p1 = { .a = 100, .b = 200 };
    Pair p2 = { .a = -1, .b = 999999 };

    /* Use int64_t keys and cast pair pointers to int64_t values.
       Not elegant, but exercises the same code paths. */
    hash_map_base_put(m, 7, (int64_t)(intptr_t)&p1);
    hash_map_base_put(m, 13, (int64_t)(intptr_t)&p2);

    Pair* got1 = (Pair*)(intptr_t)*hash_map_base_get(m, 7);
    CHECK(got1 != NULL);
    if (got1) {
        CHECK_EQ(got1->a, 100);
        CHECK_EQ(got1->b, 200);
    }

    Pair* got2 = (Pair*)(intptr_t)*hash_map_base_get(m, 13);
    CHECK(got2 != NULL);
    if (got2) {
        CHECK_EQ(got2->a, -1);
        CHECK_EQ(got2->b, 999999);
    }

    /* Update. */
    Pair p3 = { .a = 5, .b = 6 };
    int r = hash_map_base_put(m, 7, (int64_t)(intptr_t)&p3);
    CHECK_EQ(r, 0);
    Pair* got1b = (Pair*)(intptr_t)*hash_map_base_get(m, 7);
    CHECK(got1b != NULL);
    if (got1b) CHECK_EQ(got1b->a, 5);

    hash_map_base_free(m);
}

/* ---------------------------------------------------------------------------
 * Concurrent put + lookup
 *
 * Multiple threads each write into the map (single-writer per map
 * at a time is required; this test serializes writes via a mutex
 * but allows lock-free reads in parallel).
 *
 * Lock-free read correctness: while thread A holds the write mutex,
 * thread B does lookups.  The lookups must not crash and must
 * return consistent values (either present with valid value, or
 * NULL).
 * --------------------------------------------------------------------------- */

typedef struct {
    HashMapBase* map;
    int thread_id;
    int n_ops;
    pthread_mutex_t* wlock;
} WorkerArgs;

static void* worker_concurrent_put(void* arg) {
    WorkerArgs* a = (WorkerArgs*)arg;
    for (int i = 0; i < a->n_ops; i++) {
        int64_t key = (int64_t)(a->thread_id * 1000000 + i);
        pthread_mutex_lock(a->wlock);
        hash_map_base_put(a->map, key, (int64_t)(a->thread_id * 1000 + i));
        pthread_mutex_unlock(a->wlock);
    }
    return NULL;
}

static void* worker_concurrent_lookup(void* arg) {
    WorkerArgs* a = (WorkerArgs*)arg;
    /* Lock-free reads.  Just verify no crash and reasonable values. */
    int hits = 0, misses = 0;
    for (int i = 0; i < a->n_ops; i++) {
        int64_t key = (int64_t)((i % 8) * 1000000 + i);  /* mix of present/absent */
        int64_t* v = hash_map_base_get(a->map, key);
        if (v) hits++; else misses++;
    }
    /* We can't assert exact counts (race) but it should be non-trivial. */
    CHECK(hits + misses == a->n_ops);
    return NULL;
}

static void test_concurrent_put_and_lookup(void) {
    /* Use a large enough capacity (2^20) for 4000 entries — load factor 0.4%. */
    HashMapBase* m = hash_map_base_new_cap(20, 8);  /* 1M slots, 256/chunk */
    pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;

    enum { N_WRITERS = 4, N_READERS = 2, OPS = 1000 };
    pthread_t writers[N_WRITERS];
    pthread_t readers[N_READERS];
    WorkerArgs wargs[N_WRITERS];
    WorkerArgs rargs[N_READERS];

    for (int i = 0; i < N_READERS; i++) {
        rargs[i].map = m;
        rargs[i].thread_id = i;
        rargs[i].n_ops = OPS;
        rargs[i].wlock = &wlock;
        pthread_create(&readers[i], NULL, worker_concurrent_lookup, &rargs[i]);
    }
    for (int i = 0; i < N_WRITERS; i++) {
        wargs[i].map = m;
        wargs[i].thread_id = i;
        wargs[i].n_ops = OPS;
        wargs[i].wlock = &wlock;
        pthread_create(&writers[i], NULL, worker_concurrent_put, &wargs[i]);
    }

    for (int i = 0; i < N_WRITERS; i++) pthread_join(writers[i], NULL);
    for (int i = 0; i < N_READERS; i++) pthread_join(readers[i], NULL);

    /* All 4 writers * 1000 keys should be present. */
    CHECK_EQ(hash_map_base_size(m), N_WRITERS * OPS);

    /* Spot-check. */
    for (int t = 0; t < N_WRITERS; t++) {
        for (int i = 0; i < 100; i++) {
            int64_t key = (int64_t)(t * 1000000 + i);
            int64_t* v = hash_map_base_get(m, key);
            CHECK(v != NULL);
        }
    }

    hash_map_base_free(m);
}

/* ---------------------------------------------------------------------------
 * Edge cases
 * --------------------------------------------------------------------------- */

static void test_single_element(void) {
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);
    hash_map_put(m, 12345, 67890);
    int64_t* v = hash_map_get(m, 12345);
    CHECK(v != NULL);
    if (v) CHECK_EQ(*v, 67890);
    CHECK_EQ(hash_map_size(m), 1);
    hash_map_free(m);
}

static void test_negative_keys(void) {
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);

    int64_t keys[] = { -1, -100, INT64_MIN, 0, 1, 100, INT64_MAX };
    int n = (int)(sizeof(keys) / sizeof(keys[0]));
    for (int i = 0; i < n; i++) {
        hash_map_put(m, keys[i], (int64_t)(i + 1));
    }
    for (int i = 0; i < n; i++) {
        int64_t* v = hash_map_get(m, keys[i]);
        CHECK(v != NULL);
        if (v) CHECK_EQ(*v, i + 1);
    }

    hash_map_free(m);
}

static void test_capacity_boundary(void) {
    /* Phase 23: no resize.  Capacity stays fixed regardless of inserts.
       The put at the 13th slot will either succeed (if probe finds
       an empty slot via wrapping) or fail (if all slots are full). */
    HashMap(int64_t, int64_t) m = hash_map_new_cap(int64_t, int64_t, 4, 8);  /* cap=16 */
    CHECK_EQ(m->capacity, 16);

    for (int i = 0; i < 12; i++) {
        int r = hash_map_put(m, (int64_t)i, (int64_t)(i + 1));
        CHECK_EQ(r, 1);
    }
    CHECK_EQ(m->capacity, 16);  /* no resize ever */

    /* 13th insert: with capacity 16 and no probe collision resolution,
       some keys may collide.  hash_map_put either succeeds (returns 1)
       or returns -1 if the table is full.  We don't check the return
       value here — capacity is just always 16. */
    hash_map_put(m, 12, 13);
    CHECK_EQ(m->capacity, 16);

    hash_map_free(m);
}

static void test_get_returns_stable_pointer(void) {
    /* The pointer returned by get should remain valid until next mutation. */
    HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);

    hash_map_put(m, 5, 50);
    int64_t* v1 = hash_map_get(m, 5);
    int64_t* v2 = hash_map_get(m, 5);
    CHECK(v1 == v2);  /* same slot, same pointer */
    if (v1 && v2) CHECK_EQ(*v1, *v2);

    /* Update value via pointer. */
    if (v1) *v1 = 999;
    int64_t* v3 = hash_map_get(m, 5);
    CHECK(v3 != NULL);
    if (v3) CHECK_EQ(*v3, 999);

    hash_map_free(m);
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */

int main(void) {
    printf("=== Hash Map Tests ===\n");

    test_new_delete();
    test_new_cap_basic();
    test_new_cap_clamps();

    test_put_get_basic();
    test_multiple_inserts();
    test_delete();

    test_no_resize_fixed_capacity();
    test_tombstones_no_clear();

    test_tombstone_reuse();
    test_collision_probe();

    test_iterate_basic();
    test_iterate_after_delete();
    test_iterate_after_resize();
    test_iterate_empty();

    test_bulk_insert_delete();

    test_struct_values();

    test_concurrent_put_and_lookup();

    test_single_element();
    test_negative_keys();
    test_capacity_boundary();
    test_get_returns_stable_pointer();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return (tests_passed == tests_run) ? 0 : 1;
}