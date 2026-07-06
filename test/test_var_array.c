/* Phase 16: VarArray tests */
#include "var_array.h"
#include <stdio.h>
#include <pthread.h>

static int tests_run = 0, tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

/* Portable spin-wait barrier for concurrent tests (pattern from test_pool.c) */
static volatile int s_mt_ready = 0;
static void mt_wait_at_barrier(void) { while (!s_mt_ready) { /* spin */ } }
static void mt_release_barrier(void) { s_mt_ready = 1; }
static void mt_reset_barrier(void) { s_mt_ready = 0; }

/* ---------------------------------------------------------------------------
 * Basic lifecycle tests
 * --------------------------------------------------------------------------- */

static void test_new_delete(void) {
    VarArrayBase* a = var_array_new_base(sizeof(int), VFS_VAR_ARRAY_DEFAULT_CHUNK_SIZE);
    CHECK(a != NULL);
    CHECK_EQ(a->chunk_size, VFS_VAR_ARRAY_DEFAULT_CHUNK_SIZE);
    CHECK_EQ(a->count, 0);
    var_array_delete_base(a);
}

static void test_new_min_chunk(void) {
    VarArrayBase* a = var_array_new_base(sizeof(int), 1);  /* below MIN -> clamped to 16 */
    CHECK(a != NULL);
    CHECK_EQ(a->chunk_size, VFS_VAR_ARRAY_MIN_CHUNK_SIZE);
    var_array_delete_base(a);
}

static void test_new_max_chunk(void) {
    VarArrayBase* a = var_array_new_base(sizeof(int), 99999);  /* above MAX -> clamped */
    CHECK(a != NULL);
    CHECK_EQ(a->chunk_size, VFS_VAR_ARRAY_MAX_CHUNK_SIZE);
    var_array_delete_base(a);
}

/* ---------------------------------------------------------------------------
 * Append (grow + resolve) tests
 * --------------------------------------------------------------------------- */

static void test_append_basic(void) {
    VarArrayBase* a = var_array_new_base(sizeof(int), 16);
    CHECK(a != NULL);

    /* Append 3 ints */
    for (int i = 0; i < 3; i++) {
        int idx = var_array_grow_base(a);
        CHECK_EQ(idx, i);
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(a, idx);
        CHECK(chunk != NULL);
        ((int*)chunk->entries)[idx % a->chunk_size] = i * 10;
    }

    /* Verify via resolve */
    for (int i = 0; i < 3; i++) {
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(a, i);
        CHECK(chunk != NULL);
        CHECK_EQ(((int*)chunk->entries)[i % a->chunk_size], i * 10);
    }

    var_array_delete_base(a);
}

static void test_append_cross_chunk(void) {
    /* Use chunk_size=4 to force promotion quickly */
    VarArrayBase* a = var_array_new_base(sizeof(int), 4);
    CHECK(a != NULL);

    /* Fill first chunk (indices 0-3) */
    for (int i = 0; i < 4; i++) {
        int idx = var_array_grow_base(a);
        CHECK_EQ(idx, i);
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(a, idx);
        CHECK(chunk != NULL);
        ((int*)chunk->entries)[idx % a->chunk_size] = i * 100;
    }

    /* First entry of second chunk (index 4) */
    int idx = var_array_grow_base(a);
    CHECK_EQ(idx, 4);
    VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(a, idx);
    CHECK(chunk != NULL);
    ((int*)chunk->entries)[idx % a->chunk_size] = 400;

    /* Verify all 5 entries */
    for (int i = 0; i < 5; i++) {
        VarArrayChunk* ch = (VarArrayChunk*)var_array_resolve_base(a, i);
        CHECK(ch != NULL);
        CHECK_EQ(((int*)ch->entries)[i % a->chunk_size], i * 100);
    }

    var_array_delete_base(a);
}

static void test_resolve_out_of_range(void) {
    VarArrayBase* a = var_array_new_base(sizeof(int), 16);
    CHECK(a != NULL);

    CHECK(var_array_resolve_base(a, 0) == NULL);
    CHECK(var_array_resolve_base(a, -1) == NULL);

    int idx = var_array_grow_base(a);
    CHECK_EQ(idx, 0);
    CHECK(var_array_resolve_base(a, 0) != NULL);
    CHECK(var_array_resolve_base(a, 1) == NULL);

    var_array_delete_base(a);
}

static void test_large_append(void) {
    VarArrayBase* a = var_array_new_base(sizeof(int), 16);
    CHECK(a != NULL);

    int N = 1000;
    for (int i = 0; i < N; i++) {
        int idx = var_array_grow_base(a);
        CHECK_EQ(idx, i);
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(a, idx);
        CHECK(chunk != NULL);
        ((int*)chunk->entries)[idx % a->chunk_size] = i;
    }
    for (int i = 0; i < N; i++) {
        VarArrayChunk* ch = (VarArrayChunk*)var_array_resolve_base(a, i);
        CHECK(ch != NULL);
        CHECK_EQ(((int*)ch->entries)[i % a->chunk_size], i);
    }

    var_array_delete_base(a);
}

/* ---------------------------------------------------------------------------
 * Typed macro tests (var_array_new / append / lookup / delete)
 * --------------------------------------------------------------------------- */

typedef struct {
    uint64_t key;
    int64_t  vp;
} DirEntry;

static void test_var_array_basic(void) {
    VarArray(DirEntry) list = var_array_new(DirEntry);
    CHECK(list != NULL);
    CHECK_EQ(list->count, 0);

    /* Append 10 entries via typed macro */
    for (int i = 0; i < 10; i++) {
        DirEntry e = {(uint64_t)(i * 100), (int64_t)(i * 200)};
        int idx = var_array_append(list, e);
        CHECK_EQ(idx, i);
    }
    CHECK_EQ(list->count, 10);

    /* Lookup each entry via typed macro */
    for (int i = 0; i < 10; i++) {
        DirEntry* e = var_array_lookup(list, i);
        CHECK(e != NULL);
        CHECK_EQ(e->key, (uint64_t)(i * 100));
        CHECK_EQ(e->vp, (int64_t)(i * 200));
    }

    /* Update entry 5 */
    DirEntry new_e = {999, 888};
    var_array_update(list, 5, new_e);
    DirEntry* e5 = var_array_lookup(list, 5);
    CHECK(e5 != NULL);
    CHECK_EQ(e5->key, (uint64_t)999);
    CHECK_EQ(e5->vp, (int64_t)888);

    var_array_delete(list);
}

static void test_var_array_chunk_capacity(void) {
    VarArray(int) arr = var_array_new(int);
    CHECK(arr != NULL);

    /* Append exactly chunk_size (256) entries — should not trigger promotion */
    for (int i = 0; i < 256; i++) {
        int val = i + 1;
        int idx = var_array_append(arr, val);
        CHECK_EQ(idx, i);
    }
    CHECK_EQ(arr->count, 256);

    /* All entries retrievable */
    VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base((VarArrayBase*)arr, 0);
    CHECK(chunk != NULL);

    for (int i = 0; i < 256; i++) {
        int* e = var_array_lookup(arr, i);
        CHECK(e != NULL);
        CHECK_EQ(*e, i + 1);
        /* All lookups should return entries in the same chunk */
        VarArrayChunk* c = (VarArrayChunk*)var_array_resolve_base((VarArrayBase*)arr, i);
        CHECK(c == chunk);
    }

    var_array_delete(arr);
}

static void test_var_array_root_promotion_to_level_1(void) {
    VarArray(int) arr = var_array_new(int);
    CHECK(arr != NULL);

    /* Append 257 entries — triggers root promotion from chunk to level */
    for (int i = 0; i < 257; i++) {
        int idx = var_array_append(arr, i + 1);
        CHECK_EQ(idx, i);
    }
    CHECK_EQ(arr->count, 257);

    /* All 257 retrievable */
    for (int i = 0; i < 257; i++) {
        int* e = var_array_lookup(arr, i);
        CHECK(e != NULL);
        CHECK_EQ(*e, i + 1);
    }

    /* Verify entries 0..255 are in level->slots[0] (original chunk)
     * and entry 256 is in level->slots[1] (newly allocated chunk) */
    VarArrayLevel* root = (VarArrayLevel*)arr->root;
    CHECK(root != NULL);
    CHECK(root->height > 0);
    void** slots = (void**)root->slots;
    CHECK(slots[0] != NULL);
    CHECK(slots[1] != NULL);
    CHECK(slots[2] == NULL);  /* only 257 entries → 2 chunks */

    VarArrayChunk* chunk0 = (VarArrayChunk*)slots[0];
    VarArrayChunk* chunk1 = (VarArrayChunk*)slots[1];
    CHECK(chunk0 != chunk1);

    /* Entry 0 in chunk0, entry 256 in chunk1 */
    CHECK_EQ(((int*)chunk0->entries)[0], 1);
    CHECK_EQ(((int*)chunk1->entries)[0], 257);

    var_array_delete(arr);
}

static void test_var_array_multi_level_growth(void) {
    /* chunk_size=16: level-1→256 entries, level-2→4096 entries */
    VarArrayBase* a = var_array_new_base(sizeof(int), 16);
    CHECK(a != NULL);

    /* Append 257 entries — forces level-2 root (16²=256 < 257 ≤ 16³=4096) */
    for (int i = 0; i < 257; i++) {
        int idx = var_array_grow_base(a);
        CHECK_EQ(idx, i);
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(a, idx);
        CHECK(chunk != NULL);
        ((int*)chunk->entries)[idx % a->chunk_size] = i * 10;
    }
    CHECK_EQ(a->count, 257);

    /* Verify root height == 2 */
    int h = var_array_root_height_for_test(a);
    CHECK_EQ(h, 2);

    /* All 257 retrievable */
    for (int i = 0; i < 257; i++) {
        VarArrayChunk* ch = (VarArrayChunk*)var_array_resolve_base(a, i);
        CHECK(ch != NULL);
        CHECK_EQ(((int*)ch->entries)[i % a->chunk_size], i * 10);
    }

    var_array_delete_base(a);
}

static void test_var_array_bulk_10000(void) {
    VarArray(int) arr = var_array_new(int);
    CHECK(arr != NULL);

    int N = 10000;
    for (int i = 0; i < N; i++) {
        int idx = var_array_append(arr, i * 7);
        CHECK_EQ(idx, i);
    }
    CHECK_EQ(arr->count, N);

    /* Spot-check first, middle, last */
    int* first = var_array_lookup(arr, 0);
    CHECK(first != NULL);
    CHECK_EQ(*first, 0);

    int* mid = var_array_lookup(arr, N / 2);
    CHECK(mid != NULL);
    CHECK_EQ(*mid, (N / 2) * 7);

    int* last = var_array_lookup(arr, N - 1);
    CHECK(last != NULL);
    CHECK_EQ(*last, (N - 1) * 7);

    var_array_delete(arr);
}

/* Concurrent append: 4 threads, 250 entries each, shared VarArray */
typedef struct {
    VarArrayBase* arr;
    int           tid;
    int           count;
} va_thread_arg;

static void* va_append_thread(void* arg) {
    va_thread_arg* a = (va_thread_arg*)arg;
    mt_wait_at_barrier();
    for (int i = 0; i < a->count; i++) {
        int val = a->tid * 1000 + i;
        int idx = var_array_grow_base(a->arr);
        if (idx < 0) return (void*)0;
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(a->arr, idx);
        if (!chunk) return (void*)0;
        ((int*)chunk->entries)[idx % a->arr->chunk_size] = val;
    }
    return (void*)1;
}

static void test_var_array_concurrent_append(void) {
    VarArrayBase* arr = var_array_new_base(sizeof(int), 256);
    CHECK(arr != NULL);

    mt_reset_barrier();
    va_thread_arg args[4];
    pthread_t threads[4];
    for (int t = 0; t < 4; t++) {
        args[t].arr   = arr;
        args[t].tid   = t;
        args[t].count = 250;
        CHECK_EQ(pthread_create(&threads[t], NULL, va_append_thread, &args[t]), 0);
    }
    mt_release_barrier();

    void* results[4];
    for (int t = 0; t < 4; t++) {
        pthread_join(threads[t], &results[t]);
        CHECK(results[t] != NULL);
    }
    CHECK_EQ(arr->count, 1000);

    /* Verify all 1000 values present.  Build a bitmask of seen tid*1000+i. */
    int seen[4000] = {0};  /* max val = 3*1000+249 = 3249 */
    for (int i = 0; i < 1000; i++) {
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(arr, i);
        CHECK(chunk != NULL);
        int val = ((int*)chunk->entries)[i % arr->chunk_size];
        CHECK(val >= 0 && val < 4000);
        seen[val] = 1;
    }
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < 250; i++) {
            CHECK(seen[t * 1000 + i]);
        }
    }

    var_array_delete_base(arr);
}

int main(void) {
    printf("=== VarArray Tests ===\n");

    test_new_delete();
    test_new_min_chunk();
    test_new_max_chunk();
    test_append_basic();
    test_append_cross_chunk();
    test_resolve_out_of_range();
    test_large_append();
    test_var_array_basic();
    test_var_array_chunk_capacity();
    test_var_array_root_promotion_to_level_1();
    test_var_array_multi_level_growth();
    test_var_array_bulk_10000();
    test_var_array_concurrent_append();

    printf("test_var_array: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
