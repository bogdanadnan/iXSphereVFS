/* Quick C6 debug: minimal multithreaded allocation to reproduce the hang. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "storage.h"
#include "page_buf.h"

static void on_alarm(int sig) {
    fprintf(stderr, "\n[ALARM] hung at this point\n");
    fflush(stderr);
    _exit(99);
}

typedef struct {
    StorageBackend* sb;
    int             per_thread;
    int             thread_id;
    int             ok;
} mt_args;

static void* thread_fn(void* arg) {
    mt_args* a = (mt_args*)arg;
    for (int i = 0; i < a->per_thread; i++) {
        int64_t pg = storage_allocate(a->sb, 1);
        if (pg <= 0) {
            fprintf(stderr, "T%d iter %d: storage_allocate returned %lld\n",
                    a->thread_id, i, (long long)pg);
            a->ok = 0;
            return NULL;
        }
        int64_t phys = indir_lookup(a->sb, pg);
        if (phys <= 0) {
            fprintf(stderr, "T%d iter %d: indir_lookup(%lld) returned %lld\n",
                    a->thread_id, i, (long long)pg, (long long)phys);
            a->ok = 0;
            return NULL;
        }
    }
    a->ok = 1;
    return NULL;
}

int main(int argc, char** argv) {
    int n_threads = 4;
    int per_thread = 100;
    if (argc > 1) n_threads = atoi(argv[1]);
    if (argc > 2) per_thread = atoi(argv[2]);

    signal(SIGALRM, on_alarm);
    alarm(5);

    const char* path = "/tmp/test_c6_debug.vfs";
    unlink(path);

    int64_t page_size = 128;
    StorageBackend* sb = storage_open(path, page_size);
    if (!sb) { fprintf(stderr, "storage_open failed\n"); return 1; }

    fprintf(stderr, "INIT: total_pages=%lld inline_count=%lld overflow_count=%d entries_per_overflow=%lld\n",
            (long long)sb->total_pages, (long long)sb->indir.inline_count,
            sb->indir.overflow_count, (long long)sb->indir.entries_per_overflow);

    pthread_t threads[16];
    mt_args   args[16];
    for (int t = 0; t < n_threads; t++) {
        args[t].sb = sb;
        args[t].per_thread = per_thread;
        args[t].thread_id = t;
        args[t].ok = 0;
        pthread_create(&threads[t], NULL, thread_fn, &args[t]);
    }
    for (int t = 0; t < n_threads; t++) {
        pthread_join(threads[t], NULL);
        fprintf(stderr, "T%d ok=%d\n", t, args[t].ok);
    }

    fprintf(stderr, "FINAL: total_pages=%lld overflow_count=%d\n",
            (long long)sb->total_pages, sb->indir.overflow_count);

    storage_close(sb);
    unlink(path);
    return 0;
}
