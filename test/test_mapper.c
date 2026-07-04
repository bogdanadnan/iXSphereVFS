/* Phase 6: Mapper unit tests */
#include "mapper.h"
#include "ixsphere/vfs_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int tests_run = 0, tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

static const char* test_path = "/tmp/test_mapper.tmp";

static StorageBackend* mapper_setup(void) {
    unlink(test_path);
    return storage_open(test_path, 8192);
}

static void mapper_teardown(StorageBackend* sb) {
    if (sb) storage_close(sb);
    unlink(test_path);
}

static void test_mapper_init(void) {
    StorageBackend* sb = mapper_setup();
    CHECK(sb != NULL);

    int64_t list_head = 0;
    Pool pool;
    pool_init(&pool, sb, &list_head);

    int64_t chain_head = 0;
    Mapper m;
    mapper_init(&m, &pool, &chain_head);
    CHECK_EQ(m.pool, &pool);
    CHECK_EQ(m.epochMapperPtr, &chain_head);

    /* Empty chain: resolve returns identity */
    CHECK_EQ(mapper_resolve(&m, 0), 0);
    CHECK_EQ(mapper_resolve(&m, 5), 5);

    /* Empty chain: traversal_apply returns false */
    CHECK(!mapper_traversal_apply(&m, 0));

    /* Empty chain: validate returns 0 */
    CHECK_EQ(mapper_validate(&m), 0);

    mapper_teardown(sb);
}

static void test_mapper_insert_resolve(void) {
    StorageBackend* sb = mapper_setup();
    CHECK(sb != NULL);

    int64_t list_head = 0;
    Pool pool;
    pool_init(&pool, sb, &list_head);

    int64_t chain_head = 0;
    Mapper m;
    mapper_init(&m, &pool, &chain_head);

    /* Insert commit mapping: 3→4 with traversalApply */
    int ret = mapper_insert(&m, 3, 4, MAPPER_FLAG_TRAVERSAL_APPLY);
    CHECK_EQ(ret, VFS_OK);

    /* Resolve 3 → 4 */
    CHECK_EQ(mapper_resolve(&m, 3), 4);
    /* Resolve 5 → identity (5 not mapped) */
    CHECK_EQ(mapper_resolve(&m, 5), 5);
    /* Resolve 4 → identity (4 is a target, not a source) */
    CHECK_EQ(mapper_resolve(&m, 4), 4);

    /* traversal_apply for 3 → true */
    CHECK(mapper_traversal_apply(&m, 3));
    /* traversal_apply for 4 → false (4 is not a source) */
    CHECK(!mapper_traversal_apply(&m, 4));

    /* Soft-delete mapping: 7→6 (different target) without traversalApply */
    ret = mapper_insert(&m, 7, 6, 0);
    CHECK_EQ(ret, VFS_OK);

    /* traversal_apply for 7 → false */
    CHECK(!mapper_traversal_apply(&m, 7));

    /* validate returns 2 (3→4, 7→6) */
    CHECK_EQ(mapper_validate(&m), 2);

    mapper_teardown(sb);
}

static void test_mapper_invariants(void) {
    StorageBackend* sb = mapper_setup();
    CHECK(sb != NULL);

    int64_t list_head = 0;
    Pool pool;
    pool_init(&pool, sb, &list_head);

    int64_t chain_head = 0;
    Mapper m;
    mapper_init(&m, &pool, &chain_head);

    /* Baseline: 5→10 */
    CHECK_EQ(mapper_insert(&m, 5, 10, 0), VFS_OK);

    /* Duplicate fromEpoch (5 already a source) → VFS_ERR_EXISTS */
    CHECK_EQ(mapper_insert(&m, 5, 20, 0), VFS_ERR_EXISTS);

    /* Duplicate toEpoch (10 already a target of 5→10) → VFS_ERR_EXISTS */
    CHECK_EQ(mapper_insert(&m, 1, 10, 0), VFS_ERR_EXISTS);

    /* toEpoch as source (15→5: 5 is already a source) → VFS_ERR_EXISTS */
    CHECK_EQ(mapper_insert(&m, 15, 5, 0), VFS_ERR_EXISTS);

    /* fromEpoch as target (10→20: 10 is already a target of 5→10) → VFS_ERR_EXISTS */
    CHECK_EQ(mapper_insert(&m, 10, 20, 0), VFS_ERR_EXISTS);

    /* Valid insert: 1→2 (fresh epochs) */
    CHECK_EQ(mapper_insert(&m, 1, 2, 0), VFS_OK);

    /* Valid insert: 3→6 (fresh epochs) */
    CHECK_EQ(mapper_insert(&m, 3, 6, 0), VFS_OK);

    CHECK_EQ(mapper_validate(&m), 3);

    mapper_teardown(sb);
}

/* ---------------------------------------------------------------------------
 * MapperTable tests
 * --------------------------------------------------------------------------- */

static void test_mapper_table_empty(void) {
    StorageBackend* sb = mapper_setup();
    CHECK(sb != NULL);

    int64_t list_head = 0;
    Pool pool;
    pool_init(&pool, sb, &list_head);

    int64_t chain_head = 0;
    MapperTable tbl;

    /* Empty chain (epochMapperPtr == 0) */
    CHECK_EQ(mapper_table_init(&tbl, &pool, &chain_head), VFS_OK);
    CHECK_EQ(tbl.count, 0);
    CHECK(tbl.entries != NULL);
    CHECK_EQ(tbl.capacity, MAPPER_TABLE_INITIAL_CAPACITY);

    mapper_table_destroy(&tbl);
    CHECK(tbl.entries == NULL);
    CHECK_EQ(tbl.count, 0);
    CHECK_EQ(tbl.capacity, 0);

    /* NULL epochMapperPtr */
    CHECK_EQ(mapper_table_init(&tbl, &pool, NULL), VFS_OK);
    CHECK_EQ(tbl.count, 0);
    mapper_table_destroy(&tbl);

    mapper_teardown(sb);
}

static void test_mapper_table_single(void) {
    StorageBackend* sb = mapper_setup();
    CHECK(sb != NULL);

    int64_t list_head = 0;
    Pool pool;
    pool_init(&pool, sb, &list_head);

    int64_t chain_head = 0;
    Mapper m;
    mapper_init(&m, &pool, &chain_head);

    /* Insert one entry with traversalApply */
    CHECK_EQ(mapper_insert(&m, 1, 2, MAPPER_FLAG_TRAVERSAL_APPLY), VFS_OK);

    MapperTable tbl;
    CHECK_EQ(mapper_table_init(&tbl, &pool, &chain_head), VFS_OK);
    CHECK_EQ(tbl.count, 1);
    CHECK_EQ(tbl.entries[0].fromEpoch, (uint32_t)1);
    CHECK_EQ(tbl.entries[0].toEpoch, (uint32_t)2);
    CHECK(tbl.entries[0].traversalApply);

    mapper_table_destroy(&tbl);
    mapper_teardown(sb);
}

static void test_mapper_table_multi(void) {
    StorageBackend* sb = mapper_setup();
    CHECK(sb != NULL);

    int64_t list_head = 0;
    Pool pool;
    pool_init(&pool, sb, &list_head);

    int64_t chain_head = 0;
    Mapper m;
    mapper_init(&m, &pool, &chain_head);

    CHECK_EQ(mapper_insert(&m, 1, 2, MAPPER_FLAG_TRAVERSAL_APPLY), VFS_OK);
    CHECK_EQ(mapper_insert(&m, 3, 4, 0), VFS_OK);
    CHECK_EQ(mapper_insert(&m, 5, 6, MAPPER_FLAG_TRAVERSAL_APPLY), VFS_OK);

    MapperTable tbl;
    CHECK_EQ(mapper_table_init(&tbl, &pool, &chain_head), VFS_OK);
    CHECK_EQ(tbl.count, 3);

    /* Note: chain is in reverse order (CAS-prepend) */
    CHECK_EQ(tbl.entries[0].fromEpoch, (uint32_t)5);
    CHECK_EQ(tbl.entries[0].toEpoch, (uint32_t)6);
    CHECK(tbl.entries[0].traversalApply);

    CHECK_EQ(tbl.entries[1].fromEpoch, (uint32_t)3);
    CHECK_EQ(tbl.entries[1].toEpoch, (uint32_t)4);
    CHECK(!tbl.entries[1].traversalApply);

    CHECK_EQ(tbl.entries[2].fromEpoch, (uint32_t)1);
    CHECK_EQ(tbl.entries[2].toEpoch, (uint32_t)2);
    CHECK(tbl.entries[2].traversalApply);

    mapper_table_destroy(&tbl);
    mapper_teardown(sb);
}

/* ---------------------------------------------------------------------------
 * MapperTable rebuild tests
 * --------------------------------------------------------------------------- */

static void test_mapper_rebuild_empty(void) {
    StorageBackend* sb = mapper_setup();
    CHECK(sb != NULL);

    int64_t list_head = 0;
    Pool pool;
    pool_init(&pool, sb, &list_head);

    int64_t chain_head = 0;
    MapperTable tbl;
    CHECK_EQ(mapper_table_init(&tbl, &pool, &chain_head), VFS_OK);
    CHECK(tbl.entries != NULL);
    CHECK_EQ(tbl.capacity, MAPPER_TABLE_INITIAL_CAPACITY);

    /* Rebuild on empty chain — post-conditions must match init */
    CHECK_EQ(mapper_table_rebuild(&tbl), VFS_OK);
    CHECK(tbl.entries != NULL);
    CHECK_EQ(tbl.count, 0);
    CHECK_EQ(tbl.capacity, MAPPER_TABLE_INITIAL_CAPACITY);

    /* Insert must work after empty-chain rebuild */
    CHECK_EQ(mapper_table_insert(&tbl, 1, 2, true), VFS_OK);
    CHECK_EQ(tbl.count, 1);
    CHECK_EQ(tbl.entries[0].fromEpoch, (uint32_t)1);

    mapper_table_destroy(&tbl);
    mapper_teardown(sb);
}

static void test_mapper_rebuild_with_entries(void) {
    StorageBackend* sb = mapper_setup();
    CHECK(sb != NULL);

    int64_t list_head = 0;
    Pool pool;
    pool_init(&pool, sb, &list_head);

    int64_t chain_head = 0;
    Mapper m;
    mapper_init(&m, &pool, &chain_head);
    CHECK_EQ(mapper_insert(&m, 1, 2, MAPPER_FLAG_TRAVERSAL_APPLY), VFS_OK);
    CHECK_EQ(mapper_insert(&m, 3, 4, 0), VFS_OK);

    MapperTable tbl;
    CHECK_EQ(mapper_table_init(&tbl, &pool, &chain_head), VFS_OK);
    CHECK_EQ(tbl.count, 2);

    /* Rebuild — all entries should survive round-trip */
    CHECK_EQ(mapper_table_rebuild(&tbl), VFS_OK);
    CHECK_EQ(tbl.count, 2);
    CHECK_EQ(tbl.entries[0].fromEpoch, (uint32_t)3);  /* CAS-prepend order */
    CHECK_EQ(tbl.entries[0].toEpoch, (uint32_t)4);
    CHECK(!tbl.entries[0].traversalApply);
    CHECK_EQ(tbl.entries[1].fromEpoch, (uint32_t)1);
    CHECK_EQ(tbl.entries[1].toEpoch, (uint32_t)2);
    CHECK(tbl.entries[1].traversalApply);

    mapper_table_destroy(&tbl);
    mapper_teardown(sb);
}

static void test_mapper_rebuild_null_ptr(void) {
    StorageBackend* sb = mapper_setup();
    CHECK(sb != NULL);

    int64_t list_head = 0;
    Pool pool;
    pool_init(&pool, sb, &list_head);

    MapperTable tbl;
    CHECK_EQ(mapper_table_init(&tbl, &pool, NULL), VFS_OK);
    CHECK_EQ(tbl.count, 0);

    /* Rebuild with NULL epochMapperPtr — must produce clean state */
    CHECK_EQ(mapper_table_rebuild(&tbl), VFS_OK);
    CHECK(tbl.entries != NULL);
    CHECK_EQ(tbl.count, 0);
    CHECK_EQ(tbl.capacity, MAPPER_TABLE_INITIAL_CAPACITY);

    mapper_table_destroy(&tbl);
    mapper_teardown(sb);
}

int main(void) {
    test_mapper_init();
    test_mapper_insert_resolve();
    test_mapper_invariants();
    test_mapper_table_empty();
    test_mapper_table_single();
    test_mapper_table_multi();
    test_mapper_rebuild_empty();
    test_mapper_rebuild_with_entries();
    test_mapper_rebuild_null_ptr();

    printf("test_mapper: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
