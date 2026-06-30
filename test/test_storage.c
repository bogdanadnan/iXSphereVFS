/*
 * test/test_storage.c — StorageBackend Tests (Phase 2.2)
 */
#include "ixsphere_vfs.h"
#include "vfs_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

extern int tests_run;
extern int tests_passed;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ_I64(a, b) CHECK((int64_t)(a) == (int64_t)(b))

int test_storage_backend(void) {
    const char* test_path = "/tmp/vfs_test_backend.dat";
    vfs_t* vfs;

    /* Remove test file if exists */
    unlink(test_path);
    
    /* Test 1: Create new file with vfs_create() */
    printf("    Creating new VFS file...\n");
    vfs = vfs_create(test_path, 8192);
    CHECK(vfs != NULL);
    CHECK(vfs->backend.initialized != 0);
    CHECK_EQ_I64(vfs->backend.total_pages, 4);
    CHECK_EQ_I64(vfs->backend.page_size, 8192);
    CHECK_EQ_I64(vfs->backend.bitmap_dir[0], 2);
    printf("    Created: total_pages=%llu, page_size=%llu, bitmap_dir[0]=%lld\n",
           (unsigned long long)vfs->backend.total_pages,
           (unsigned long long)vfs->backend.page_size,
           (long long)vfs->backend.bitmap_dir[0]);
    
    /* Test segment_size field */
    CHECK(vfs->backend.segment_size == 1024);
    
    /* Test vfs_page_size() accessor */
    CHECK_EQ_I64(vfs_page_size(vfs), 8192);
    
    vfs_close(vfs);
    
    /* Test 2: Open existing file with vfs_open() */
    printf("    Opening existing VFS file...\n");
    vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    CHECK(vfs->backend.initialized != 0);
    CHECK_EQ_I64(vfs->backend.total_pages, 4);
    CHECK_EQ_I64(vfs->backend.page_size, 8192);
    CHECK_EQ_I64(vfs->backend.bitmap_dir[0], 2);
    CHECK(vfs->backend.segment_size == 1024);
    
    vfs_page_size(vfs); /* Accessor should work */
    vfs_close(vfs);
    
    /* Test 3: vfs_open on non-VFS file fails */
    printf("    Opening non-VFS file...\n");
    unlink("/tmp/not_vfs.dat");
    FILE* f = fopen("/tmp/not_vfs.dat", "w");
    fprintf(f, "Not a VFS file\n");
    fclose(f);
    
    vfs = vfs_open("/tmp/not_vfs.dat");
    CHECK(vfs == NULL); /* Should fail - no XVFS magic */
    
    /* Test 4: vfs_open on non-existent file fails */
    printf("    Opening non-existent file...\n");
    vfs = vfs_open("/tmp/does_not_exist.dat");
    CHECK(vfs == NULL);
    
    /* Test 5: Create with default page size */
    printf("    Creating with default page size...\n");
    vfs = vfs_create("/tmp/vfs_custom.dat", 0);
    if (vfs) {
        CHECK_EQ_I64(vfs_page_size(vfs), 8192); /* Should use VFS_PAGE_SIZE as default */
        vfs_close(vfs);
    }
    
    /* Test 6: Create with custom page size (4096) */
    printf("    Creating with custom page size 4096...\n");
    vfs = vfs_create("/tmp/vfs_4k.dat", 4096);
    if (vfs) {
        CHECK(vfs != NULL);
        CHECK_EQ_I64(vfs->backend.page_size, 4096);
        /* Verify bitmap_dir[0] is correct (should be 2) */
        CHECK_EQ_I64(vfs->backend.bitmap_dir[0], 2);
        CHECK(vfs->backend.segment_size == 1024);
        
        /* Open the 4K file and verify */
        vfs_close(vfs);
        vfs = vfs_open("/tmp/vfs_4k.dat");
        CHECK(vfs != NULL);
        CHECK_EQ_I64(vfs->backend.page_size, 4096);
        CHECK_EQ_I64(vfs_page_size(vfs), 4096);
        vfs_close(vfs);
        unlink("/tmp/vfs_4k.dat");
    }
    
    unlink("/tmp/not_vfs.dat");
    unlink(test_path);
    unlink("/tmp/vfs_custom.dat");
    
    return 0;
}

int test_bitmap_allocator(void) {
    const char* test_path = "/tmp/vfs_test_bitmap.dat";
    vfs_t* vfs;
    int64_t page;
    
    printf("    Testing bitmap allocator...\n");
    unlink(test_path);
    
    vfs = vfs_create(test_path, 8192);
    CHECK(vfs != NULL);
    
    /* Test 1: Allocate 1 page - should return page 4 */
    page = vfs_allocate(vfs, 1);
    CHECK(page == 4); /* First available page after 0-3 reserved */
    
    /* Test 2: Allocate 10 contiguous pages */
    int64_t start_page = vfs_allocate(vfs, 10);
    CHECK(start_page == 5);
    
    /* Verify we can allocate again after */
    int64_t next_page = vfs_allocate(vfs, 1);
    CHECK(next_page == 15);
    
    /* Test 3: vfs_acquire on a free page returns true */
    int acquired = vfs_acquire(vfs, 20);
    CHECK(acquired == 1);
    
    /* Check it's now allocated (try to acquire again) */
    acquired = vfs_acquire(vfs, 20);
    CHECK(acquired == 0); /* Should fail - already allocated */
    
    /* Test 4: Free a page */
    vfs_free(vfs, 20);
    /* After free, acquire should succeed */
    acquired = vfs_acquire(vfs, 20);
    CHECK(acquired == 1);
    
    vfs_close(vfs);
    unlink(test_path);
    
    return 0;
}

/* Concurrent allocation test data */
static int64_t concurrent_allocated_pages[4][10000];
static int concurrent_alloc_count[4];
static vfs_t* concurrent_vfs_ptr;

static void* alloc_thread_func(void* arg) {
    int id = *(int*)arg;
    int count = 0;
    for (int i = 0; i < 10000; i++) {
        int64_t p = vfs_allocate(concurrent_vfs_ptr, 1);
        if (p >= 0) {
            concurrent_allocated_pages[id][count++] = p;
        }
    }
    concurrent_alloc_count[id] = count;
    return NULL;
}

int test_bitmap_concurrent(void) {
    printf("    Testing concurrent allocation...\n");
    const char* test_path = "/tmp/vfs_test_concurrent.dat";
    unlink(test_path);
    
    vfs_t* vfs = vfs_create(test_path, 8192);
    CHECK(vfs != NULL);
    concurrent_vfs_ptr = vfs;
    
    pthread_t threads[4];
    int thread_ids[4] = {0, 1, 2, 3};
    
    for (int t = 0; t < 4; t++) {
        pthread_create(&threads[t], NULL, alloc_thread_func, &thread_ids[t]);
    }
    
    int64_t total = 0;
    for (int t = 0; t < 4; t++) {
        pthread_join(threads[t], NULL);
        total += concurrent_alloc_count[t];
    }
    
    /* Verify no double allocations */
    int seen[65536] = {0};
    int duplicates = 0;
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < concurrent_alloc_count[t]; i++) {
            int64_t p = concurrent_allocated_pages[t][i];
            if (p >= 4 && p < 65536) {
                if (seen[p]) duplicates++;
                seen[p] = 1;
            }
        }
    }
    
    /* 40000 pages allocated, no duplicates, all within first bitmap */
    CHECK(total == 40000);
    CHECK(duplicates == 0);
    
    vfs_close(vfs);
    unlink(test_path);
    return 0;
}
