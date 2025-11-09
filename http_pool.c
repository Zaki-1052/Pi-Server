#include "common.h"
#include "http_pool.h"
#include "logging.h"
#include "http_server.h"

// ==========================================================================
// HTTP Thread Pool Implementation
// ==========================================================================

struct http_thread_pool {
    pthread_t* threads;      // Array of thread handles
    int num_threads;         // Number of threads in the pool
    sem_t* job_semaphore;    // Semaphore for job availability (named semaphore for macOS compatibility)
    char sem_name[32];       // Name of the semaphore
    bool shutdown;           // Shutdown flag
};

// Queue for storing incoming client socket file descriptors
int http_job_queue[MAX_HTTP_QUEUE_SIZE];  // Job queue to store client sockets
volatile int http_queue_front = 0, http_queue_back = 0;  // Indices for the circular queue - volatile to prevent compiler optimizations
pthread_mutex_t http_queue_lock = PTHREAD_MUTEX_INITIALIZER;  // Lock for queue access
pthread_cond_t http_queue_not_full = PTHREAD_COND_INITIALIZER;  // Condition for queue not full
pthread_cond_t http_queue_not_empty = PTHREAD_COND_INITIALIZER;  // Condition for queue not empty

// Calculate number of items in the queue
int http_queue_size() {
    int size = http_queue_back - http_queue_front;
    if (size < 0) {
        size += MAX_HTTP_QUEUE_SIZE;
    }
    return size;
}

// Enqueue a client socket into the HTTP job queue
void http_enqueue_job(int client_sock) {
    pthread_mutex_lock(&http_queue_lock);
    
    // Check if the queue is full, wait if necessary
    while ((http_queue_back + 1) % MAX_HTTP_QUEUE_SIZE == http_queue_front) {
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            fprintf(stderr, "Debug: HTTP job queue is full, waiting for space\n");
        }
        pthread_cond_wait(&http_queue_not_full, &http_queue_lock);
    }
    
    // Add the client socket to the queue
    http_job_queue[http_queue_back] = client_sock;
    
    // Update queue back index atomically
    int new_back = (http_queue_back + 1) % MAX_HTTP_QUEUE_SIZE;
    
    // Use memory barrier to ensure writes are visible to other threads
    __sync_synchronize();
    
    // Update the index
    http_queue_back = new_back;
    
    // Signal that queue is not empty
    pthread_cond_signal(&http_queue_not_empty);
    
    // Log the queue operation
    if (config.log_level <= LOG_LEVEL_DEBUG) {
        fprintf(stderr, "Debug: Enqueued socket %d, queue size now %d\n", 
                client_sock, http_queue_size());
    }
    
    pthread_mutex_unlock(&http_queue_lock);
}

// Dequeue a client socket from the HTTP job queue
int http_dequeue_job() {
    pthread_mutex_lock(&http_queue_lock);
    
    // Check if the queue is empty, wait if necessary
    while (http_queue_front == http_queue_back) {
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            fprintf(stderr, "Debug: HTTP job queue is empty, waiting for job\n");
        }
        pthread_cond_wait(&http_queue_not_empty, &http_queue_lock);
        
        // If we're shutting down and still empty after waiting, return error
        if (http_queue_front == http_queue_back) {
            pthread_mutex_unlock(&http_queue_lock);
            return -1;
        }
    }
    
    // Dequeue client socket
    int client_sock = http_job_queue[http_queue_front];
    
    // Update queue front index atomically
    int new_front = (http_queue_front + 1) % MAX_HTTP_QUEUE_SIZE;
    
    // Use memory barrier to ensure writes are visible to other threads
    __sync_synchronize();
    
    // Update the index
    http_queue_front = new_front;
    
    // Signal that queue is not full
    pthread_cond_signal(&http_queue_not_full);
    
    // Log the queue operation
    if (config.log_level <= LOG_LEVEL_DEBUG) {
        fprintf(stderr, "Debug: Dequeued socket %d, queue size now %d\n", 
                client_sock, http_queue_size());
    }
    
    pthread_mutex_unlock(&http_queue_lock);
    return client_sock;
}

// Forward declaration for HTTP request handler
void* handle_http_request(void* arg);

// HTTP worker thread function
void* http_worker_function(void* arg) {
    http_thread_pool* pool = (http_thread_pool*)arg;
    
    while (!pool->shutdown) {
        sem_wait(pool->job_semaphore);  // Wait for a job
        
        if (pool->shutdown) {
            break;  // Exit if shutting down
        }
        
        // Get a job from the queue - http_dequeue_job now handles waiting via condition variable
        int client_sock = http_dequeue_job();
        
        if (client_sock >= 0) {
            // Handle the HTTP request
            handle_http_request(&client_sock);
        }
    }
    
    return NULL;
}

// Initialize HTTP thread pool
void http_thread_pool_init(http_thread_pool* pool, int num_threads) {
    // Ensure num_threads is positive before using in allocation
    if (num_threads <= 0) {
        log_message("error", "Invalid thread count %d for HTTP thread pool", num_threads);
        num_threads = 1; // Default to at least one thread
    }
    
    pool->num_threads = num_threads;
    // Cast to size_t after validation to prevent negative values
    size_t thread_count = (size_t)num_threads;
    pool->threads = (pthread_t*)malloc(thread_count * sizeof(pthread_t));
    pool->shutdown = false;
    
    // Create a unique semaphore name using process ID and timestamp
    snprintf(pool->sem_name, sizeof(pool->sem_name), "/pi_http_sem_%d_%ld", getpid(), time(NULL));
    
    // Create a named semaphore instead of using sem_init (macOS compatibility)
    pool->job_semaphore = sem_open(pool->sem_name, O_CREAT | O_EXCL, 0644, 0);
    if (pool->job_semaphore == SEM_FAILED) {
        perror("Failed to create semaphore");
        exit(1);
    }
    
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, http_worker_function, pool) != 0) {
            log_message("critical", "pthread_create failed in http_thread_pool_init: %s", strerror(errno));
            // Partial initialization: try to clean up what was done
            for (int j = 0; j < i; j++) {
                pthread_cancel(pool->threads[j]); // Request cancel for already created threads
                pthread_join(pool->threads[j], NULL); // Wait for them to exit
            }
            sem_close(pool->job_semaphore);
            sem_unlink(pool->sem_name);
            free(pool->threads);
            pool->threads = NULL; // Mark as failed
            // This is a critical failure for server setup.
            exit(EXIT_FAILURE); 
        }
    }
}

// Shutdown HTTP thread pool
void http_thread_pool_shutdown(http_thread_pool* pool) {
    // Set the shutdown flag first
    pool->shutdown = true;
    
    // Wake up all worker threads so they can check the shutdown flag
    for (int i = 0; i < pool->num_threads; i++) {
        sem_post(pool->job_semaphore);
    }
    
    // Wait for all threads to exit
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    // Clean up any remaining jobs in the queue
    int client_sock;
    int count = 0;
    
    // Keep dequeuing until the queue is empty
    while ((client_sock = http_dequeue_job()) >= 0) {
        // Close any client sockets still in the queue
        close(client_sock);
        fprintf(stderr, "Closed socket %d during HTTP thread pool shutdown\n", client_sock);
        count++;
    }
    
    if (count > 0) {
        fprintf(stderr, "Closed %d socket(s) during HTTP thread pool shutdown\n", count);
    }
    
    // Clean up resources
    sem_close(pool->job_semaphore);
    sem_unlink(pool->sem_name); // Remove the named semaphore
    free(pool->threads);
}
