#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "common.h"
#include "disk_int.h"
#include "algorithms.h"

// ==========================================================================
// Calculation Thread Pool - Header
// ==========================================================================

// Constants
#define MAX_CALC_QUEUE_SIZE 100

// Task types
typedef enum {
    TASK_BINARY_SPLIT,
    TASK_MULTIPLY,
    TASK_ADD
} calc_task_type;

// Task parameter structures
typedef struct {
    unsigned long a;
    unsigned long b;
    chudnovsky_state* result;
    char base_path[MAX_PATH];
    size_t memory_threshold;
} binary_split_params;

typedef struct {
    disk_int* result;
    disk_int* a;
    disk_int* b;
} multiply_params;

typedef struct {
    disk_int* result;
    disk_int* a;
    disk_int* b;
} add_params;

// Task structure
struct calc_task {
    calc_task_type type;
    void* params;
    pthread_mutex_t* result_lock;
    pthread_cond_t* completion;
    bool completed;
    struct calc_task* dependencies[16];
    int num_dependencies;
};

// Thread pool structure
struct calc_thread_pool {
    pthread_t* threads;
    int num_threads;
    calc_task** task_queue;
    int queue_size;
    int queue_head;
    int queue_tail;
    int queue_count;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_not_empty;
    pthread_cond_t queue_not_full;
    bool shutdown;
};

// Thread pool management functions
void calc_thread_pool_init(calc_thread_pool* pool, int num_threads);
void calc_thread_pool_start(calc_thread_pool* pool);
void calc_thread_pool_shutdown(calc_thread_pool* pool);
void calc_thread_pool_add_task(calc_thread_pool* pool, calc_task* t);
calc_task* calc_thread_pool_get_task(calc_thread_pool* pool);

// Task management functions
bool calc_task_dependencies_met(calc_task* t);
void* calc_thread_worker(void* arg);
calc_task* calc_thread_pool_submit_binary_split(calc_thread_pool* pool, 
                                              unsigned long a, unsigned long b, 
                                              chudnovsky_state* result, 
                                              const char* base_path, 
                                              size_t memory_threshold);
void calc_task_wait(calc_task* t);
void calc_task_free(calc_task* t);

#endif // THREAD_POOL_H
