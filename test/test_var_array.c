/* Phase 16: VarArray tests */
#include "var_array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    var_array_set(list, 5, new_e);
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

/* Concurrent append + random lookup: 2 appenders, 2 readers */
static void* va_append_bulk_thread(void* arg) {
    va_thread_arg* a = (va_thread_arg*)arg;
    mt_wait_at_barrier();
    for (int i = 0; i < a->count; i++) {
        int val = a->tid * 10000 + i;
        int idx = var_array_grow_base(a->arr);
        if (idx < 0) return (void*)0;
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(a->arr, idx);
        if (!chunk) return (void*)0;
        ((int*)chunk->entries)[idx % a->arr->chunk_size] = val;
    }
    return (void*)1;
}

static void* va_random_lookup_thread(void* arg) {
    VarArrayBase* arr = (VarArrayBase*)arg;
    mt_wait_at_barrier();
    unsigned rseed = 42;
    for (int i = 0; i < 10000; i++) {
        int count = arr->count;
        if (count == 0) continue;
        int idx = rand_r(&rseed) % count;
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(arr, idx);
        if (chunk) {
            volatile int val = ((int*)chunk->entries)[idx % arr->chunk_size];
            (void)val;  /* reader may get any value or NULL — no assertions */
        }
    }
    return (void*)1;
}

static void test_var_array_concurrent_append_and_lookup(void) {
    VarArrayBase* arr = var_array_new_base(sizeof(int), 256);
    CHECK(arr != NULL);

    mt_reset_barrier();
    va_thread_arg a0 = {arr, 0, 5000};
    va_thread_arg a1 = {arr, 1, 5000};
    pthread_t t[4];
    CHECK_EQ(pthread_create(&t[0], NULL, va_append_bulk_thread, &a0), 0);
    CHECK_EQ(pthread_create(&t[1], NULL, va_append_bulk_thread, &a1), 0);
    CHECK_EQ(pthread_create(&t[2], NULL, va_random_lookup_thread, arr), 0);
    CHECK_EQ(pthread_create(&t[3], NULL, va_random_lookup_thread, arr), 0);
    mt_release_barrier();

    void* results[4];
    for (int i = 0; i < 4; i++) {
        pthread_join(t[i], &results[i]);
        CHECK(results[i] != NULL);
    }
    CHECK_EQ(arr->count, 10000);

    /* Verify all 10000 values present */
    int seen[20000] = {0};
    for (int i = 0; i < 10000; i++) {
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(arr, i);
        CHECK(chunk != NULL);
        int val = ((int*)chunk->entries)[i % arr->chunk_size];
        CHECK(val >= 0 && val < 20000);
        seen[val] = 1;
    }
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 5000; i++)
            CHECK(seen[t * 10000 + i]);

    var_array_delete_base(arr);
}

static void test_var_array_lookup_out_of_range(void) {
    VarArray(int) arr = var_array_new(int);
    CHECK(arr != NULL);

    /* Append 10 entries */
    for (int i = 0; i < 10; i++) {
        int idx = var_array_append(arr, i);
        CHECK_EQ(idx, i);
    }

    /* Out-of-range lookups return NULL */
    CHECK(var_array_lookup(arr, 500) == NULL);
    CHECK(var_array_lookup(arr, -1) == NULL);

    var_array_delete(arr);
}

static void test_var_array_custom_chunk_size(void) {
    /* chunk_size=64: level-1→4096 entries, 4097 forces level-2 root */
    VarArrayBase* a = var_array_new_base(sizeof(int), 64);
    CHECK(a != NULL);
    CHECK_EQ(a->chunk_size, 64);

    int N = 4097;  /* 64² + 1 */
    for (int i = 0; i < N; i++) {
        int idx = var_array_grow_base(a);
        CHECK_EQ(idx, i);
        VarArrayChunk* chunk = (VarArrayChunk*)var_array_resolve_base(a, idx);
        CHECK(chunk != NULL);
        ((int*)chunk->entries)[idx % a->chunk_size] = i;
    }
    CHECK_EQ(a->count, N);

    /* Verify root height == 2 */
    int h = var_array_root_height_for_test(a);
    CHECK_EQ(h, 2);

    /* Spot-check first, last, boundary */
    VarArrayChunk* ch0 = (VarArrayChunk*)var_array_resolve_base(a, 0);
    CHECK(ch0 != NULL);
    CHECK_EQ(((int*)ch0->entries)[0], 0);

    VarArrayChunk* ch4095 = (VarArrayChunk*)var_array_resolve_base(a, 4095);
    CHECK(ch4095 != NULL);
    CHECK_EQ(((int*)ch4095->entries)[4095 % 64], 4095);

    VarArrayChunk* ch4096 = (VarArrayChunk*)var_array_resolve_base(a, 4096);
    CHECK(ch4096 != NULL);
    CHECK_EQ(((int*)ch4096->entries)[4096 % 64], 4096);

    var_array_delete_base(a);
}

static void test_var_array_set_in_place(void) {
    VarArray(int) arr = var_array_new(int);
    CHECK(arr != NULL);

    /* Append 300 entries — forces promotion (256 < 300) */
    for (int i = 0; i < 300; i++) {
        int idx = var_array_append(arr, i);
        CHECK_EQ(idx, i);
    }

    /* Update first and last entries */
    var_array_set(arr, 0, 999);
    var_array_set(arr, 299, 888);

    /* Verify via lookup */
    int* e0 = var_array_lookup(arr, 0);
    CHECK(e0 != NULL);
    CHECK_EQ(*e0, 999);

    int* e299 = var_array_lookup(arr, 299);
    CHECK(e299 != NULL);
    CHECK_EQ(*e299, 888);

    /* Middle entry unchanged */
    int* e150 = var_array_lookup(arr, 150);
    CHECK(e150 != NULL);
    CHECK_EQ(*e150, 150);

    var_array_delete(arr);
}

static void test_var_array_delete_no_leak(void) {
    /* Append 1000 entries, then delete.  Run under valgrind to confirm
     * no leaks — the struct and all internal allocations are freed. */
    VarArray(int) arr = var_array_new(int);
    CHECK(arr != NULL);

    for (int i = 0; i < 1000; i++) {
        int idx = var_array_append(arr, i);
        CHECK_EQ(idx, i);
    }
    CHECK_EQ(arr->count, 1000);
    var_array_delete(arr);
}

/* ---------------------------------------------------------------------------
 * Sparse-mode tests (Phase 22)
 *
 * var_array_set / var_array_unset can write to any idx in [0, capacity),
 * not just sequential positions.  The tree path is allocated lazily —
 * siblings stay NULL, only the path from root to leaf chunk is allocated.
 * --------------------------------------------------------------------------- */

static void test_set_basic_at_index(void) {
    /* Set at idx 0, before any grow.  Allocates the existing chunk path. */
    VarArray(int) arr = var_array_new(int);
    var_array_set(arr, 0, 42);
    int* p = var_array_lookup(arr, 0);
    CHECK(p != NULL);
    if (p) CHECK_EQ(*p, 42);
    CHECK_EQ(arr->count, 1);
    var_array_delete(arr);
}

static void test_set_at_idx_beyond_count(void) {
    /* Set at idx 100 without any prior grows.  Allocates a deep tree path,
       jumps count to 101.  Note: with chunk_size=16, all slots in the
       same chunk as a set idx share the chunk and are addressable.  Slots
       in chunks that were never allocated return NULL. */
    VarArray(int) arr = (VarArray(int))var_array_new_base(sizeof(int), 16);
    int v = 12345;
    var_array_set(arr, 100, v);
    CHECK_EQ(arr->count, 101);

    /* idx 100 is set, in chunk 6. */
    int* p = var_array_lookup(arr, 100);
    CHECK(p != NULL);
    if (p) CHECK_EQ(*p, 12345);

    /* idx 50 is in chunk 3 — chunk never allocated -> NULL. */
    CHECK(var_array_lookup(arr, 50) == NULL);
    /* idx 32 in chunk 2 — NULL. */
    CHECK(var_array_lookup(arr, 32) == NULL);

    var_array_delete(arr);
}

static void test_set_creates_holes(void) {
    /* Set at sparse positions that span multiple chunks.  We use a
       small chunk_size so chunks don't share. */
    VarArray(int) arr = (VarArray(int))var_array_new_base(sizeof(int), 16);

    int v0 = 10, v1 = 20, v2 = 30, v3 = 40;
    var_array_set(arr, 0,   v0);    /* chunk 0 */
    var_array_set(arr, 16,  v1);    /* chunk 1 */
    var_array_set(arr, 100, v2);    /* chunk 6 */
    var_array_set(arr, 200, v3);    /* chunk 12 */

    /* count is high-water mark. */
    CHECK_EQ(arr->count, 201);

    /* All set values findable. */
    int* p;
    p = var_array_lookup(arr, 0);
    CHECK(p != NULL); if (p) CHECK_EQ(*p, 10);
    p = var_array_lookup(arr, 16);
    CHECK(p != NULL); if (p) CHECK_EQ(*p, 20);
    p = var_array_lookup(arr, 100);
    CHECK(p != NULL); if (p) CHECK_EQ(*p, 30);
    p = var_array_lookup(arr, 200);
    CHECK(p != NULL); if (p) CHECK_EQ(*p, 40);

    /* Slots in chunks that were NEVER allocated return NULL. */
    CHECK(var_array_lookup(arr, 32)  == NULL);  /* chunk 2 unallocated */
    CHECK(var_array_lookup(arr, 80)  == NULL);  /* chunk 5 unallocated */
    CHECK(var_array_lookup(arr, 112) == NULL);  /* chunk 7 unallocated */
    CHECK(var_array_lookup(arr, 150) == NULL);  /* chunk 9 unallocated */

    var_array_delete(arr);
}

static void test_set_overwrites_existing(void) {
    /* Set at the same idx twice: second set overwrites the first. */
    VarArray(int) arr = var_array_new(int);
    var_array_set(arr, 5, 100);
    var_array_set(arr, 5, 200);
    CHECK_EQ(*var_array_lookup(arr, 5), 200);
    /* count stays at 6 (didn't jump on overwrite). */
    CHECK_EQ(arr->count, 6);
    var_array_delete(arr);
}

static void test_set_with_append_mixed(void) {
    /* Mix append (sequential) and set (sparse).  Both update count. */
    VarArray(int) arr = var_array_new(int);
    var_array_append(arr, 1);
    var_array_append(arr, 2);
    var_array_append(arr, 3);   /* count = 3 */
    var_array_set(arr, 10, 99); /* count jumps to 11 */
    var_array_append(arr, 4);   /* count becomes 12 (sets idx 11) */

    CHECK_EQ(arr->count, 12);
    int* p;

    /* All explicitly-set values findable with correct content. */
    p = var_array_lookup(arr, 0);
    CHECK(p != NULL); if (p) CHECK_EQ(*p, 1);

    p = var_array_lookup(arr, 1);
    CHECK(p != NULL); if (p) CHECK_EQ(*p, 2);

    p = var_array_lookup(arr, 2);
    CHECK(p != NULL); if (p) CHECK_EQ(*p, 3);

    p = var_array_lookup(arr, 10);
    CHECK(p != NULL); if (p) CHECK_EQ(*p, 99);

    p = var_array_lookup(arr, 11);
    CHECK(p != NULL); if (p) CHECK_EQ(*p, 4);

    /* Slots 3-9 are in the same chunk (chunk_size=256) as idx 0,1,2, so
       the chunk is allocated.  These slots return non-NULL with value 0
       (calloc'd, never written).  This is the inherent limitation of
       sparse arrays: the chunk tree doesn't track per-slot "set" state. */
    for (int i = 3; i < 10; i++) {
        int* q = var_array_lookup(arr, i);
        CHECK(q != NULL);   /* chunk allocated */
        if (q) CHECK_EQ(*q, 0);  /* never set, so zero */
    }
    var_array_delete(arr);
}

static void test_set_unset_clears_slot(void) {
    /* Set, then unset: slot is cleared (all-zero). */
    VarArray(int) arr = var_array_new(int);
    var_array_set(arr, 7, 999);
    CHECK_EQ(*var_array_lookup(arr, 7), 999);

    var_array_unset(arr, 7);
    /* Slot exists (calloc'd to 0), but lookup returns the zero value. */
    int* p = var_array_lookup(arr, 7);
    CHECK(p != NULL);  /* slot exists (path was allocated) */
    if (p) CHECK_EQ(*p, 0);
    /* count doesn't shrink on unset. */
    CHECK_EQ(arr->count, 8);

    var_array_delete(arr);
}

static void test_unset_allocates_path(void) {
    /* var_array_unset(arr, idx) clears the slot at idx.  If the slot
       doesn't exist (chunk not allocated), unset is a no-op — it does
       NOT allocate the path or bump count.  This matches the chosen
       implementation that checks existence before clearing. */
    VarArray(int) arr = var_array_new(int);
    var_array_append(arr, 1);
    var_array_append(arr, 2);
    /* idx 50 was never set.  unset is no-op. */
    var_array_unset(arr, 50);
    CHECK_EQ(arr->count, 2);  /* count unchanged */

    /* idx 50 still NULL (chunk never allocated). */
    CHECK(var_array_lookup(arr, 50) == NULL);

    /* Now set idx 50 — allocates the path. */
    var_array_set(arr, 50, 999);
    CHECK_EQ(arr->count, 51);
    int* p = var_array_lookup(arr, 50);
    CHECK(p != NULL);
    if (p) CHECK_EQ(*p, 999);

    /* Unset clears the slot. */
    var_array_unset(arr, 50);
    p = var_array_lookup(arr, 50);
    CHECK(p != NULL);   /* chunk still allocated */
    if (p) CHECK_EQ(*p, 0);

    /* Count didn't change (no shrink on unset). */
    CHECK_EQ(arr->count, 51);

    /* Set then unset then set — exercises the lifecycle. */
    var_array_set(arr, 50, 555);
    p = var_array_lookup(arr, 50);
    CHECK(p != NULL);
    if (p) CHECK_EQ(*p, 555);

    var_array_delete(arr);
}

static void test_set_at_idx_forces_rebalance(void) {
    /* Default chunk_size is 256.  Set at idx 1000 forces root promotion
       from chunk to level node. */
    VarArray(int) arr = var_array_new(int);
    var_array_set(arr, 1000, 42);
    CHECK_EQ(arr->count, 1001);
    int* p = var_array_lookup(arr, 1000);
    CHECK(p != NULL);
    if (p) CHECK_EQ(*p, 42);

    /* Set at a different high idx to confirm tree is reusable. */
    var_array_set(arr, 500, 99);
    CHECK_EQ(*var_array_lookup(arr, 500), 99);
    /* count is max of both ops. */
    CHECK_EQ(arr->count, 1001);

    var_array_delete(arr);
}

static void test_set_with_typed_struct(void) {
    /* set works with arbitrary typed entries, not just int. */
    typedef struct { int64_t a; int64_t b; } Pair;
    VarArray(Pair) arr = var_array_new(Pair);

    Pair p1 = { .a = 100, .b = 200 };
    Pair p2 = { .a = -1, .b = 999999 };
    var_array_set(arr, 50, p1);
    var_array_set(arr, 100, p2);

    Pair* got1 = var_array_lookup(arr, 50);
    CHECK(got1 != NULL);
    if (got1) {
        CHECK_EQ(got1->a, 100);
        CHECK_EQ(got1->b, 200);
    }

    Pair* got2 = var_array_lookup(arr, 100);
    CHECK(got2 != NULL);
    if (got2) {
        CHECK_EQ(got2->a, -1);
        CHECK_EQ(got2->b, 999999);
    }

    var_array_delete(arr);
}

static void test_set_holes_then_append(void) {
    /* Set sparse slots, then append — append goes after the high-water mark. */
    VarArray(int) arr = var_array_new(int);
    var_array_set(arr, 50, 999);
    /* count = 51.  Append at idx 51. */
    int idx = var_array_append(arr, 12345);
    CHECK_EQ(idx, 51);
    CHECK_EQ(*var_array_lookup(arr, 51), 12345);
    /* The sparse slot at 50 is preserved. */
    CHECK_EQ(*var_array_lookup(arr, 50), 999);
    var_array_delete(arr);
}

static void test_set_allocates_only_path(void) {
    /* Pick set indices that fall in DIFFERENT chunks (chunk_size apart)
       and verify that lookup in chunks that were never allocated
       returns NULL.  Use a small chunk_size so we can reason about
       chunk boundaries. */
    VarArray(int) arr = (VarArray(int))var_array_new_base(sizeof(int), 16);

    /* Set in chunks 0, 1, 6, 62 — leave chunks 2-5 and 7-61 unallocated. */
    var_array_set(arr, 0,   100);   /* chunk 0 */
    var_array_set(arr, 16,  200);   /* chunk 1 */
    var_array_set(arr, 96,  300);   /* chunk 6 */
    var_array_set(arr, 992, 400);   /* chunk 62 */

    CHECK_EQ(arr->count, 993);

    /* All set values findable. */
    int* p;
    p = var_array_lookup(arr, 0);   CHECK(p != NULL); if (p) CHECK_EQ(*p, 100);
    p = var_array_lookup(arr, 16);  CHECK(p != NULL); if (p) CHECK_EQ(*p, 200);
    p = var_array_lookup(arr, 96);  CHECK(p != NULL); if (p) CHECK_EQ(*p, 300);
    p = var_array_lookup(arr, 992); CHECK(p != NULL); if (p) CHECK_EQ(*p, 400);

    /* Lookup in chunks that were NEVER allocated returns NULL.
       (resolve_base returns NULL for unallocated chunks; lookup macro
       forwards that.) */
    CHECK(var_array_lookup(arr, 32)  == NULL);  /* chunk 2 unallocated */
    CHECK(var_array_lookup(arr, 50)  == NULL);  /* chunk 3 unallocated */
    CHECK(var_array_lookup(arr, 80)  == NULL);  /* chunk 5 unallocated */
    CHECK(var_array_lookup(arr, 112) == NULL);  /* chunk 7 unallocated */
    CHECK(var_array_lookup(arr, 500) == NULL);  /* chunk 31 unallocated */

    /* Intermediate slots WITHIN an allocated chunk return non-NULL
       (chunk was allocated) but contain 0 (calloc'd, never written). */
    p = var_array_lookup(arr, 5);    /* chunk 0 (allocated for idx 0) */
    CHECK(p != NULL);
    if (p) CHECK_EQ(*p, 0);

    var_array_delete(arr);
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
    test_var_array_concurrent_append_and_lookup();
    test_var_array_lookup_out_of_range();
    test_var_array_custom_chunk_size();
    test_var_array_set_in_place();
    test_var_array_delete_no_leak();

    /* Sparse-mode tests (Phase 22) */
    test_set_basic_at_index();
    test_set_at_idx_beyond_count();
    test_set_creates_holes();
    test_set_overwrites_existing();
    test_set_with_append_mixed();
    test_set_unset_clears_slot();
    test_unset_allocates_path();
    test_set_at_idx_forces_rebalance();
    test_set_with_typed_struct();
    test_set_holes_then_append();
    test_set_allocates_only_path();

    printf("test_var_array: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
