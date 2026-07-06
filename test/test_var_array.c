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
        void* slot = var_array_resolve_base(a, idx);
        CHECK(slot != NULL);
        *(int*)slot = i * 10;
    }

    /* Verify via resolve */
    for (int i = 0; i < 3; i++) {
        int* slot = (int*)var_array_resolve_base(a, i);
        CHECK(slot != NULL);
        CHECK_EQ(*slot, i * 10);
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
        int* slot = (int*)var_array_resolve_base(a, idx);
        CHECK(slot != NULL);
        *slot = i * 100;
    }

    /* First entry of second chunk (index 4) */
    int idx = var_array_grow_base(a);
    CHECK_EQ(idx, 4);
    int* slot = (int*)var_array_resolve_base(a, idx);
    CHECK(slot != NULL);
    *slot = 400;

    /* Verify all 5 entries */
    for (int i = 0; i < 5; i++) {
        int* s = (int*)var_array_resolve_base(a, i);
        CHECK(s != NULL);
        CHECK_EQ(*s, i * 100);
    }

    var_array_delete_base(a);
}

static void test_resolve_out_of_range(void) {
    VarArrayBase* a = var_array_new_base(sizeof(int), 16);
    CHECK(a != NULL);

    /* Nothing claimed yet — resolve should return NULL */
    CHECK(var_array_resolve_base(a, 0) == NULL);
    CHECK(var_array_resolve_base(a, -1) == NULL);

    /* Claim slot 0 */
    int idx = var_array_grow_base(a);
    CHECK_EQ(idx, 0);

    /* Slot 0 works, slot 1 still NULL */
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
        int* slot = (int*)var_array_resolve_base(a, idx);
        CHECK(slot != NULL);
        *slot = i;
    }
    /* Verify */
    for (int i = 0; i < N; i++) {
        int* s = (int*)var_array_resolve_base(a, i);
        CHECK(s != NULL);
        CHECK_EQ(*s, i);
    }

    var_array_delete_base(a);
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

    printf("test_var_array: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
