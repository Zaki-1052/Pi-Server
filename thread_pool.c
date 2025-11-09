#include "common.h"
#include "thread_pool.h"
#include "algorithms.h"
#include "disk_int.h"
#include "logging.h"

// ==========================================================================
// Calculation Thread Pool Implementation
// ==========================================================================

typedef enum {
    TASK_BINARY_SPLIT,
    TASK_MULTIPLY,
    TASK_ADD
} calc_task_type;

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

typedef struct calc_task {
    calc_task_type type;
    void* params;
    pthread_mutex_t* result_lock;
    pthread_cond_t* completion;
    bool completed;
    struct calc_task* dependencies[16]; // Maximum 16 dependencies
    int num_dependencies;
} calc_task;

// Calculation thread pool structure
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

// Check if task dependencies are met
bool calc_task_dependencies_met(calc_task* t) {
    for (int i = 0; i < t->num_dependencies; i++) {
        if (!t->dependencies[i]->completed) {
            return false;
        }
    }
    return true;
}

// Initialize calculation thread pool
void calc_thread_pool_init(calc_thread_pool* pool, int num_threads) {
    // Ensure num_threads is positive before using in allocation
    if (num_threads <= 0) {
        log_message("error", "Invalid thread count %d for thread pool", num_threads);
        num_threads = 1; // Default to at least one thread
    }
    
    pool->num_threads = num_threads;
    // Cast to size_t after validation to prevent negative values
    size_t thread_count = (size_t)num_threads;
    
    // TODO: Add error handling for malloc failures
    pool->threads = (pthread_t*)malloc(thread_count * sizeof(pthread_t));
    if (pool->threads == NULL) {
        log_message("critical", "Failed to allocate memory for thread pool threads");
        return;
    }
    
    pool->task_queue = (calc_task**)malloc(MAX_CALC_QUEUE_SIZE * sizeof(calc_task*));
    if (pool->task_queue == NULL) {
        log_message("critical", "Failed to allocate memory for thread pool task queue");
        free(pool->threads);
        return;
    }
    
    pool->queue_size = MAX_CALC_QUEUE_SIZE;
    pool->queue_head = 0;
    pool->queue_tail = 0;
    pool->queue_count = 0;
    pool->shutdown = false;
    
    pthread_mutex_init(&pool->queue_lock, NULL);
    pthread_cond_init(&pool->queue_not_empty, NULL);
    pthread_cond_init(&pool->queue_not_full, NULL);
}

// Shutdown calculation thread pool
void calc_thread_pool_shutdown(calc_thread_pool* pool) {
    pthread_mutex_lock(&pool->queue_lock);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->queue_not_empty);
    pthread_mutex_unlock(&pool->queue_lock);
    
    // Join all threads
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    // Clean up resources
    pthread_mutex_destroy(&pool->queue_lock);
    pthread_cond_destroy(&pool->queue_not_empty);
    pthread_cond_destroy(&pool->queue_not_full);
    
    free(pool->threads);
    
    // Free remaining tasks in queue
    for (int i = 0; i < pool->queue_count; i++) {
        int idx = (pool->queue_head + i) % pool->queue_size;
        free(pool->task_queue[idx]);
    }
    
    free(pool->task_queue);
}

// Add task to calculation queue
void calc_thread_pool_add_task(calc_thread_pool* pool, calc_task* t) {
    pthread_mutex_lock(&pool->queue_lock);
    
    // Wait until queue has space
    while (pool->queue_count == pool->queue_size && !pool->shutdown) {
        pthread_cond_wait(&pool->queue_not_full, &pool->queue_lock);
    }
    
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->queue_lock);
        return;
    }
    
    // Add task to queue
    pool->task_queue[pool->queue_tail] = t;
    pool->queue_tail = (pool->queue_tail + 1) % pool->queue_size;
    pool->queue_count++;
    
    // Signal that queue is not empty
    pthread_cond_signal(&pool->queue_not_empty);
    
    pthread_mutex_unlock(&pool->queue_lock);
}

// Get task from calculation queue
calc_task* calc_thread_pool_get_task(calc_thread_pool* pool) {
    pthread_mutex_lock(&pool->queue_lock);
    
    // Wait until queue has tasks
    while (pool->queue_count == 0 && !pool->shutdown) {
        pthread_cond_wait(&pool->queue_not_empty, &pool->queue_lock);
    }
    
    if (pool->shutdown && pool->queue_count == 0) {
        pthread_mutex_unlock(&pool->queue_lock);
        return NULL;
    }
    
    // Get task from queue
    calc_task* t = pool->task_queue[pool->queue_head];
    pool->queue_head = (pool->queue_head + 1) % pool->queue_size;
    pool->queue_count--;
    
    // Signal that queue is not full
    pthread_cond_signal(&pool->queue_not_full);
    
    pthread_mutex_unlock(&pool->queue_lock);
    
    return t;
}

// Worker thread function for calculation thread pool
void* calc_thread_worker(void* arg) {
    calc_thread_pool* pool = (calc_thread_pool*)arg;
    
    while (1) {
        // Get task from queue
        calc_task* t = calc_thread_pool_get_task(pool);
        
        if (t == NULL) {
            // Pool is shutting down
            break;
        }
        
        // Check if dependencies are met
        if (!calc_task_dependencies_met(t)) {
            // Put task back in queue
            calc_thread_pool_add_task(pool, t);
            continue;
        }
        
        // Execute task based on type
        switch (t->type) {
            case TASK_BINARY_SPLIT: {
                binary_split_params* params = (binary_split_params*)t->params;
                binary_split_disk(params->result, params->a, params->b, 
                                  params->base_path, params->memory_threshold);
                break;
            }
            case TASK_MULTIPLY: {
                multiply_params* params = (multiply_params*)t->params;
                disk_int_mul(params->result, params->a, params->b);
                break;
            }
            case TASK_ADD: {
                add_params* params = (add_params*)t->params;
                disk_int_add(params->result, params->a, params->b);
                break;
            }
        }
        
        // Mark task as completed
        pthread_mutex_lock(t->result_lock);
        t->completed = true;
        pthread_cond_signal(t->completion);
        pthread_mutex_unlock(t->result_lock);
        
        // Clean up task params and set to NULL to prevent use-after-free
        void* params = t->params;
        t->params = NULL;  // Set to NULL before freeing to prevent use-after-free
        free(params);
        
        // Note: We don't free the task itself here as it might still be in use
        // by the thread that submitted it. It will be freed by calc_task_free.
    }
    
    return NULL;
}

// Start calculation thread pool
void calc_thread_pool_start(calc_thread_pool* pool) {
    for (int i = 0; i < pool->num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, calc_thread_worker, pool) != 0) {
            log_message("critical", "pthread_create failed in calc_thread_pool_start: %s", strerror(errno));
            // Partial initialization: try to clean up
            for (int j = 0; j < i; j++) {
                // Attempt to cancel and join threads that were successfully created
                // Note: Cancellation is cooperative. These threads might not terminate immediately
                // or cleanly, especially if they are in uninterruptible operations.
                pthread_cancel(pool->threads[j]);
                pthread_join(pool->threads[j], NULL);
            }
            // The main resources (queue, mutexes, cond vars) are initialized in calc_thread_pool_init.
            // If threads failed to create, the pool is not fully functional.
            // Freeing pool->threads here makes sense.
            if (pool->threads) {
                 free(pool->threads);
                 pool->threads = NULL; // Mark as failed to prevent double free if shutdown is called
            }
            // This is a critical failure for the calculation pool's operation.
            // Depending on program design, could propagate error or exit. Exiting for robustness.
            exit(EXIT_FAILURE);
        }
    }
}

// Submit binary split task to calculation thread pool
calc_task* calc_thread_pool_submit_binary_split(calc_thread_pool* pool, 
                                              unsigned long a, unsigned long b, 
                                              chudnovsky_state* result, 
                                              const char* base_path, 
                                              size_t memory_threshold) {
    calc_task* t = (calc_task*)malloc(sizeof(calc_task));
    binary_split_params* params = (binary_split_params*)malloc(sizeof(binary_split_params));
    
    params->a = a;
    params->b = b;
    params->result = result;
    // Ensure null termination and prevent overflow if base_path is too long
    snprintf(params->base_path, MAX_PATH, "%s", base_path);
    params->memory_threshold = memory_threshold;
    
    t->type = TASK_BINARY_SPLIT;
    t->params = params;
    t->result_lock = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    t->completion = (pthread_cond_t*)malloc(sizeof(pthread_cond_t));
    t->completed = false;
    t->num_dependencies = 0;
    
    pthread_mutex_init(t->result_lock, NULL);
    pthread_cond_init(t->completion, NULL);
    
    calc_thread_pool_add_task(pool, t);
    
    return t;
}

// Wait for calculation task completion
void calc_task_wait(calc_task* t) {
    pthread_mutex_lock(t->result_lock);
    while (!t->completed) {
        pthread_cond_wait(t->completion, t->result_lock);
    }
    pthread_mutex_unlock(t->result_lock);
}

// Free calculation task resources
void calc_task_free(calc_task* t) {
    if (!t) {
        return;  // Guard against null pointer
    }
    
    // Free the task parameters if they exist and haven't been freed in the worker thread
    if (t->params) {
        // In worker thread's switch statement at line ~1454, the params are freed
        // Let's check if the task has been completed to avoid double-free
        
        // If the task is marked as completed, the worker thread already freed the params
        // Otherwise, we need to free them here
        if (!t->completed) {
            // Free parameters based on task type
            switch (t->type) {
                case TASK_BINARY_SPLIT:
                    // binary_split_params doesn't contain pointers that need to be freed
                    free(t->params);
                    break;
                    
                case TASK_MULTIPLY:
                    // multiply_params doesn't contain pointers that need to be freed
                    free(t->params);
                    break;
                    
                case TASK_ADD:
                    // add_params doesn't contain pointers that need to be freed
                    free(t->params);
                    break;
                    
                default:
                    // Unknown task type - still free the params to avoid leaks
                    fprintf(stderr, "Warning: Unknown task type in calc_task_free\n");
                    free(t->params);
                    break;
            }
        }
        
        // Set params to NULL to prevent any future use-after-free
        t->params = NULL;
    }
    
    // Free synchronization primitives
    if (t->result_lock) {
        pthread_mutex_destroy(t->result_lock);
        free(t->result_lock);
        t->result_lock = NULL;  // Prevent use-after-free
    }
    
    if (t->completion) {
        pthread_cond_destroy(t->completion);
        free(t->completion);
        t->completion = NULL;  // Prevent use-after-free
    }
    
    // Finally free the task structure itself
    free(t);
}
