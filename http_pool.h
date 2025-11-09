#ifndef HTTP_POOL_H
#define HTTP_POOL_H

#include "common.h"

// ==========================================================================
// HTTP Thread Pool - Header
// ==========================================================================

// HTTP thread pool structure
struct http_thread_pool {
    pthread_t* threads;
    int num_threads;
    sem_t* job_semaphore;
    char sem_name[32];
    bool shutdown;
};

// HTTP thread pool functions
void http_thread_pool_init(http_thread_pool* pool, int num_threads);
void http_thread_pool_shutdown(http_thread_pool* pool);

// HTTP worker functions
void* http_worker_function(void* arg);

// Queue management functions
void http_enqueue_job(int client_sock);
int http_dequeue_job(void);
int http_queue_size(void);

#endif // HTTP_POOL_H
