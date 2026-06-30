/*
 * test/test_atomics.c — Atomics Tests
 *
 * Tests CAS, atomic add, and concurrent counter increments.
 */
#include "vfs_internal.h"
#include <stdio.h>
#include <pthread.h>

extern int tests_run;
extern int tests_passed;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

/* Test CAS with matching expected value */
static void test_cas_match(void) {
    int64_t val = 42;
    int64_t result = vfs_cas_i64(&val, 42, 100);
    CHECK_EQ(result, 42);  /* Returns the old value */
    CHECK_EQ(val, 100);   /* Value was updated */
}

/* Test CAS with non-matching expected value */
static void test_cas_nomatch(void) {
    int64_t val = 42;
    int64_t result = vfs_cas_i64(&val, 99, 100);
    CHECK_EQ(result, 42);  /* Returns current value (not expected) */
    CHECK_EQ(val, 42);     /* Value was NOT updated */
}

/* Test CAS retry loop */
static void test_cas_retry(void) {
    int64_t counter = 0;
    int64_t expected;
    
    for (int i = 0; i < 1000; i++) {
        expected = vfs_atomic_load_i64(&counter);
        while (vfs_cas_i64(&counter, expected, expected + 1) != expected) {
            expected = vfs_atomic_load_i64(&counter);
        }
    }
    CHECK_EQ(counter, 1000);
}

/* Concurrent increment test */
static int64_t g_concurrent_counter = 0;

static void* increment_thread(void* arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) {
        vfs_atomic_add_i64(&g_concurrent_counter, 1);
    }
    return NULL;
}

static void test_concurrent_increment(void) {
    pthread_t threads[4];
    g_concurrent_counter = 0;
    
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, increment_thread, NULL);
    }
    
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    
    CHECK_EQ(g_concurrent_counter, 400000);
}

/* Single-threaded sanity check */
static void test_load_store_i64(void) {
    int64_t val = 0;
    vfs_atomic_store_i64(&val, 12345);
    CHECK_EQ(vfs_atomic_load_i64(&val), 12345);
}

/* Memory barrier tests - verify they are callable and don't crash */
static void test_memory_barriers(void) {
    volatile int64_t x = 0;
    volatile int64_t y = 0;
    
    /* These should compile and run without crashing */
    vfs_mb_acquire();
    x = 1;
    vfs_mb_release();
    y = 2;
    vfs_mb_full();
    
    /* Basic sanity check that the values were set */
    CHECK_EQ(x, 1);
    CHECK_EQ(y, 2);
    
    /* Test that barriers work correctly with atomics */
    int64_t a = 0;
    int64_t b = 0;
    
    /* Release store pattern */
    vfs_mb_release();
    vfs_atomic_store_i64(&a, 42);
    
    /* Acquire load pattern */
    vfs_mb_acquire();
    CHECK_EQ(vfs_atomic_load_i64(&a), 42);
    
    /* Full barrier - ensures both pattern and barrier work */
    vfs_mb_full();
    vfs_atomic_store_i64(&b, 100);
    CHECK_EQ(b, 100);
}

int test_atomics(void) {
    printf("    CAS match...\n");
    test_cas_match();
    
    printf("    CAS no-match...\n");
    test_cas_nomatch();
    
    printf("    CAS retry loop...\n");
    test_cas_retry();
    
    printf("    Concurrent increment (4 threads x 100k)...");
    test_concurrent_increment();
    printf(" done\n");
    
    printf("    Load/store sanity...\n");
    test_load_store_i64();
    
    printf("    Memory barriers...\n");
    test_memory_barriers();
    
    /* Also test i32 atomics */
    int32_t i32_val = 0;
    vfs_atomic_store_i32(&i32_val, 42);
    CHECK_EQ(vfs_atomic_load_i32(&i32_val), 42);
    CHECK_EQ(vfs_atomic_add_i32(&i32_val, 8), 50);
    CHECK_EQ(i32_val, 50);
    
    /* Also test ptr atomics on 64-bit platforms */
#if defined(VFS_ATOMIC_64BIT) || defined(VFS_ARCH_AARCH64)
    void* ptr_arr[2];
    ptr_arr[0] = (void*)0x1000;
    ptr_arr[1] = NULL;
    vfs_atomic_store_ptr((void* volatile*)ptr_arr + 1, (void*)0x2000);
    CHECK_EQ(vfs_atomic_load_ptr((void* volatile*)ptr_arr + 1), (void*)0x2000);
#endif
    
    return 0;
}
