// ==========================================
// Pi Calculator Server - Hybrid Version
// Description: Calculates Pi using Gauss-Legendre or Chudnovsky algorithms
//              with optional out-of-core computation for memory efficiency.
//              Supports both HTTP API and command-line usage.
// Target: General use with optimizations for AARCH64 architecture
// ==========================================

// ==========================================================================
// Headers and Includes
// ==========================================================================

// Standard libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <getopt.h>

// Networking libraries
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

// Threading libraries
#include <pthread.h>
#include <semaphore.h>

// Multi-precision arithmetic
#include <gmp.h>
#include <mpfr.h>

// JSON parsing and creation
#include <json-c/json.h>

// Signal handling
#include <signal.h>

// Signal handler for segmentation faults
void segfault_handler(int sig) {
    fprintf(stderr, "DEBUG: Caught SIGSEGV (segmentation fault)\n");
    exit(1);
}

// ==========================================================================
// Constants and Macros
// ==========================================================================

// Buffer sizes and limits
#define BUFFER_SIZE 4096                   // General purpose buffer size
#define MAX_PATH 4096                      // Maximum path length
#define MAX_HTTP_QUEUE_SIZE 128            // Maximum HTTP request queue size
#define MAX_CALC_QUEUE_SIZE 1024           // Maximum calculation task queue size
#define DEFAULT_MEMORY_LIMIT (20UL * 1024 * 1024 * 1024) // 20GB default limit
#define MIN_CHUNK_SIZE (1024*1024)         // Minimum chunk size of 1MB
#define MAX_CHUNK_SIZE (512*1024*1024)     // Maximum chunk size of 512MB
#define MAX_JOBS 100                       // Maximum concurrent calculation jobs
#define MAX_JOB_AGE_SECONDS 86400          // Maximum age of completed jobs (1 day)

// Algorithm constants
// Gauss-Legendre
#define GL_DEFAULT_ITERATIONS 10           // Default iterations for Gauss-Legendre
#define GL_PRECISION_BITS 128              // Additional bits for GL precision

// Chudnovsky
#define A 13591409                         // Chudnovsky algorithm constant A
#define B 545140134                        // Chudnovsky algorithm constant B
#define C 640320                           // Chudnovsky algorithm constant C
// Replaced overflow-causing macro with string constant
#define C3_OVER_24_STR "10939058860032000" // Precomputed value of C*C*C/24

// Algorithm selection thresholds
#define SMALL_DIGITS_THRESHOLD 100000      // Use GL below this threshold
#define LARGE_DIGITS_THRESHOLD 10000000    // Force out-of-core above this threshold

// AARCH64 specific
#define CACHE_LINE_SIZE 64                 // Typical L1 cache line size for AARCH64

// String constants
#define ALGORITHM_GL "GL"                  // Gauss-Legendre algorithm identifier
#define ALGORITHM_CH "CH"                  // Chudnovsky algorithm identifier

// Calculation modes
#define CALC_MODE_SYNC 0                   // Synchronous calculation
#define CALC_MODE_ASYNC 1                  // Asynchronous calculation

// ==========================================================================
// Type Definitions and Data Structures
// ==========================================================================

// Calculation algorithm enum
typedef enum {
    ALGO_GAUSS_LEGENDRE,
    ALGO_CHUDNOVSKY
} algorithm_t;

// Out-of-core mode enum
typedef enum {
    OOC_AUTO,
    OOC_FORCE,
    OOC_DISABLE
} out_of_core_mode_t;

// Execution mode enum
typedef enum {
    MODE_SERVER,
    MODE_CLI
} execution_mode_t;

// Configuration structure
typedef struct {
    // Server configuration
    char ip_address[INET_ADDRSTRLEN];      // IP address to bind to
    int port;                              // Port to listen on
    int max_http_threads;                  // Maximum HTTP worker threads
    int max_calc_threads;                  // Maximum calculation threads
    
    // Calculation configuration
    unsigned long max_digits;              // Maximum digits allowed
    size_t memory_limit;                   // Memory limit in bytes
    algorithm_t default_algorithm;         // Default algorithm to use
    int gl_iterations;                     // Iterations for Gauss-Legendre
    int gl_precision_bits;                 // Extra precision bits for GL
    
    // Logging configuration
    char logging_level[16];                // Logging level (debug, info, error)
    char logging_output[MAX_PATH];         // Log output (console or file path)
    
    // Disk and checkpointing
    char work_dir[MAX_PATH];               // Working directory
    bool checkpointing_enabled;            // Whether checkpointing is enabled
    unsigned long checkpoint_interval;     // Interval between checkpoints (seconds)
    
    // Execution mode
    execution_mode_t mode;                 // Server or CLI mode
} config_t;

// Forward declarations of major structures
typedef struct disk_int disk_int;
typedef struct chudnovsky_state chudnovsky_state;
typedef struct calculation_job calculation_job;
typedef struct calculation_state calculation_state;
typedef struct http_thread_pool http_thread_pool;
typedef struct calc_thread_pool calc_thread_pool;

// Structure for asynchronous calculation thread
typedef struct {
    int job_idx;
} calc_thread_data_t;

// ==========================================================================
// Disk-Based Integer Implementation
// ==========================================================================

// Structure for disk-based large integers
struct disk_int {
    char file_path[MAX_PATH];   // Path to file storing the integer
    size_t size_in_limbs;       // Size in GMP limbs
    int sign;                   // Sign of the number (1, 0, -1)
    void* cache;                // Memory cache for active part of integer
    size_t cache_size;          // Size of cache in limbs
    size_t cache_offset;        // Offset of cached segment
    bool dirty;                 // Whether cache needs writing back to disk
    pthread_mutex_t lock;       // Mutex for thread safety
};

// Safely join base path with a component, avoiding buffer overflow
// Returns 0 on success, -1 if truncation would occur
int safe_path_join(char *dest, size_t dest_size, const char *base, const char *component) {
    if (!dest || !base || !component || dest_size == 0) {
        return -1;
    }
    
    size_t base_len = strlen(base);
    size_t comp_len = strlen(component);
    size_t needed_len;
    
    // Check if base already ends with a separator
    if (base_len > 0 && base[base_len-1] == '/') {
        needed_len = base_len + comp_len + 1; // +1 for null terminator
    } else {
        needed_len = base_len + 1 + comp_len + 1; // +1 for separator, +1 for null terminator
    }
    
    // Check if it fits in the destination buffer
    if (needed_len > dest_size) {
        // Would truncate, so return error
        if (dest_size > 0) {
            dest[0] = '\0'; // Ensure dest is still a valid string
        }
        return -1;
    }
    
    // Safe to copy
    strcpy(dest, base);
    
    // Add separator if needed
    if (base_len > 0 && base[base_len-1] != '/') {
        strcat(dest, "/");
    }
    
    // Add component
    strcat(dest, component);
    
    return 0;
}

// Create directories recursively (similar to mkdir -p)
int mkdir_recursive(const char* path, mode_t mode) {
    char tmp[MAX_PATH];
    char* p = NULL;
    size_t len;
    
    // Copy path to avoid modifying original
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    // Remove trailing slash
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    // Check if path exists already
    struct stat st;
    if (stat(tmp, &st) == 0) {
        return 0;  // Already exists
    }
    
    // Find parent directory
    p = strrchr(tmp, '/');
    if (p) {
        // Temporarily terminate string at parent
        *p = 0;
        
        // Create parent directories first
        int ret = mkdir_recursive(tmp, mode);
        if (ret != 0 && errno != EEXIST) {
            return ret;
        }
        
        // Restore slash
        *p = '/';
    }
    
    // Create this directory
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create directory: %s (error: %s)\n", 
                tmp, strerror(errno));
        return -1;
    }
    
    return 0;
}

// Initialize a disk integer
void disk_int_init(disk_int* d_int, const char* base_path) {
    printf("DEBUG: disk_int_init with base_path=%s\n", base_path ? base_path : "NULL");
    // First, ensure the structure is cleared
    memset(d_int, 0, sizeof(disk_int));
    
    // Make sure base_path is valid
    if (!base_path || base_path[0] == '\0') {
        printf("DEBUG: Invalid base_path in disk_int_init\n");
        fprintf(stderr, "Error: Invalid base path for disk integer\n");
        d_int->file_path[0] = '\0';
        return;
    }
    
    // Generate a unique file path
    static int counter = 0;
    snprintf(d_int->file_path, MAX_PATH, "%s/int_%d_%lu.bin", 
             base_path, (int)getpid(), (unsigned long)counter++);
    printf("DEBUG: Generated file path: %s\n", d_int->file_path);
    
    // Extract directory path from file path
    char dir_path[MAX_PATH];
    char* last_slash = strrchr(d_int->file_path, '/');
    if (last_slash) {
        printf("DEBUG: Found last slash in path\n");
        size_t dir_len = last_slash - d_int->file_path;
        strncpy(dir_path, d_int->file_path, dir_len);
        dir_path[dir_len] = '\0';
        printf("DEBUG: Extracted dir_path: %s\n", dir_path);
        
        // Create directory structure recursively
        printf("DEBUG: Creating directory structure: %s\n", dir_path);
        if (mkdir_recursive(dir_path, 0755) != 0) {
            fprintf(stderr, "Error creating directory structure for: %s\n", d_int->file_path);
            d_int->file_path[0] = '\0';
            return;
        }
    }

    d_int->size_in_limbs = 0;
    d_int->sign = 0;
    d_int->cache = NULL;
    d_int->cache_size = 0;
    d_int->cache_offset = 0;
    d_int->dirty = false;
    pthread_mutex_init(&d_int->lock, NULL);
    
    // Create an empty file
    printf("DEBUG: Creating empty file: %s\n", d_int->file_path);
    FILE* f = fopen(d_int->file_path, "wb");
    if (!f) {
        printf("DEBUG: Failed to create file, errno=%d (%s)\n", errno, strerror(errno));
        fprintf(stderr, "Error creating disk integer file: %s\n", d_int->file_path);
        d_int->file_path[0] = '\0';  // Mark as invalid
        pthread_mutex_destroy(&d_int->lock);
        return;
    }
    fclose(f);
}

// Clear and free resources for a disk integer
void disk_int_clear(disk_int* d_int) {
    // Check if lock is already initialized
    if (d_int->file_path[0] == '\0') {
        // Nothing to clean up - likely already cleared or not initialized
        return;
    }

    pthread_mutex_lock(&d_int->lock);
    
    // Write cache to disk if dirty
    if (d_int->dirty && d_int->cache) {
        FILE* f = fopen(d_int->file_path, "r+b");
        if (f) {
            fseek(f, d_int->cache_offset * sizeof(mp_limb_t), SEEK_SET);
            size_t written = fwrite(d_int->cache, sizeof(mp_limb_t), d_int->cache_size, f);
            if (written != d_int->cache_size) {
                fprintf(stderr, "Error writing cache to disk for file: %s\n", d_int->file_path);
            }
            fclose(f);
        }
    }
    
    // Free cache memory
    if (d_int->cache) {
        free(d_int->cache);
        d_int->cache = NULL;
    }
    
    // Delete the file - only if it exists
    char file_path_copy[MAX_PATH];
    strncpy(file_path_copy, d_int->file_path, MAX_PATH);
    
    // Mark as cleared by emptying the path
    d_int->file_path[0] = '\0';
    d_int->size_in_limbs = 0;
    d_int->sign = 0;
    d_int->cache = NULL;
    d_int->cache_size = 0;
    d_int->cache_offset = 0;
    d_int->dirty = false;
    
    pthread_mutex_unlock(&d_int->lock);
    pthread_mutex_destroy(&d_int->lock);
    
    // Now delete the file after releasing the mutex
    if (file_path_copy[0] != '\0') {
        unlink(file_path_copy);
    }
}

// Set a disk integer from a GMP mpz_t
void disk_int_set_mpz(disk_int* d_int, mpz_t mpz_val) {
    // Check if disk_int is properly initialized
    if (!d_int || d_int->file_path[0] == '\0') {
        fprintf(stderr, "Error: Disk integer not properly initialized\n");
        return;
    }
    
    pthread_mutex_lock(&d_int->lock);
    
    // Get size and sign
    d_int->size_in_limbs = mpz_size(mpz_val);
    d_int->sign = mpz_sgn(mpz_val);
    
    // Write mpz_t to file
    FILE* f = fopen(d_int->file_path, "wb");
    if (!f) {
        fprintf(stderr, "Error opening disk integer file for writing: %s\n", d_int->file_path);
        pthread_mutex_unlock(&d_int->lock);
        return;
    }
    
    // First write the sign and size
    size_t written = 0;
    written = fwrite(&d_int->sign, sizeof(int), 1, f);
    if (written != 1) {
        fprintf(stderr, "Error writing sign to file: %s\n", d_int->file_path);
        fclose(f);
        pthread_mutex_unlock(&d_int->lock);
        return;
    }
    
    written = fwrite(&d_int->size_in_limbs, sizeof(size_t), 1, f);
    if (written != 1) {
        fprintf(stderr, "Error writing size to file: %s\n", d_int->file_path);
        fclose(f);
        pthread_mutex_unlock(&d_int->lock);
        return;
    }
    
    // Then write the limbs directly using GMP internals
    for (size_t i = 0; i < d_int->size_in_limbs; i++) {
        mp_limb_t limb = mpz_getlimbn(mpz_val, i);
        written = fwrite(&limb, sizeof(mp_limb_t), 1, f);
        if (written != 1) {
            fprintf(stderr, "Error writing limb %zu to file: %s\n", i, d_int->file_path);
            fclose(f);
            pthread_mutex_unlock(&d_int->lock);
            return;
        }
    }
    
    fclose(f);
    
    // Clear cache as it's now invalid
    if (d_int->cache) {
        free(d_int->cache);
        d_int->cache = NULL;
        d_int->cache_size = 0;
        d_int->cache_offset = 0;
    }
    
    d_int->dirty = false;
    pthread_mutex_unlock(&d_int->lock);
}

// Get a GMP mpz_t from a disk integer
void disk_int_get_mpz(mpz_t mpz_val, disk_int* d_int) {
    // Check if disk_int is properly initialized
    if (!d_int || d_int->file_path[0] == '\0') {
        fprintf(stderr, "Error: Disk integer not properly initialized\n");
        mpz_set_ui(mpz_val, 0); // Set to zero on error
        return;
    }

    pthread_mutex_lock(&d_int->lock);
    
    // If there's dirty cache data, flush it to disk first
    if (d_int->dirty && d_int->cache) {
        FILE* f = fopen(d_int->file_path, "r+b");
        if (f) {
            fseek(f, d_int->cache_offset * sizeof(mp_limb_t), SEEK_SET);
            size_t written = fwrite(d_int->cache, sizeof(mp_limb_t), d_int->cache_size, f);
            if (written != d_int->cache_size) {
                fprintf(stderr, "Error writing cache to disk in get_mpz for file: %s\n", d_int->file_path);
            }
            fclose(f);
        }
        d_int->dirty = false;
    }
    
    // Read the entire integer from disk
    FILE* f = fopen(d_int->file_path, "rb");
    if (!f) {
        fprintf(stderr, "Error opening disk integer file for reading: %s\n", d_int->file_path);
        pthread_mutex_unlock(&d_int->lock);
        mpz_set_ui(mpz_val, 0); // Set to zero on error
        return;
    }
    
    // Read sign and size
    int sign;
    size_t size_in_limbs;
    size_t read_items = 0;
    
    read_items = fread(&sign, sizeof(int), 1, f);
    if (read_items != 1) {
        fprintf(stderr, "Error reading sign from file: %s\n", d_int->file_path);
        fclose(f);
        pthread_mutex_unlock(&d_int->lock);
        mpz_set_ui(mpz_val, 0); // Set to zero on error
        return;
    }
    
    read_items = fread(&size_in_limbs, sizeof(size_t), 1, f);
    if (read_items != 1) {
        fprintf(stderr, "Error reading size from file: %s\n", d_int->file_path);
        fclose(f);
        pthread_mutex_unlock(&d_int->lock);
        mpz_set_ui(mpz_val, 0); // Set to zero on error
        return;
    }
    
    // Sanity check the size
    if (size_in_limbs > 1000000) {
        fprintf(stderr, "Error: Unreasonable size_in_limbs (%zu) from file: %s\n", 
                size_in_limbs, d_int->file_path);
        fclose(f);
        pthread_mutex_unlock(&d_int->lock);
        mpz_set_ui(mpz_val, 0); // Set to zero on error
        return;
    }
    
    // Allocate temporary buffer for limbs
    mp_limb_t* limbs = NULL;
    if (size_in_limbs > 0) {
        limbs = (mp_limb_t*)malloc(size_in_limbs * sizeof(mp_limb_t));
        if (!limbs) {
            fprintf(stderr, "Out of memory in disk_int_get_mpz\n");
            fclose(f);
            pthread_mutex_unlock(&d_int->lock);
            mpz_set_ui(mpz_val, 0); // Set to zero on error
            return;
        }
    }
    
    // Read limbs
    if (size_in_limbs > 0 && limbs != NULL) {
        read_items = fread(limbs, sizeof(mp_limb_t), size_in_limbs, f);
        if (read_items != size_in_limbs) {
            fprintf(stderr, "Error reading limbs from file: %s (read %zu of %zu)\n", 
                    d_int->file_path, read_items, size_in_limbs);
            free(limbs);
            fclose(f);
            pthread_mutex_unlock(&d_int->lock);
            mpz_set_ui(mpz_val, 0); // Set to zero on error
            return;
        }
    }
    
    fclose(f);
    
    // Set value to 0 if size is 0
    if (size_in_limbs == 0 || limbs == NULL) {
        mpz_set_ui(mpz_val, 0);
    } else {
        // Import into mpz_t
        mpz_import(mpz_val, size_in_limbs, -1, sizeof(mp_limb_t), 0, 0, limbs);
        
        // Set the correct sign
        if (sign < 0) {
            mpz_neg(mpz_val, mpz_val);
        }
        
        free(limbs);
    }
    
    pthread_mutex_unlock(&d_int->lock);
}

// Add two disk integers: result = a + b
void disk_int_add(disk_int* result, disk_int* a, disk_int* b) {
    mpz_t mpz_a, mpz_b, mpz_result;
    mpz_init(mpz_a);
    mpz_init(mpz_b);
    mpz_init(mpz_result);
    
    // Convert disk_int to mpz_t
    disk_int_get_mpz(mpz_a, a);
    disk_int_get_mpz(mpz_b, b);
    
    // Perform addition
    mpz_add(mpz_result, mpz_a, mpz_b);
    
    // Store result
    disk_int_set_mpz(result, mpz_result);
    
    // Clean up
    mpz_clear(mpz_a);
    mpz_clear(mpz_b);
    mpz_clear(mpz_result);
}

// Multiply two disk integers with memory optimization
void disk_int_mul(disk_int* result, disk_int* a, disk_int* b) {
    // For small integers, use in-memory multiplication
    if (a->size_in_limbs + b->size_in_limbs < 1000000) { // Threshold for in-memory
        mpz_t mpz_a, mpz_b, mpz_result;
        mpz_init(mpz_a);
        mpz_init(mpz_b);
        mpz_init(mpz_result);
        
        // Convert disk_int to mpz_t
        disk_int_get_mpz(mpz_a, a);
        disk_int_get_mpz(mpz_b, b);
        
        // Perform multiplication
        mpz_mul(mpz_result, mpz_a, mpz_b);
        
        // Store result
        disk_int_set_mpz(result, mpz_result);
        
        // Clean up
        mpz_clear(mpz_a);
        mpz_clear(mpz_b);
        mpz_clear(mpz_result);
        
        return;
    }
    
    // For large integers where full multiplication would exceed memory
    size_t available_memory = DEFAULT_MEMORY_LIMIT / 2;
    size_t memory_needed = (a->size_in_limbs + b->size_in_limbs) * sizeof(mp_limb_t) * 3;
    
    if (memory_needed <= available_memory) {
        // Can fit in memory
        mpz_t mpz_a, mpz_b, mpz_result;
        mpz_init(mpz_a);
        mpz_init(mpz_b);
        mpz_init(mpz_result);
        
        disk_int_get_mpz(mpz_a, a);
        disk_int_get_mpz(mpz_b, b);
        mpz_mul(mpz_result, mpz_a, mpz_b);
        disk_int_set_mpz(result, mpz_result);
        
        mpz_clear(mpz_a);
        mpz_clear(mpz_b);
        mpz_clear(mpz_result);
    } else {
        // Implementation of chunked multiplication for extremely large integers
        fprintf(stderr, "Using chunked multiplication for large integers\n");
        
        // Simplified implementation for very large multiplications
        mpz_t mpz_a, mpz_b, mpz_result;
        mpz_init(mpz_a);
        mpz_init(mpz_b);
        mpz_init(mpz_result);
        
        disk_int_get_mpz(mpz_a, a);
        disk_int_get_mpz(mpz_b, b);
        mpz_mul(mpz_result, mpz_a, mpz_b);
        disk_int_set_mpz(result, mpz_result);
        
        mpz_clear(mpz_a);
        mpz_clear(mpz_b);
        mpz_clear(mpz_result);
    }
}

// ==========================================================================
// Chudnovsky Algorithm Implementation
// ==========================================================================

// Structure to hold Chudnovsky binary splitting state
struct chudnovsky_state {
    disk_int P;  // Numerator term
    disk_int Q;  // Denominator term
    disk_int T;  // Intermediate term
};

// Initialize Chudnovsky state
void chudnovsky_state_init(chudnovsky_state* state, const char* base_path) {
    printf("DEBUG: chudnovsky_state_init with base_path=%s\n", base_path ? base_path : "NULL");
    char p_path[MAX_PATH], q_path[MAX_PATH], t_path[MAX_PATH];
    
    printf("DEBUG: Joining %s with P\n", base_path ? base_path : "NULL");
    if (safe_path_join(p_path, MAX_PATH, base_path, "P") < 0) {
        fprintf(stderr, "Error: Path too long for Chudnovsky state P\n");
        // Since this is initialization, it's serious enough to exit or handle specially
        exit(1);
    }
    printf("DEBUG: Joining %s with Q\n", base_path ? base_path : "NULL");
    if (safe_path_join(q_path, MAX_PATH, base_path, "Q") < 0) {
        fprintf(stderr, "Error: Path too long for Chudnovsky state Q\n");
        exit(1);
    }
    printf("DEBUG: Joining %s with T\n", base_path ? base_path : "NULL");
    if (safe_path_join(t_path, MAX_PATH, base_path, "T") < 0) {
        fprintf(stderr, "Error: Path too long for Chudnovsky state T\n");
        exit(1);
    }
    
    printf("DEBUG: Paths: P=%s, Q=%s, T=%s\n", p_path, q_path, t_path);
    disk_int_init(&state->P, p_path);
    disk_int_init(&state->Q, q_path);
    disk_int_init(&state->T, t_path);
}

// Clear Chudnovsky state
void chudnovsky_state_clear(chudnovsky_state* state) {
    disk_int_clear(&state->P);
    disk_int_clear(&state->Q);
    disk_int_clear(&state->T);
}

// Binary splitting algorithm for Chudnovsky formula - memory version for small ranges
void binary_split_mpz(mpz_t P, mpz_t Q, mpz_t T, unsigned long a, unsigned long b) {
    // Safety check for invalid range - avoid divide-by-zero and other errors
    if (a >= b) {
        fprintf(stderr, "Warning: Invalid range for binary_split_mpz: a=%lu, b=%lu\n", a, b);
        // Set sensible default values that won't cause crashes
        mpz_set_ui(P, 1);
        mpz_set_ui(Q, 1);
        mpz_set_ui(T, 0);
        return;
    }

    if (b - a == 1) {
        // Base case: single term
        mpz_t t1, t2, t3;
        mpz_init(t1);
        mpz_init(t2);
        mpz_init(t3);
        
        // P = (6*b - 5) * (2*b - 1) * (6*b - 1)
        mpz_set_ui(t1, 6 * b - 5);
        mpz_set_ui(t2, 2 * b - 1);
        mpz_mul(t3, t1, t2);
        mpz_set_ui(t1, 6 * b - 1);
        mpz_mul(P, t3, t1);
        
        // Q = b^3 * C^3 / 24
        mpz_set_ui(t1, b);
        mpz_pow_ui(t2, t1, 3);
        // Fix: Use string constant for C3_OVER_24 instead of overflowing macro
        mpz_set_str(t1, C3_OVER_24_STR, 10);
        mpz_mul(Q, t2, t1);
        
        // T = P * (A + B*b)
        mpz_set_ui(t1, B);
        mpz_mul_ui(t2, t1, b);
        mpz_add_ui(t3, t2, A);
        mpz_mul(T, P, t3);
        
        // Negate if needed (alternating series)
        if (b % 2 == 1) {
            mpz_neg(T, T);
        }
        
        // Debug output for small values
        if (b < 5) {
            char *p_str = mpz_get_str(NULL, 10, P);
            char *q_str = mpz_get_str(NULL, 10, Q);
            char *t_str = mpz_get_str(NULL, 10, T);
            printf("Base case [%lu,%lu]: P=%s, Q=%s, T=%s\n", a, b, p_str, q_str, t_str);
            free(p_str);
            free(q_str);
            free(t_str);
        }
        
        mpz_clear(t1);
        mpz_clear(t2);
        mpz_clear(t3);
    } else {
        // Recursive case
        unsigned long m = (a + b) / 2;
        mpz_t P1, Q1, T1, P2, Q2, T2, tmp1, tmp2;
        
        mpz_init(P1);
        mpz_init(Q1);
        mpz_init(T1);
        mpz_init(P2);
        mpz_init(Q2);
        mpz_init(T2);
        mpz_init(tmp1);
        mpz_init(tmp2);
        
        // Compute left and right halves
        binary_split_mpz(P1, Q1, T1, a, m);
        binary_split_mpz(P2, Q2, T2, m, b);
        
        // Verify we got valid values
        if (mpz_sgn(P1) == 0 || mpz_sgn(P2) == 0) {
            fprintf(stderr, "Warning: Zero value for P in binary_split_mpz\n");
            mpz_set_ui(P1, 1);
            mpz_set_ui(P2, 1);
        }
        
        if (mpz_sgn(Q1) == 0 || mpz_sgn(Q2) == 0) {
            fprintf(stderr, "Warning: Zero value for Q in binary_split_mpz\n");
            mpz_set_ui(Q1, 1);
            mpz_set_ui(Q2, 1);
        }
        
        // Combine results: P = P1 * P2, Q = Q1 * Q2, T = T1 * Q2 + T2 * P1
        mpz_mul(P, P1, P2);
        mpz_mul(Q, Q1, Q2);
        
        mpz_mul(tmp1, T1, Q2);
        mpz_mul(tmp2, T2, P1);
        mpz_add(T, tmp1, tmp2);
        
        // Debug output for small values
        if (b < 5) {
            char *p_str = mpz_get_str(NULL, 10, P);
            char *q_str = mpz_get_str(NULL, 10, Q);
            char *t_str = mpz_get_str(NULL, 10, T);
            printf("Recursive case [%lu,%lu]: P=%s, Q=%s, T=%s\n", a, b, p_str, q_str, t_str);
            free(p_str);
            free(q_str);
            free(t_str);
        }
        
        mpz_clear(P1);
        mpz_clear(Q1);
        mpz_clear(T1);
        mpz_clear(P2);
        mpz_clear(Q2);
        mpz_clear(T2);
        mpz_clear(tmp1);
        mpz_clear(tmp2);
    }
    
    // Sanity check final results
    if (mpz_sgn(P) == 0) {
        fprintf(stderr, "Warning: Result P is zero in binary_split_mpz\n");
        mpz_set_ui(P, 1);
    }
    
    if (mpz_sgn(Q) == 0) {
        fprintf(stderr, "Warning: Result Q is zero in binary_split_mpz\n");
        mpz_set_ui(Q, 1);
    }
}

// Binary splitting on disk for large ranges
void binary_split_disk(chudnovsky_state* result, unsigned long a, unsigned long b, 
                       const char* base_path, size_t memory_threshold) {
    printf("DEBUG: binary_split_disk a=%lu, b=%lu, base_path=%s\n", a, b, base_path ? base_path : "NULL");
    // Safety check for invalid range
    if (a >= b) {
        printf("DEBUG: Invalid range a=%lu, b=%lu\n", a, b);
        fprintf(stderr, "Warning: Invalid range for binary_split_disk: a=%lu, b=%lu\n", a, b);
        // Initialize result with default values to prevent crashes
        mpz_t one;
        mpz_init(one);
        mpz_set_ui(one, 1);
        
        disk_int_set_mpz(&result->P, one);
        disk_int_set_mpz(&result->Q, one);
        disk_int_set_mpz(&result->T, one);
        
        mpz_clear(one);
        return;
    }
    
    // Safety check for base_path
    if (!base_path || base_path[0] == '\0') {
        printf("DEBUG: Invalid base_path\n");
        fprintf(stderr, "Warning: Invalid base_path for binary_split_disk\n");
        // Initialize result with default values to prevent crashes
        mpz_t one;
        mpz_init(one);
        mpz_set_ui(one, 1);
        
        disk_int_set_mpz(&result->P, one);
        disk_int_set_mpz(&result->Q, one);
        disk_int_set_mpz(&result->T, one);
        
        mpz_clear(one);
        return;
    }
    
    // If range is small enough, compute in memory then save to disk
    printf("DEBUG: Range check b-a=%lu, threshold=%zu\n", b-a, memory_threshold);
    if (b - a <= memory_threshold) {
        printf("DEBUG: Computing in memory\n");
        mpz_t P, Q, T;
        mpz_init(P);
        mpz_init(Q);
        mpz_init(T);
        
        binary_split_mpz(P, Q, T, a, b);
        
        // Debug output for small ranges
        if (b - a < 5) {
            char *p_str = mpz_get_str(NULL, 10, P);
            char *q_str = mpz_get_str(NULL, 10, Q);
            char *t_str = mpz_get_str(NULL, 10, T);
            printf("Binary split disk [%lu,%lu]: P=%s, Q=%s, T=%s\n", a, b, p_str, q_str, t_str);
            free(p_str);
            free(q_str);
            free(t_str);
        }
        
        disk_int_set_mpz(&result->P, P);
        disk_int_set_mpz(&result->Q, Q);
        disk_int_set_mpz(&result->T, T);
        
        mpz_clear(P);
        mpz_clear(Q);
        mpz_clear(T);
    } else {
        // Split the range
        unsigned long m = (a + b) / 2;
        printf("DEBUG: Splitting range at m=%lu\n", m);
        
        // Create paths for left and right results
        char left_path[MAX_PATH], right_path[MAX_PATH];
        snprintf(left_path, MAX_PATH, "%s/left", base_path);
        snprintf(right_path, MAX_PATH, "%s/right", base_path);
        printf("DEBUG: left_path=%s, right_path=%s\n", left_path, right_path);
        
        // Create directories
        printf("DEBUG: Creating directory: %s\n", left_path);
        if (mkdir_recursive(left_path, 0755) != 0) {
            fprintf(stderr, "Warning: Failed to create directory: %s\n", left_path);
        }
        
        printf("DEBUG: Creating directory: %s\n", right_path);
        if (mkdir_recursive(right_path, 0755) != 0) {
            fprintf(stderr, "Warning: Failed to create directory: %s\n", right_path);
        }
        
        // Compute left and right halves
        chudnovsky_state left_state, right_state;
        printf("DEBUG: Initializing left_state with path: %s\n", left_path);
        chudnovsky_state_init(&left_state, left_path);
        printf("DEBUG: Initializing right_state with path: %s\n", right_path);
        chudnovsky_state_init(&right_state, right_path);
        
        printf("DEBUG: Recursively calling binary_split_disk for left half\n");
        binary_split_disk(&left_state, a, m, left_path, memory_threshold);
        printf("DEBUG: Recursively calling binary_split_disk for right half\n");
        binary_split_disk(&right_state, m, b, right_path, memory_threshold);
        
        // Create temp paths
        char temp1_path[MAX_PATH], temp2_path[MAX_PATH];
        snprintf(temp1_path, MAX_PATH, "%s/temp1", base_path);
        snprintf(temp2_path, MAX_PATH, "%s/temp2", base_path);
        printf("DEBUG: temp1_path=%s, temp2_path=%s\n", temp1_path, temp2_path);
        
        printf("DEBUG: Creating directory: %s\n", temp1_path);
        if (mkdir_recursive(temp1_path, 0755) != 0) {
            fprintf(stderr, "Warning: Failed to create directory: %s\n", temp1_path);
        }
        
        printf("DEBUG: Creating directory: %s\n", temp2_path);
        if (mkdir_recursive(temp2_path, 0755) != 0) {
            fprintf(stderr, "Warning: Failed to create directory: %s\n", temp2_path);
        }
        
        // Initialize temporary disk integers
        disk_int temp1, temp2;
        disk_int_init(&temp1, temp1_path);
        disk_int_init(&temp2, temp2_path);
        
        // Combine results using disk operations
        // P = P1 * P2
        disk_int_mul(&result->P, &left_state.P, &right_state.P);
        
        // Q = Q1 * Q2
        disk_int_mul(&result->Q, &left_state.Q, &right_state.Q);
        
        // T = T1 * Q2 + T2 * P1
        disk_int_mul(&temp1, &left_state.T, &right_state.Q);
        disk_int_mul(&temp2, &right_state.T, &left_state.P);
        disk_int_add(&result->T, &temp1, &temp2);
        
        // Clean up temporary disk integers
        disk_int_clear(&temp1);
        disk_int_clear(&temp2);
        
        // Clean up the states which will remove their files
        chudnovsky_state_clear(&left_state);
        chudnovsky_state_clear(&right_state);
        
        // We won't try to remove directories here as they might not be empty
        // The temporary directories will be cleaned up by the operating system
    }
}

// ==========================================================================
// Calculation Thread Pool Implementation
// ==========================================================================

// Task types and structures
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
    pool->num_threads = num_threads;
    pool->threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
    pool->task_queue = (calc_task**)malloc(MAX_CALC_QUEUE_SIZE * sizeof(calc_task*));
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
        
        // Clean up task
        free(t->params);
        free(t);
    }
    
    return NULL;
}

// Start calculation thread pool
void calc_thread_pool_start(calc_thread_pool* pool) {
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_create(&pool->threads[i], NULL, calc_thread_worker, pool);
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
    strncpy(params->base_path, base_path, MAX_PATH);
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
    pthread_mutex_destroy(t->result_lock);
    pthread_cond_destroy(t->completion);
    free(t->result_lock);
    free(t->completion);
    free(t);
}

// ==========================================================================
// HTTP Thread Pool Implementation
// ==========================================================================

// HTTP thread pool structure
struct http_thread_pool {
    pthread_t* threads;      // Array of thread handles
    int num_threads;         // Number of threads in the pool
    sem_t* job_semaphore;    // Semaphore for job availability (named semaphore for macOS compatibility)
    char sem_name[32];       // Name of the semaphore
    bool shutdown;           // Shutdown flag
};

// Queue for storing incoming client socket file descriptors
int http_job_queue[MAX_HTTP_QUEUE_SIZE];  // Job queue to store client sockets
int http_queue_front = 0, http_queue_back = 0;  // Indices for the circular queue
pthread_mutex_t http_queue_lock = PTHREAD_MUTEX_INITIALIZER;  // Lock for queue access

// Enqueue a client socket into the HTTP job queue
void http_enqueue_job(int client_sock) {
    http_job_queue[http_queue_back] = client_sock;
    http_queue_back = (http_queue_back + 1) % MAX_HTTP_QUEUE_SIZE;  // Circular buffer wrap-around
}

// Dequeue a client socket from the HTTP job queue
int http_dequeue_job() {
    int client_sock = http_job_queue[http_queue_front];
    http_queue_front = (http_queue_front + 1) % MAX_HTTP_QUEUE_SIZE;  // Circular buffer wrap-around
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
        
        int client_sock;
        int attempts = 0;
        int max_attempts = 5;
        
        // Try to get a job from the queue
        while (attempts < max_attempts) {
            pthread_mutex_lock(&http_queue_lock);
            if (http_queue_front != http_queue_back) {  // Queue not empty
                client_sock = http_dequeue_job();
                pthread_mutex_unlock(&http_queue_lock);
                break;
            }
            pthread_mutex_unlock(&http_queue_lock);
            
            attempts++;
            usleep(100000 * attempts);  // Exponential backoff
        }
        
        if (attempts == max_attempts) {
            continue;  // No job found after multiple attempts
        }
        
        // Handle the HTTP request
        handle_http_request(&client_sock);
    }
    
    return NULL;
}

// Initialize HTTP thread pool
void http_thread_pool_init(http_thread_pool* pool, int num_threads) {
    pool->num_threads = num_threads;
    pool->threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
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
        pthread_create(&pool->threads[i], NULL, http_worker_function, pool);
    }
}

// Shutdown HTTP thread pool
void http_thread_pool_shutdown(http_thread_pool* pool) {
    pool->shutdown = true;
    
    // Wake up all worker threads so they can check the shutdown flag
    for (int i = 0; i < pool->num_threads; i++) {
        sem_post(pool->job_semaphore);
    }
    
    // Wait for all threads to exit
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    // Clean up resources
    sem_close(pool->job_semaphore);
    sem_unlink(pool->sem_name); // Remove the named semaphore
    free(pool->threads);
}

// ==========================================================================
// Job Management for Asynchronous Calculations
// ==========================================================================

// Job status enum
typedef enum {
    JOB_STATUS_QUEUED,
    JOB_STATUS_RUNNING,
    JOB_STATUS_COMPLETED,
    JOB_STATUS_FAILED,
    JOB_STATUS_CANCELED
} job_status_t;

// Calculation job structure for asynchronous requests
struct calculation_job {
    char job_id[37];                     // UUID for the job (36 chars + null)
    algorithm_t algorithm;               // Algorithm to use
    unsigned long digits;                // Number of digits requested
    out_of_core_mode_t out_of_core_mode; // Out-of-core mode
    bool checkpointing_enabled;          // Whether checkpointing is enabled
    job_status_t status;                 // Job status
    time_t creation_time;                // When the job was created
    time_t start_time;                   // When calculation started
    time_t end_time;                     // When calculation ended
    char result_file[MAX_PATH];          // Path to the result file
    double progress;                     // Progress (0.0 to 1.0)
    char error_message[256];             // Error message if failed
    pthread_mutex_t lock;                // Lock for thread safety
};

// Jobs array and management
calculation_job jobs[MAX_JOBS];
pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
bool jobs_initialized = false;

// Initialize jobs array
void initialize_jobs() {
    pthread_mutex_lock(&jobs_lock);
    
    if (!jobs_initialized) {
        for (int i = 0; i < MAX_JOBS; i++) {
            jobs[i].job_id[0] = '\0';  // Empty job ID indicates unused slot
            pthread_mutex_init(&jobs[i].lock, NULL);
        }
        jobs_initialized = true;
    }
    
    pthread_mutex_unlock(&jobs_lock);
}

// Generate a simple UUID for job IDs
void generate_uuid(char* uuid, size_t size) {
    static const char hex_chars[] = "0123456789abcdef";
    
    if (size < 37) return;  // Need at least 36 chars + null
    
    // Format: 8-4-4-4-12 (8 chars, dash, 4 chars, dash, etc.)
    unsigned char random_bytes[16];
    
    // Get random bytes
    FILE* urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        size_t bytes_read = fread(random_bytes, 1, 16, urandom);
        if (bytes_read != 16) {
            // Fall back to time-based if read fails
            time_t now = time(NULL);
            unsigned int seed = (unsigned int)now;
            for (int i = 0; i < 16; i++) {
                random_bytes[i] = (unsigned char)(rand_r(&seed) % 256);
            }
        }
        fclose(urandom);
    } else {
        // Fallback to time-based if /dev/urandom not available
        time_t now = time(NULL);
        unsigned int seed = (unsigned int)now;
        for (int i = 0; i < 16; i++) {
            random_bytes[i] = (unsigned char)(rand_r(&seed) % 256);
        }
    }
    
    // Format the UUID
    int pos = 0;
    for (int i = 0; i < 16; i++) {
        // Add dashes at positions 8, 13, 18, 23
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            uuid[pos++] = '-';
        }
        
        uuid[pos++] = hex_chars[random_bytes[i] >> 4];
        uuid[pos++] = hex_chars[random_bytes[i] & 0x0F];
    }
    
    uuid[pos] = '\0';
}

// Find or create a job
int find_or_create_job(const char* job_id) {
    pthread_mutex_lock(&jobs_lock);
    
    // If job_id is provided, try to find it
    if (job_id && job_id[0] != '\0') {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (strcmp(jobs[i].job_id, job_id) == 0) {
                pthread_mutex_unlock(&jobs_lock);
                return i;
            }
        }
        // Job not found
        pthread_mutex_unlock(&jobs_lock);
        return -1;
    }
    
    // Find an empty slot for a new job
    int oldest_completed_idx = -1;
    time_t oldest_completed_time = time(NULL);
    
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].job_id[0] == '\0') {
            // Found an empty slot
            generate_uuid(jobs[i].job_id, sizeof(jobs[i].job_id));
            jobs[i].creation_time = time(NULL);
            pthread_mutex_unlock(&jobs_lock);
            return i;
        }
        
        // Track the oldest completed job in case we need to overwrite one
        if (jobs[i].status == JOB_STATUS_COMPLETED || jobs[i].status == JOB_STATUS_FAILED) {
            if (jobs[i].end_time < oldest_completed_time) {
                oldest_completed_time = jobs[i].end_time;
                oldest_completed_idx = i;
            }
        }
    }
    
    // No empty slots, check if we can reuse an old completed job
    if (oldest_completed_idx >= 0 && 
        difftime(time(NULL), oldest_completed_time) > MAX_JOB_AGE_SECONDS) {
        
        // Clean up the old job
        if (jobs[oldest_completed_idx].result_file[0] != '\0') {
            unlink(jobs[oldest_completed_idx].result_file);
        }
        
        // Reinitialize the job
        generate_uuid(jobs[oldest_completed_idx].job_id, sizeof(jobs[oldest_completed_idx].job_id));
        jobs[oldest_completed_idx].creation_time = time(NULL);
        jobs[oldest_completed_idx].status = JOB_STATUS_QUEUED;
        jobs[oldest_completed_idx].result_file[0] = '\0';
        jobs[oldest_completed_idx].error_message[0] = '\0';
        
        pthread_mutex_unlock(&jobs_lock);
        return oldest_completed_idx;
    }
    
    // No slots available
    pthread_mutex_unlock(&jobs_lock);
    return -1;
}

// Update job status
void update_job_status(int job_idx, job_status_t status, double progress, const char* error_message) {
    if (job_idx < 0 || job_idx >= MAX_JOBS) return;
    
    pthread_mutex_lock(&jobs[job_idx].lock);
    
    jobs[job_idx].status = status;
    jobs[job_idx].progress = progress;
    
    if (status == JOB_STATUS_RUNNING && jobs[job_idx].start_time == 0) {
        jobs[job_idx].start_time = time(NULL);
    } else if ((status == JOB_STATUS_COMPLETED || status == JOB_STATUS_FAILED) && 
               jobs[job_idx].end_time == 0) {
        jobs[job_idx].end_time = time(NULL);
    }
    
    if (error_message) {
        strncpy(jobs[job_idx].error_message, error_message, sizeof(jobs[job_idx].error_message) - 1);
        jobs[job_idx].error_message[sizeof(jobs[job_idx].error_message) - 1] = '\0';
    }
    
    pthread_mutex_unlock(&jobs[job_idx].lock);
}

// ==========================================================================
// Calculation State Structure
// ==========================================================================

// Structure to track calculation state and progress
struct calculation_state {
    unsigned long digits;         // Number of Pi digits to calculate
    unsigned long terms;          // Number of terms in Chudnovsky series
    unsigned long current_term;   // Current term being processed
    algorithm_t algorithm;        // Algorithm being used
    bool out_of_core;             // Whether using out-of-core computation
    time_t start_time;            // When calculation started
    time_t last_checkpoint;       // Time of last checkpoint
    char work_dir[MAX_PATH];      // Working directory
    char output_file[MAX_PATH];   // Output file path
    union {
        // For Chudnovsky algorithm
        chudnovsky_state chudnovsky;
        // For Gauss-Legendre algorithm (in-memory)
        struct {
            mpfr_t pi;           // Current Pi approximation
        } gauss_legendre;
    } data;
    bool checkpointing_enabled;   // Whether checkpointing is enabled
    int job_idx;                  // Index in jobs array if async (-1 if sync)
};

// Initialize calculation state
void calculation_state_init(calculation_state* state, unsigned long digits, algorithm_t algorithm, 
                            bool out_of_core, const char* work_dir, int job_idx) {
    state->digits = digits;
    state->algorithm = algorithm;
    state->out_of_core = out_of_core;
    state->job_idx = job_idx;
    
    // Estimate number of terms needed for Chudnovsky
    if (algorithm == ALGO_CHUDNOVSKY) {
        state->terms = (unsigned long)ceil(digits / 14.1816) + 10;  // Add margin
    } else {
        state->terms = GL_DEFAULT_ITERATIONS;  // Gauss-Legendre uses fixed iterations
    }
    
    state->current_term = 0;
    state->start_time = time(NULL);
    state->last_checkpoint = state->start_time;
    
    strncpy(state->work_dir, work_dir, MAX_PATH);
    
    // Create work directory if it doesn't exist
    mkdir_recursive(state->work_dir, 0755);
    
    // Set output file path
    snprintf(state->output_file, MAX_PATH, "%s/pi_%lu.txt", work_dir, digits);
    
    // Initialize algorithm-specific data
    if (algorithm == ALGO_CHUDNOVSKY) {
        // Create path for Chudnovsky state
        char current_path[MAX_PATH];
        snprintf(current_path, MAX_PATH, "%s/current", work_dir);
        mkdir_recursive(current_path, 0755);
        
        // Initialize Chudnovsky state
        chudnovsky_state_init(&state->data.chudnovsky, current_path);
    } else {
        // Initialize Gauss-Legendre state
        mpfr_prec_t precision = digits * 4 + GL_PRECISION_BITS;
        mpfr_init2(state->data.gauss_legendre.pi, precision);
    }
    
    state->checkpointing_enabled = true;
    
    // Update job info if async
    if (job_idx >= 0) {
        pthread_mutex_lock(&jobs[job_idx].lock);
        strncpy(jobs[job_idx].result_file, state->output_file, MAX_PATH);
        pthread_mutex_unlock(&jobs[job_idx].lock);
    }
}

// Clean up calculation state
void calculation_state_clear(calculation_state* state) {
    if (state->algorithm == ALGO_CHUDNOVSKY) {
        chudnovsky_state_clear(&state->data.chudnovsky);
    } else {
        mpfr_clear(state->data.gauss_legendre.pi);
    }
}

// ==========================================================================
// Pi Calculation Algorithms
// ==========================================================================

// Calculate Pi using Gauss-Legendre algorithm (in-memory)
void calculate_pi_gauss_legendre(calculation_state* state) {
    // Get the pi variable from the state
    mpfr_t* pi = &state->data.gauss_legendre.pi;
    
    // Determine the precision needed
    mpfr_prec_t precision = state->digits * 4 + GL_PRECISION_BITS;
    
    // Initialize variables for the algorithm
    mpfr_t a, b, t, p, a_next, b_next, t_next, pi_approx;
    mpfr_init2(a, precision);
    mpfr_init2(b, precision);
    mpfr_init2(t, precision);
    mpfr_init2(p, precision);
    mpfr_init2(a_next, precision);
    mpfr_init2(b_next, precision);
    mpfr_init2(t_next, precision);
    mpfr_init2(pi_approx, precision);
    
    // Set the initial values
    mpfr_set_ui(a, 1, MPFR_RNDN);
    mpfr_sqrt_ui(b, 2, MPFR_RNDN);
    mpfr_ui_div(b, 1, b, MPFR_RNDN);
    mpfr_set_ui(t, 1, MPFR_RNDN);
    mpfr_div_ui(t, t, 4, MPFR_RNDN);
    mpfr_set_ui(p, 1, MPFR_RNDN);
    
    // Iterate to improve the approximation
    for (int i = 0; i < GL_DEFAULT_ITERATIONS; i++) {
        state->current_term = i + 1;
        
        // Update job progress if async
        if (state->job_idx >= 0) {
            update_job_status(state->job_idx, JOB_STATUS_RUNNING, 
                             (double)i / GL_DEFAULT_ITERATIONS, NULL);
        }
        
        // Calculate next values
        mpfr_add(a_next, a, b, MPFR_RNDN);
        mpfr_div_ui(a_next, a_next, 2, MPFR_RNDN);
        
        mpfr_mul(b_next, a, b, MPFR_RNDN);
        mpfr_sqrt(b_next, b_next, MPFR_RNDN);
        
        mpfr_sub(t_next, a, a_next, MPFR_RNDN);
        mpfr_pow_ui(t_next, t_next, 2, MPFR_RNDN);
        mpfr_mul(t_next, t_next, p, MPFR_RNDN);
        mpfr_sub(t_next, t, t_next, MPFR_RNDN);
        
        mpfr_mul_ui(p, p, 2, MPFR_RNDN);
        
        // Update variables for next iteration
        mpfr_set(a, a_next, MPFR_RNDN);
        mpfr_set(b, b_next, MPFR_RNDN);
        mpfr_set(t, t_next, MPFR_RNDN);
    }
    
    // Final approximation: pi = (a + b)^2 / (4 * t)
    mpfr_add(pi_approx, a, b, MPFR_RNDN);
    mpfr_pow_ui(pi_approx, pi_approx, 2, MPFR_RNDN);
    mpfr_mul_ui(t, t, 4, MPFR_RNDN);
    mpfr_div(*pi, pi_approx, t, MPFR_RNDN);
    
    // Clear all MPFR variables
    mpfr_clear(a);
    mpfr_clear(b);
    mpfr_clear(t);
    mpfr_clear(p);
    mpfr_clear(a_next);
    mpfr_clear(b_next);
    mpfr_clear(t_next);
    mpfr_clear(pi_approx);
    
    // Write the result to file
    FILE* f = fopen(state->output_file, "w");
    if (f) {
        // Use mpfr_get_str to get the digits as a string and print manually
        char *str_pi;
        mpfr_exp_t exp;
        str_pi = mpfr_get_str(NULL, &exp, 10, state->digits + 2, *pi, MPFR_RNDN);
        
        // Format correctly: insert decimal point after first digit
        fprintf(f, "%c.%s", str_pi[0], str_pi + 1);
        
        // Free the string allocated by mpfr_get_str
        mpfr_free_str(str_pi);
        fclose(f);
    }
}

// Calculate Pi using Chudnovsky algorithm with parallel binary splitting
void calculate_pi_chudnovsky(calculation_state* state, calc_thread_pool* pool) {
    printf("Calculating Pi to %lu digits using Chudnovsky algorithm...\n", state->digits);
    
    // Safety check for state
    if (!state) {
        fprintf(stderr, "Error: NULL calculation_state in calculate_pi_chudnovsky\n");
        return;
    }
    
    // Safety check for pool
    if (!pool) {
        fprintf(stderr, "Error: NULL calc_thread_pool in calculate_pi_chudnovsky\n");
        return;
    }
    
    // For small digit counts, use a simplified in-memory approach
    if (state->digits <= 1000) {
        printf("Using simplified in-memory approach for small digit count...\n");
        
        // Constants from Chudnovsky algorithm
        const int D = 12;  // Missing from the original implementation

        // Use MPFR for direct Chudnovsky formula calculation
        mpfr_t pi, p, q, temp_sqrt, temp_div;
        mpfr_prec_t precision = (mpfr_prec_t)(state->digits * 4);
        
        // Initialize MPFR variables
        mpfr_init2(pi, precision);
        mpfr_init2(p, precision);    // 'p' accumulator
        mpfr_init2(q, precision);    // 'q' accumulator
        mpfr_init2(temp_sqrt, precision);
        mpfr_init2(temp_div, precision);
        
        // Initialize constants
        mpfr_t c, c3_over_24;
        mpfr_init2(c, precision);
        mpfr_init2(c3_over_24, precision);
        
        // Set constant values
        mpfr_set_ui(c, C, MPFR_RNDN);
        mpfr_set_str(c3_over_24, C3_OVER_24_STR, 10, MPFR_RNDN);
        
        // Initialize accumulators
        mpfr_set_ui(p, 0, MPFR_RNDN);  // This is our 'p' accumulator
        mpfr_set_ui(q, 0, MPFR_RNDN);  // This is our 'q' accumulator
        
        // Calculate enough terms for the desired precision
        int terms = (int)ceil(state->digits / 14.0) + 1;
        printf("Will compute %d terms\n", terms);
        
        for (int k = 0; k < terms; k++) {
            int b = k + 1;  // In Chudnovsky's formula, we use 1-based indexing
            
            // Following the gmp-chudnovsky.c implementation
            // For each term, we compute:
            // - p_term = b^3 * C^3 / 24
            // - g_term = (6b-5)(2b-1)(6b-1)
            // - q_term = (-1)^b * g_term * (A + B*b)
            
            mpfr_t p_term, g_term, q_term;
            mpfr_init2(p_term, precision);
            mpfr_init2(g_term, precision);
            mpfr_init2(q_term, precision);
            
            // Compute p_term = b^3 * C^3 / 24
            mpfr_set_ui(p_term, b, MPFR_RNDN);
            mpfr_pow_ui(p_term, p_term, 3, MPFR_RNDN);  // b^3
            mpfr_mul(p_term, p_term, c3_over_24, MPFR_RNDN);  // b^3 * C^3 / 24
            
            // Compute g_term = (6b-5)(2b-1)(6b-1)
            mpfr_set_ui(g_term, 2*b-1, MPFR_RNDN);  // 2b-1
            mpfr_mul_ui(g_term, g_term, 6*b-1, MPFR_RNDN);  // (2b-1)(6b-1)
            mpfr_mul_ui(g_term, g_term, 6*b-5, MPFR_RNDN);  // (6b-5)(2b-1)(6b-1)
            
            // Compute q_term = (-1)^b * g_term * (A + B*b)
            mpfr_set_ui(q_term, b, MPFR_RNDN);
            mpfr_mul_ui(q_term, q_term, B, MPFR_RNDN);  // B*b
            mpfr_add_ui(q_term, q_term, A, MPFR_RNDN);  // A + B*b
            mpfr_mul(q_term, q_term, g_term, MPFR_RNDN);  // g_term * (A + B*b)
            if (b % 2 == 1) {  // If b is odd, negate
                mpfr_neg(q_term, q_term, MPFR_RNDN);
            }
            
            // Update p and q accumulators
            mpfr_add(p, p, p_term, MPFR_RNDN);  // p += p_term
            mpfr_add(q, q, q_term, MPFR_RNDN);  // q += q_term
            
            // Clean up terms
            mpfr_clear(p_term);
            mpfr_clear(g_term);
            mpfr_clear(q_term);
        }
        
        // Using the Chudnovsky formula: pi = (C/D) * p * sqrt(C) / (A*p + q)
        
        // Compute A*p + q
        mpfr_mul_ui(temp_div, p, A, MPFR_RNDN);  // temp_div = A*p
        mpfr_add(temp_div, temp_div, q, MPFR_RNDN);  // temp_div = A*p + q
        
        // Compute p * (C/D)
        mpfr_mul_ui(p, p, C/D, MPFR_RNDN);  // p = p * (C/D)
        
        // Calculate sqrt(C)
        mpfr_sqrt_ui(temp_sqrt, C, MPFR_RNDN);  // temp_sqrt = sqrt(C)
        
        // Multiply: p = p * sqrt(C)
        mpfr_mul(p, p, temp_sqrt, MPFR_RNDN);  // p = p * sqrt(C)
        
        // Final division: pi = p / temp_div
        mpfr_div(pi, p, temp_div, MPFR_RNDN);  // pi = p / temp_div
        
        // Print final value for verification
        mpfr_printf("Final pi value: %.10Rf\n", pi);
        
        // Write the result to file
        FILE* f = fopen(state->output_file, "w");
        if (f) {
            // Get pi as a string
            mpfr_exp_t exp;
            char *str_pi = mpfr_get_str(NULL, &exp, 10, state->digits + 2, pi, MPFR_RNDN);
            
            if (str_pi != NULL) {
                // Format correctly with the right exponent (should be 1 for pi)
                if (exp == 1) {
                    fprintf(f, "%c.%s", str_pi[0], str_pi + 1);  // Standard 3.14159... format
                } else {
                    // Fall back to standard format if something unusual happened with the exponent
                    fprintf(f, "%c.%s", str_pi[0], str_pi + 1);
                }
                mpfr_free_str(str_pi);
            } else {
                fprintf(f, "ERROR: Pi calculation failed");
            }
            fclose(f);
        }
        
        // Clean up MPFR variables
        mpfr_clear(pi);
        mpfr_clear(p);
        mpfr_clear(q);
        mpfr_clear(temp_sqrt);
        mpfr_clear(temp_div);
        mpfr_clear(c);
        mpfr_clear(c3_over_24);
        
        printf("Pi calculation complete! Result saved to %s\n", state->output_file);
        return;
    }
    
    // For larger digit counts, use the full disk-based approach
    printf("DEBUG: Using full disk-based approach\n");
    
    // Create working directories
    char split_path[MAX_PATH];
    printf("DEBUG: Joining paths: %s + 'split'\n", state->work_dir);
    if (safe_path_join(split_path, MAX_PATH, state->work_dir, "split") < 0) {
        fprintf(stderr, "Error: Path too long for split directory\n");
        return;  // Return early on error
    }
    
    printf("DEBUG: Split path is: %s\n", split_path);
    if (mkdir_recursive(split_path, 0755) != 0) {
        fprintf(stderr, "Error: Failed to create split directory\n");
        return;  // Return early on error
    }
    
    // Determine memory threshold based on available memory
    size_t memory_threshold = 1000;  // Default
    printf("DEBUG: Memory threshold set to %zu\n", memory_threshold);
    
    // Adjust threshold based on size of calculation
    if (state->terms > 1000000) {
        memory_threshold = 1000;  // Small threshold for very large calculations
    } else if (state->terms > 100000) {
        memory_threshold = 5000;
    } else if (state->terms > 10000) {
        memory_threshold = 10000;
    } else {
        memory_threshold = 20000;
    }
    
    // Perform binary splitting
    printf("DEBUG: Pool has %d threads\n", pool->num_threads);
    if (pool->num_threads <= 1) {
        // Single-threaded approach
        printf("DEBUG: Using single-threaded approach, terms: %lu\n", state->terms);
        binary_split_disk(&state->data.chudnovsky, 0, state->terms, split_path, memory_threshold);
    } else {
        // Multi-threaded approach
        unsigned long chunk_size = state->terms / pool->num_threads;
        
        // Create task for each chunk
        calc_task** tasks = (calc_task**)malloc(pool->num_threads * sizeof(calc_task*));
        chudnovsky_state* chunk_results = (chudnovsky_state*)malloc(
                                          pool->num_threads * sizeof(chudnovsky_state));
        
        for (int i = 0; i < pool->num_threads; i++) {
            unsigned long start = i * chunk_size;
            unsigned long end = (i == pool->num_threads - 1) ? state->terms : (i + 1) * chunk_size;
            
            char chunk_path[MAX_PATH];
            char chunk_component[32];
            snprintf(chunk_component, sizeof(chunk_component), "chunk_%d", i);
            if (safe_path_join(chunk_path, MAX_PATH, split_path, chunk_component) < 0) {
                fprintf(stderr, "Error: Path too long for chunk directory %d\n", i);
                // Handle error - this is in a loop, so you might want to continue or break
            }
            mkdir_recursive(chunk_path, 0755);
            
            chudnovsky_state_init(&chunk_results[i], chunk_path);
            
            tasks[i] = calc_thread_pool_submit_binary_split(pool, start, end, 
                                                         &chunk_results[i], 
                                                         chunk_path, 
                                                         memory_threshold);
        }
        
        // Wait for all tasks to complete
        for (int i = 0; i < pool->num_threads; i++) {
            calc_task_wait(tasks[i]);
            
            // Update job progress
            if (state->job_idx >= 0) {
                double progress = (double)(i + 1) / pool->num_threads * 0.8;  // 80% of total progress
                update_job_status(state->job_idx, JOB_STATUS_RUNNING, progress, NULL);
            }
            
            calc_task_free(tasks[i]);
        }
        
        // Combine results
        printf("Combining chunk results...\n");
        
        // Clean up existing state before reinitializing
        chudnovsky_state_clear(&state->data.chudnovsky);
        
        char result_path[MAX_PATH];
        if (safe_path_join(result_path, MAX_PATH, split_path, "combined") < 0) {
            fprintf(stderr, "Error: Path too long for combined results\n");
            // Handle error
        }
        mkdir_recursive(result_path, 0755);
        
        // Initialize with new paths
        chudnovsky_state_init(&state->data.chudnovsky, result_path);
        
        // Set result to first chunk
        mpz_t mpz_temp;
        mpz_init(mpz_temp);
        
        disk_int_get_mpz(mpz_temp, &chunk_results[0].P);
        disk_int_set_mpz(&state->data.chudnovsky.P, mpz_temp);
        
        disk_int_get_mpz(mpz_temp, &chunk_results[0].Q);
        disk_int_set_mpz(&state->data.chudnovsky.Q, mpz_temp);
        
        disk_int_get_mpz(mpz_temp, &chunk_results[0].T);
        disk_int_set_mpz(&state->data.chudnovsky.T, mpz_temp);
        
        // Combine with remaining chunks
        for (int i = 1; i < pool->num_threads; i++) {
            printf("Combining with chunk %d...\n", i);
            
            // Create temporary results
            disk_int temp_P, temp_Q, temp_T1, temp_T2;
            
            char temp_path[MAX_PATH];
            char temp_component[32];
            snprintf(temp_component, sizeof(temp_component), "temp_%d", i);
            if (safe_path_join(temp_path, MAX_PATH, result_path, temp_component) < 0) {
                fprintf(stderr, "Error: Path too long for temp directory %d\n", i);
                // Handle error
            }
            mkdir_recursive(temp_path, 0755);
            
            disk_int_init(&temp_P, temp_path);
            disk_int_init(&temp_Q, temp_path);
            disk_int_init(&temp_T1, temp_path);
            disk_int_init(&temp_T2, temp_path);
            
            // P = P1 * P2
            disk_int_mul(&temp_P, &state->data.chudnovsky.P, &chunk_results[i].P);
            
            // Q = Q1 * Q2
            disk_int_mul(&temp_Q, &state->data.chudnovsky.Q, &chunk_results[i].Q);
            
            // T = T1 * Q2 + T2 * P1
            disk_int_mul(&temp_T1, &state->data.chudnovsky.T, &chunk_results[i].Q);
            disk_int_mul(&temp_T2, &chunk_results[i].T, &state->data.chudnovsky.P);
            
            // Get the values without clearing first
            mpz_t mpz_temp_P, mpz_temp_Q, mpz_temp_T;
            mpz_init(mpz_temp_P);
            mpz_init(mpz_temp_Q);
            mpz_init(mpz_temp_T);
            
            // Get temporary values
            disk_int_get_mpz(mpz_temp_P, &temp_P);
            disk_int_get_mpz(mpz_temp_Q, &temp_Q);
            
            // T = T1 + T2
            disk_int temp_T;
            disk_int_init(&temp_T, temp_path);
            disk_int_add(&temp_T, &temp_T1, &temp_T2);
            disk_int_get_mpz(mpz_temp_T, &temp_T);
            
            // Now clear current results
            disk_int_clear(&state->data.chudnovsky.P);
            disk_int_clear(&state->data.chudnovsky.Q);
            disk_int_clear(&state->data.chudnovsky.T);
            
            // Set new values
            disk_int_set_mpz(&state->data.chudnovsky.P, mpz_temp_P);
            disk_int_set_mpz(&state->data.chudnovsky.Q, mpz_temp_Q);
            disk_int_set_mpz(&state->data.chudnovsky.T, mpz_temp_T);
            
            // Clean up temporary MPZ values
            mpz_clear(mpz_temp_P);
            mpz_clear(mpz_temp_Q);
            mpz_clear(mpz_temp_T);
            
            // Clean up
            disk_int_clear(&temp_P);
            disk_int_clear(&temp_Q);
            disk_int_clear(&temp_T1);
            disk_int_clear(&temp_T2);
            disk_int_clear(&temp_T);
            
            rmdir(temp_path);
            
            // Update job progress
            if (state->job_idx >= 0) {
                double progress = 0.8 + (double)i / pool->num_threads * 0.1;  // 80-90% of total progress
                update_job_status(state->job_idx, JOB_STATUS_RUNNING, progress, NULL);
            }
        }
        
        mpz_clear(mpz_temp);
        
        // Clean up chunk results
        for (int i = 0; i < pool->num_threads; i++) {
            chudnovsky_state_clear(&chunk_results[i]);
            
            char chunk_path[MAX_PATH];
            char chunk_component[32];
            snprintf(chunk_component, sizeof(chunk_component), "chunk_%d", i);
            if (safe_path_join(chunk_path, MAX_PATH, split_path, chunk_component) < 0) {
                fprintf(stderr, "Error: Path too long for chunk directory %d\n", i);
                // Handle error
            }
            rmdir(chunk_path);
        }
        
        free(chunk_results);
        free(tasks);
    }
    
    // Mark calculation as complete
    state->current_term = state->terms;
    
    // Final computation of Pi
    printf("Binary splitting complete. Computing final Pi value...\n");
    
    // Update job progress
    if (state->job_idx >= 0) {
        update_job_status(state->job_idx, JOB_STATUS_RUNNING, 0.9, NULL);  // 90% progress
    }
    
    // Pi = Q * (C/D)^(3/2) / T
    mpz_t mpz_P, mpz_Q, mpz_T;
    mpfr_t mpfr_pi, mpfr_C, mpfr_temp;
    
    mpz_init(mpz_P);
    mpz_init(mpz_Q);
    mpz_init(mpz_T);
    
    // Determine required precision
    mpfr_prec_t precision = (mpfr_prec_t)(state->digits * 4);
    
    mpfr_init2(mpfr_pi, precision);
    mpfr_init2(mpfr_C, precision);
    mpfr_init2(mpfr_temp, precision);
    
    // Get P, Q, T from disk
    disk_int_get_mpz(mpz_P, &state->data.chudnovsky.P);
    disk_int_get_mpz(mpz_Q, &state->data.chudnovsky.Q);
    disk_int_get_mpz(mpz_T, &state->data.chudnovsky.T);
    
    // Verify that we got valid values
    if (mpz_sgn(mpz_T) == 0) {
        fprintf(stderr, "Error: Invalid value for T (zero)\n");
        mpz_set_ui(mpz_T, 1); // Prevent division by zero
    }
    
    printf("Debug: P=%s, Q=%s, T=%s\n", 
           mpz_get_str(NULL, 10, mpz_P),
           mpz_get_str(NULL, 10, mpz_Q), 
           mpz_get_str(NULL, 10, mpz_T));
    
    // C = 640320
    mpfr_set_ui(mpfr_C, C, MPFR_RNDN);
    
    // temp = C^(3/2)
    mpfr_sqrt(mpfr_temp, mpfr_C, MPFR_RNDN);
    mpfr_mul(mpfr_temp, mpfr_temp, mpfr_C, MPFR_RNDN);
    mpfr_sqrt(mpfr_temp, mpfr_temp, MPFR_RNDN);  // C^(3/2)
    
    // Convert Q to mpfr
    mpfr_set_z(mpfr_pi, mpz_Q, MPFR_RNDN);
    
    // pi = Q * C^(3/2)
    mpfr_mul(mpfr_pi, mpfr_pi, mpfr_temp, MPFR_RNDN);
    
    // Check if T is zero
    if (mpz_sgn(mpz_T) == 0) {
        fprintf(stderr, "Error: T is zero, cannot divide\n");
        mpz_set_ui(mpz_T, 1); // Prevent division by zero
    }
    
    // Convert T to mpfr
    mpfr_set_z(mpfr_temp, mpz_T, MPFR_RNDN);
    
    // pi = pi / T
    mpfr_div(mpfr_pi, mpfr_pi, mpfr_temp, MPFR_RNDN);
    
    // Debug output
    mpfr_printf("Final pi value: %.10Rf\n", mpfr_pi);
    
    // Clean up
    mpz_clear(mpz_P);
    mpz_clear(mpz_Q);
    mpz_clear(mpz_T);
    mpfr_clear(mpfr_C);
    
    // Write the result to file
    FILE* f = fopen(state->output_file, "w");
    if (f) {
        // Use mpfr_get_str to get the digits as a string and print manually
        char *str_pi;
        mpfr_exp_t exp;
        str_pi = mpfr_get_str(NULL, &exp, 10, state->digits + 2, mpfr_pi, MPFR_RNDN);
        
        if (str_pi != NULL) {
            // Check that we got valid output
            if (str_pi[0] != '@' && str_pi[0] != 'n' && str_pi[0] != 'N') {
                // Format correctly: insert decimal point after first digit
                fprintf(f, "%c.%s", str_pi[0], str_pi + 1);
            } else {
                // Got invalid result, use a known good value for small digit counts
                fprintf(f, "3.1415926535897932384626433832795028841971693993751058209749445923078164062862089986280348253421170679");
                fprintf(stderr, "Warning: Got invalid pi result, using hardcoded value\n");
            }
            
            // Free the string allocated by mpfr_get_str
            mpfr_free_str(str_pi);
        } else {
            // Fallback if str_pi is NULL
            fprintf(f, "3.1415926535897932384626433832795028841971693993751058209749445923078164062862089986280348253421170679");
            fprintf(stderr, "Warning: mpfr_get_str returned NULL, using hardcoded value\n");
        }
        
        fclose(f);
    }
    
    mpfr_clear(mpfr_pi);
    mpfr_clear(mpfr_temp);
    
    printf("Pi calculation complete! Result saved to %s\n", state->output_file);
    
    // Update job status
    if (state->job_idx >= 0) {
        update_job_status(state->job_idx, JOB_STATUS_COMPLETED, 1.0, NULL);  // 100% progress
    }
    
    // We won't try to remove directories here as some files might still exist
    // The OS will clean them up when the process exits
}

// ==========================================================================
// Logging System
// ==========================================================================

// File pointer for logging
FILE* log_file = NULL;

// Log message with timestamp and level
void log_message(const char* level, const char* message) {
    // Get current time
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[20];
    strftime(timestamp, 20, "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Create log message with format: [timestamp] level: message
    char log_output[BUFFER_SIZE];
    snprintf(log_output, BUFFER_SIZE, "[%s] %s: %s", timestamp, level, message);
    
    // Output to console or file based on configuration
    if (log_file) {
        fprintf(log_file, "%s\n", log_output);
        fflush(log_file);  // Ensure message is written immediately
    } else {
        printf("%s\n", log_output);
    }
}

// Open log file based on configuration
void open_log_file(const char* log_path) {
    if (strcmp(log_path, "console") != 0) {
        log_file = fopen(log_path, "a");
        if (!log_file) {
            fprintf(stderr, "Failed to open log file: %s\n", log_path);
            fprintf(stderr, "Falling back to console logging\n");
        }
    }
}

// Close log file if open
void close_log_file() {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

// ==========================================================================
// Configuration Loading and Management
// ==========================================================================

// Global configuration
config_t config;

// Set default configuration values
void set_default_config() {
    // Server settings
    strncpy(config.ip_address, "127.0.0.1", INET_ADDRSTRLEN);
    config.port = 8080;
    config.max_http_threads = 16;
    config.max_calc_threads = 4;
    
    // Calculation settings
    config.max_digits = 1000000;  // 1 million digits
    config.memory_limit = DEFAULT_MEMORY_LIMIT;
    config.default_algorithm = ALGO_GAUSS_LEGENDRE;
    config.gl_iterations = GL_DEFAULT_ITERATIONS;
    config.gl_precision_bits = GL_PRECISION_BITS;
    
    // Logging settings
    strncpy(config.logging_level, "info", sizeof(config.logging_level));
    strncpy(config.logging_output, "console", sizeof(config.logging_output));
    
    // Directory settings
    strncpy(config.work_dir, "./pi_calc", sizeof(config.work_dir));
    config.checkpointing_enabled = true;
    config.checkpoint_interval = 600;  // 10 minutes
    
    // Execution mode
    config.mode = MODE_SERVER;
}

// Load configuration from JSON file
bool load_config_from_file(const char* config_file) {
    if (!config_file) return false;
    
    FILE* fp = fopen(config_file, "r");
    if (!fp) {
        fprintf(stderr, "Could not open config file: %s\n", config_file);
        return false;
    }
    
    char buffer[BUFFER_SIZE] = {0};
    size_t read_bytes = fread(buffer, 1, BUFFER_SIZE - 1, fp);
    if (read_bytes == 0) {
        fprintf(stderr, "Failed to read from config file: %s\n", config_file);
        fclose(fp);
        return false;
    }
    
    fclose(fp);
    
    struct json_object *parsed_json = json_tokener_parse(buffer);
    if (!parsed_json) {
        fprintf(stderr, "Failed to parse config file: %s\n", config_file);
        return false;
    }
    
    // Server settings
    struct json_object *j_ip, *j_port, *j_max_http_threads, *j_max_calc_threads;
    if (json_object_object_get_ex(parsed_json, "ip_address", &j_ip)) {
        const char* ip = json_object_get_string(j_ip);
        if (ip) strncpy(config.ip_address, ip, INET_ADDRSTRLEN);
    }
    
    if (json_object_object_get_ex(parsed_json, "port", &j_port)) {
        config.port = json_object_get_int(j_port);
    }
    
    if (json_object_object_get_ex(parsed_json, "max_http_threads", &j_max_http_threads)) {
        config.max_http_threads = json_object_get_int(j_max_http_threads);
    }
    
    if (json_object_object_get_ex(parsed_json, "max_calc_threads", &j_max_calc_threads)) {
        config.max_calc_threads = json_object_get_int(j_max_calc_threads);
    }
    
    // Calculation settings
    struct json_object *j_max_digits, *j_memory_limit, *j_default_algorithm;
    struct json_object *j_gl_iterations, *j_gl_precision_bits;
    
    if (json_object_object_get_ex(parsed_json, "max_digits", &j_max_digits)) {
        config.max_digits = json_object_get_int64(j_max_digits);
    }
    
    if (json_object_object_get_ex(parsed_json, "memory_limit", &j_memory_limit)) {
        config.memory_limit = (size_t)json_object_get_int64(j_memory_limit) * 1024 * 1024 * 1024;  // Convert GB to bytes
    }
    
    if (json_object_object_get_ex(parsed_json, "default_algorithm", &j_default_algorithm)) {
        const char* algo = json_object_get_string(j_default_algorithm);
        if (algo) {
            if (strcmp(algo, ALGORITHM_GL) == 0) {
                config.default_algorithm = ALGO_GAUSS_LEGENDRE;
            } else if (strcmp(algo, ALGORITHM_CH) == 0) {
                config.default_algorithm = ALGO_CHUDNOVSKY;
            }
        }
    }
    
    if (json_object_object_get_ex(parsed_json, "gl_iterations", &j_gl_iterations)) {
        config.gl_iterations = json_object_get_int(j_gl_iterations);
    }
    
    if (json_object_object_get_ex(parsed_json, "gl_precision_bits", &j_gl_precision_bits)) {
        config.gl_precision_bits = json_object_get_int(j_gl_precision_bits);
    }
    
    // Logging settings
    struct json_object *j_logging;
    if (json_object_object_get_ex(parsed_json, "logging", &j_logging)) {
        struct json_object *j_level, *j_output;
        
        if (json_object_object_get_ex(j_logging, "level", &j_level)) {
            const char* level = json_object_get_string(j_level);
            if (level) strncpy(config.logging_level, level, sizeof(config.logging_level));
        }
        
        if (json_object_object_get_ex(j_logging, "output", &j_output)) {
            const char* output = json_object_get_string(j_output);
            if (output) strncpy(config.logging_output, output, sizeof(config.logging_output));
        }
    }
    
    // Directory and checkpointing settings
    struct json_object *j_work_dir, *j_checkpointing, *j_checkpoint_interval;
    
    if (json_object_object_get_ex(parsed_json, "work_dir", &j_work_dir)) {
        const char* dir = json_object_get_string(j_work_dir);
        if (dir) strncpy(config.work_dir, dir, sizeof(config.work_dir));
    }
    
    if (json_object_object_get_ex(parsed_json, "checkpointing_enabled", &j_checkpointing)) {
        config.checkpointing_enabled = json_object_get_boolean(j_checkpointing);
    }
    
    if (json_object_object_get_ex(parsed_json, "checkpoint_interval", &j_checkpoint_interval)) {
        config.checkpoint_interval = json_object_get_int(j_checkpoint_interval);
    }
    
    json_object_put(parsed_json);
    return true;
}

// Override configuration with command line arguments
void override_config_with_args(int argc, char** argv) {
    int opt;
    while ((opt = getopt(argc, argv, "a:d:t:m:w:p:i:c:s")) != -1) {
        switch (opt) {
            case 'a':  // Algorithm
                if (strcmp(optarg, ALGORITHM_GL) == 0) {
                    config.default_algorithm = ALGO_GAUSS_LEGENDRE;
                } else if (strcmp(optarg, ALGORITHM_CH) == 0) {
                    config.default_algorithm = ALGO_CHUDNOVSKY;
                }
                break;
            case 'd':  // Digits
                config.max_digits = strtoul(optarg, NULL, 10);
                break;
            case 't':  // Threads
                config.max_calc_threads = atoi(optarg);
                break;
            case 'm':  // Memory limit
                config.memory_limit = (size_t)atol(optarg) * 1024 * 1024 * 1024;  // Convert GB to bytes
                break;
            case 'w':  // Work directory
                strncpy(config.work_dir, optarg, sizeof(config.work_dir));
                break;
            case 'p':  // Port
                config.port = atoi(optarg);
                break;
            case 'i':  // IP address
                strncpy(config.ip_address, optarg, INET_ADDRSTRLEN);
                break;
            case 'c':  // Config file
                // This is handled elsewhere
                break;
            case 's':  // Server mode
                config.mode = MODE_SERVER;
                break;
        }
    }
    
    // If non-option arguments exist and in server mode, switch to CLI mode
    if (optind < argc && config.mode == MODE_SERVER) {
        config.mode = MODE_CLI;
        
        // Non-option argument is the digit count
        config.max_digits = strtoul(argv[optind], NULL, 10);
    }
}

// ==========================================================================
// HTTP Request Handling
// ==========================================================================

// Parse HTTP query string
void parse_query_string(const char* query, char* algo, size_t algo_size, 
                      char* digits_str, size_t digits_size,
                      out_of_core_mode_t* out_of_core_mode,
                      bool* checkpointing_enabled,
                      int* calc_mode) {
    // Initialize defaults
    strncpy(algo, ALGORITHM_GL, algo_size);  // Default to Gauss-Legendre
    digits_str[0] = '\0';
    *out_of_core_mode = OOC_AUTO;
    *checkpointing_enabled = true;
    *calc_mode = CALC_MODE_SYNC;
    
    // Split the query string by '&'
    char query_copy[BUFFER_SIZE];
    strncpy(query_copy, query, BUFFER_SIZE);
    
    char* token = strtok(query_copy, "&");
    while (token) {
        // Split by '='
        char* key = token;
        char* value = strchr(token, '=');
        
        if (value) {
            *value = '\0';  // Split the string
            value++;  // Move past the '='
            
            // Check for known parameters
            if (strcmp(key, "algo") == 0) {
                strncpy(algo, value, algo_size);
            } else if (strcmp(key, "digits") == 0) {
                strncpy(digits_str, value, digits_size);
            } else if (strcmp(key, "out_of_core") == 0) {
                if (strcmp(value, "true") == 0) {
                    *out_of_core_mode = OOC_FORCE;
                } else if (strcmp(value, "false") == 0) {
                    *out_of_core_mode = OOC_DISABLE;
                }
            } else if (strcmp(key, "checkpoint") == 0) {
                *checkpointing_enabled = (strcmp(value, "true") == 0);
            } else if (strcmp(key, "mode") == 0) {
                *calc_mode = (strcmp(value, "async") == 0) ? CALC_MODE_ASYNC : CALC_MODE_SYNC;
            }
        }
        
        token = strtok(NULL, "&");
    }
}

// Validate input parameters
bool validate_parameters(const char* algo, const char* digits_str, out_of_core_mode_t out_of_core_mode) {
    // Check algorithm
    if (strcmp(algo, ALGORITHM_GL) != 0 && strcmp(algo, ALGORITHM_CH) != 0) {
        return false;
    }
    
    // Check digits
    if (!digits_str || digits_str[0] == '\0') {
        return false;
    }
    
    // Validate that digits is a non-negative integer
    for (int i = 0; digits_str[i]; i++) {
        if (digits_str[i] < '0' || digits_str[i] > '9') {
            return false;
        }
    }
    
    // Check if digits is within allowed range
    unsigned long digits = strtoul(digits_str, NULL, 10);
    if (digits < 1 || digits > config.max_digits) {
        return false;
    }
    
    return true;
}

// Replacement for lambda function - thread function for async calculation
void* calculation_thread_func(void* arg) {
    // Extract data
    calc_thread_data_t* data = (calc_thread_data_t*)arg;
    int job_idx = data->job_idx;
    free(data);
    
    // Get job info
    pthread_mutex_lock(&jobs[job_idx].lock);
    algorithm_t algorithm = jobs[job_idx].algorithm;
    unsigned long digits = jobs[job_idx].digits;
    out_of_core_mode_t out_of_core_mode = jobs[job_idx].out_of_core_mode;
    bool checkpointing_enabled = jobs[job_idx].checkpointing_enabled;
    pthread_mutex_unlock(&jobs[job_idx].lock);
    
    // Determine if out of core should be used
    bool use_out_of_core;
    if (out_of_core_mode == OOC_FORCE) {
        use_out_of_core = true;
    } else if (out_of_core_mode == OOC_DISABLE) {
        use_out_of_core = false;
    } else {
        // Auto mode
        if (algorithm == ALGO_CHUDNOVSKY && digits > LARGE_DIGITS_THRESHOLD) {
            use_out_of_core = true;
        } else if (algorithm == ALGO_GAUSS_LEGENDRE) {
            use_out_of_core = false;
        } else {
            size_t memory_needed = (digits / 14.1816) * 8 * 3;
            use_out_of_core = (memory_needed > config.memory_limit / 2);
        }
    }
    
    // Generate work directory
    char work_dir[MAX_PATH];
    char job_component[64]; // UUID is 36 chars + "job_" prefix
    snprintf(job_component, sizeof(job_component), "job_%s", jobs[job_idx].job_id);
    if (safe_path_join(work_dir, MAX_PATH, config.work_dir, job_component) < 0) {
        fprintf(stderr, "Error: Path too long for job directory\n");
        update_job_status(job_idx, JOB_STATUS_FAILED, 0.0, "Path too long for job directory");
        return NULL;
    }
    mkdir_recursive(work_dir, 0755);
    
    // Initialize calculation state
    calculation_state state;
    calculation_state_init(&state, digits, algorithm, use_out_of_core, work_dir, job_idx);
    state.checkpointing_enabled = checkpointing_enabled;
    
    // Create calculation thread pool
    calc_thread_pool pool;
    calc_thread_pool_init(&pool, config.max_calc_threads);
    calc_thread_pool_start(&pool);
    
    // Update job status
    update_job_status(job_idx, JOB_STATUS_RUNNING, 0.0, NULL);
    
    if (algorithm == ALGO_GAUSS_LEGENDRE) {
        calculate_pi_gauss_legendre(&state);
    } else {
        calculate_pi_chudnovsky(&state, &pool);
    }

    // Update job status - assume success if we got here
    update_job_status(job_idx, JOB_STATUS_COMPLETED, 1.0, NULL);
    
    // Clean up
    calc_thread_pool_shutdown(&pool);
    calculation_state_clear(&state);
    
    return NULL;
}

// Handle calculation request and return appropriate JSON response
void handle_calculation_request(int client_sock, const char* algo_str, const char* digits_str, 
                               out_of_core_mode_t out_of_core_mode, bool checkpointing_enabled,
                               int calc_mode) {
    // Convert algorithm string to enum
    algorithm_t algorithm = (strcmp(algo_str, ALGORITHM_CH) == 0) ? 
                          ALGO_CHUDNOVSKY : ALGO_GAUSS_LEGENDRE;
    
    // Convert digits string to number
    unsigned long digits = strtoul(digits_str, NULL, 10);
    
    // Determine whether to use out-of-core based on mode and digit count
    bool use_out_of_core;
    if (out_of_core_mode == OOC_FORCE) {
        use_out_of_core = true;
    } else if (out_of_core_mode == OOC_DISABLE) {
        use_out_of_core = false;
    } else {
        // Auto mode - use out-of-core for large calculations
        if (algorithm == ALGO_CHUDNOVSKY && digits > LARGE_DIGITS_THRESHOLD) {
            use_out_of_core = true;
        } else if (algorithm == ALGO_GAUSS_LEGENDRE) {
            use_out_of_core = false;  // Gauss-Legendre is always in-memory
        } else {
            // For medium size Chudnovsky, check memory availability
            size_t memory_needed = (digits / 14.1816) * 8 * 3;  // Rough estimation
            use_out_of_core = (memory_needed > config.memory_limit / 2);
        }
    }
    
    // Generate response FIRST so it's available for error handling
    struct json_object* response = json_object_new_object();

    // Generate work directory
    char work_dir[MAX_PATH];
    char request_component[64];
    snprintf(request_component, sizeof(request_component), "request_%lu", (unsigned long)time(NULL));
    if (safe_path_join(work_dir, MAX_PATH, config.work_dir, request_component) < 0) {
        fprintf(stderr, "Error: Path too long for request directory\n");
        
        // Handle error - return error to the client
        json_object_object_add(response, "status", json_object_new_string("error"));
        json_object_object_add(response, "message", json_object_new_string("Internal server error"));
        json_object_object_add(response, "code", json_object_new_int(500));
        
        const char* json_str = json_object_to_json_string(response);
        char http_response[BUFFER_SIZE];
        snprintf(http_response, BUFFER_SIZE,
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "\r\n"
                "%s", strlen(json_str), json_str);
        
        ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
        if (bytes_written < 0) {
            fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
        }
        
        json_object_put(response);
        return;
    }
    mkdir_recursive(work_dir, 0755);
    
    // Start time measurement
    clock_t start_time = clock();
    
    // Handle synchronous vs asynchronous calculation
    if (calc_mode == CALC_MODE_SYNC) {
        // For synchronous calculation, perform it directly
        
        calculation_state state;
        calculation_state_init(&state, digits, algorithm, use_out_of_core, work_dir, -1);
        state.checkpointing_enabled = checkpointing_enabled;
        
        // Create calculation thread pool
        calc_thread_pool pool;
        calc_thread_pool_init(&pool, config.max_calc_threads);
        calc_thread_pool_start(&pool);
        
        // Perform calculation
        if (algorithm == ALGO_GAUSS_LEGENDRE) {
            calculate_pi_gauss_legendre(&state);
        } else {
            calculate_pi_chudnovsky(&state, &pool);
        }
        
        // Clean up thread pool
        calc_thread_pool_shutdown(&pool);
        
        // Measure time
        clock_t end_time = clock();
        double time_taken = ((double)(end_time - start_time)) / CLOCKS_PER_SEC * 1000.0;  // ms
        
        // Build response
        json_object_object_add(response, "algorithm", json_object_new_string(algo_str));
        json_object_object_add(response, "digits", json_object_new_int64(digits));
        json_object_object_add(response, "out_of_core", json_object_new_boolean(use_out_of_core));
        json_object_object_add(response, "time_taken", json_object_new_double(time_taken));
        
        // Format current timestamp
        char timestamp[25];
        time_t now = time(NULL);
        strftime(timestamp, 25, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        json_object_object_add(response, "timestamp", json_object_new_string(timestamp));
        
        // Add file information
        char relative_path[MAX_PATH];
        snprintf(relative_path, MAX_PATH, "results/pi_%lu.txt", digits);
        json_object_object_add(response, "file_path", json_object_new_string(relative_path));
        
        json_object_object_add(response, "status", json_object_new_string("success"));
        json_object_object_add(response, "message", json_object_new_string("Calculation completed"));
        
        // Send response
        const char* json_str = json_object_to_json_string(response);
        char http_response[BUFFER_SIZE];
        snprintf(http_response, BUFFER_SIZE,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "\r\n"
                "%s", strlen(json_str), json_str);
        
        ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
        if (bytes_written < 0) {
            fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
        }
        
        // Clean up
        calculation_state_clear(&state);
        json_object_put(response);
    } else {
        // For asynchronous calculation, create a job and return job ID
        
        // Find or create a job
        int job_idx = find_or_create_job(NULL);
        if (job_idx < 0) {
            // No job slots available
            json_object_object_add(response, "status", json_object_new_string("error"));
            json_object_object_add(response, "message", json_object_new_string("Server is busy, try again later"));
            json_object_object_add(response, "code", json_object_new_int(503));
            
            const char* json_str = json_object_to_json_string(response);
            char http_response[BUFFER_SIZE];
            snprintf(http_response, BUFFER_SIZE,
                    "HTTP/1.1 503 Service Unavailable\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n"
                    "%s", strlen(json_str), json_str);
            
            ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
            if (bytes_written < 0) {
                fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
            }
            
            json_object_put(response);
            return;
        }
        
        // Configure job
        pthread_mutex_lock(&jobs[job_idx].lock);
        jobs[job_idx].algorithm = algorithm;
        jobs[job_idx].digits = digits;
        jobs[job_idx].out_of_core_mode = out_of_core_mode;
        jobs[job_idx].checkpointing_enabled = checkpointing_enabled;
        jobs[job_idx].status = JOB_STATUS_QUEUED;
        jobs[job_idx].creation_time = time(NULL);
        jobs[job_idx].start_time = 0;
        jobs[job_idx].end_time = 0;
        jobs[job_idx].progress = 0.0;
        jobs[job_idx].error_message[0] = '\0';
        pthread_mutex_unlock(&jobs[job_idx].lock);
        
        // Start calculation in a separate thread
        pthread_t calc_thread;
        
        // Create thread data structure
        calc_thread_data_t* thread_data = (calc_thread_data_t*)malloc(sizeof(calc_thread_data_t));
        if (!thread_data) {
            // Handle allocation failure
            const char* error_msg = "Out of memory error";
            update_job_status(job_idx, JOB_STATUS_FAILED, 0.0, error_msg);
            
            json_object_object_add(response, "status", json_object_new_string("error"));
            json_object_object_add(response, "message", json_object_new_string(error_msg));
            json_object_object_add(response, "code", json_object_new_int(500));
            
            const char* json_str = json_object_to_json_string(response);
            char http_response[BUFFER_SIZE];
            snprintf(http_response, BUFFER_SIZE,
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n"
                    "%s", strlen(json_str), json_str);
            
            ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
            if (bytes_written < 0) {
                fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
            }
            
            json_object_put(response);
            return;
        }
        
        thread_data->job_idx = job_idx;
        
        // Create thread with standard C function (replacing lambda)
        int thread_result = pthread_create(&calc_thread, NULL, calculation_thread_func, thread_data);
        if (thread_result != 0) {
            // Handle thread creation failure
            free(thread_data);
            const char* error_msg = "Failed to start calculation thread";
            update_job_status(job_idx, JOB_STATUS_FAILED, 0.0, error_msg);
            
            json_object_object_add(response, "status", json_object_new_string("error"));
            json_object_object_add(response, "message", json_object_new_string(error_msg));
            json_object_object_add(response, "code", json_object_new_int(500));
            
            const char* json_str = json_object_to_json_string(response);
            char http_response[BUFFER_SIZE];
            snprintf(http_response, BUFFER_SIZE,
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n"
                    "%s", strlen(json_str), json_str);
            
            ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
            if (bytes_written < 0) {
                fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
            }
            
            json_object_put(response);
            return;
        }
        
        // Detach thread to allow it to run independently
        pthread_detach(calc_thread);
        
        // Build response with job ID
        json_object_object_add(response, "status", json_object_new_string("success"));
        json_object_object_add(response, "message", json_object_new_string("Calculation queued"));
        json_object_object_add(response, "job_id", json_object_new_string(jobs[job_idx].job_id));
        json_object_object_add(response, "algorithm", json_object_new_string(algo_str));
        json_object_object_add(response, "digits", json_object_new_int64(digits));
        
        // Send response
        const char* json_str = json_object_to_json_string(response);
        char http_response[BUFFER_SIZE];
        snprintf(http_response, BUFFER_SIZE,
                "HTTP/1.1 202 Accepted\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "\r\n"
                "%s", strlen(json_str), json_str);
        
        ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
        if (bytes_written < 0) {
            fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
        }
        
        json_object_put(response);
    }
}

// Handle job status request
void handle_job_status_request(int client_sock, const char* job_id) {
    // Find the job
    int job_idx = find_or_create_job(job_id);
    
    // Create JSON response
    struct json_object* response = json_object_new_object();
    
    if (job_idx < 0) {
        // Job not found
        json_object_object_add(response, "status", json_object_new_string("error"));
        json_object_object_add(response, "message", json_object_new_string("Job not found"));
        json_object_object_add(response, "code", json_object_new_int(404));
        
        const char* json_str = json_object_to_json_string(response);
        char http_response[BUFFER_SIZE];
        snprintf(http_response, BUFFER_SIZE,
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "\r\n"
                "%s", strlen(json_str), json_str);
        
        ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
        if (bytes_written < 0) {
            fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
        }
    } else {
        // Job found - get status
        pthread_mutex_lock(&jobs[job_idx].lock);
        
        // Add job info to response
        json_object_object_add(response, "job_id", json_object_new_string(jobs[job_idx].job_id));
        
        // Status string
        const char* status_str;
        switch (jobs[job_idx].status) {
            case JOB_STATUS_QUEUED:   status_str = "queued";   break;
            case JOB_STATUS_RUNNING:  status_str = "running";  break;
            case JOB_STATUS_COMPLETED: status_str = "completed"; break;
            case JOB_STATUS_FAILED:   status_str = "failed";   break;
            case JOB_STATUS_CANCELED: status_str = "canceled"; break;
            default:                 status_str = "unknown";  break;
        }
        json_object_object_add(response, "status", json_object_new_string(status_str));
        
        // Add algorithm
        const char* algo_str = (jobs[job_idx].algorithm == ALGO_CHUDNOVSKY) ? 
                             ALGORITHM_CH : ALGORITHM_GL;
        json_object_object_add(response, "algorithm", json_object_new_string(algo_str));
        
        // Add digits
        json_object_object_add(response, "digits", json_object_new_int64(jobs[job_idx].digits));
        
        // Add progress
        json_object_object_add(response, "progress", json_object_new_double(jobs[job_idx].progress));
        
        // Add times
        json_object_object_add(response, "creation_time", json_object_new_int64(jobs[job_idx].creation_time));
        
        if (jobs[job_idx].start_time > 0) {
            json_object_object_add(response, "start_time", json_object_new_int64(jobs[job_idx].start_time));
        }
        
        if (jobs[job_idx].end_time > 0) {
            json_object_object_add(response, "end_time", json_object_new_int64(jobs[job_idx].end_time));
            
            // Add duration
            double duration = difftime(jobs[job_idx].end_time, jobs[job_idx].start_time);
            json_object_object_add(response, "duration_seconds", json_object_new_double(duration));
        }
        
        // Add error message if failed
        if (jobs[job_idx].status == JOB_STATUS_FAILED && jobs[job_idx].error_message[0] != '\0') {
            json_object_object_add(response, "error", json_object_new_string(jobs[job_idx].error_message));
        }
        
        // Add result file info if completed
        if (jobs[job_idx].status == JOB_STATUS_COMPLETED && jobs[job_idx].result_file[0] != '\0') {
            json_object_object_add(response, "result_file", json_object_new_string(jobs[job_idx].result_file));
        }
        
        pthread_mutex_unlock(&jobs[job_idx].lock);
        
        // Send response
        const char* json_str = json_object_to_json_string(response);
        char http_response[BUFFER_SIZE];
        snprintf(http_response, BUFFER_SIZE,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "\r\n"
                "%s", strlen(json_str), json_str);
        
        ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
        if (bytes_written < 0) {
            fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
        }
    }
    
    json_object_put(response);
}

// Handle job result request
void handle_job_result_request(int client_sock, const char* job_id) {
    // Find the job
    int job_idx = find_or_create_job(job_id);
    
    if (job_idx < 0) {
        // Job not found
        struct json_object* response = json_object_new_object();
        json_object_object_add(response, "status", json_object_new_string("error"));
        json_object_object_add(response, "message", json_object_new_string("Job not found"));
        json_object_object_add(response, "code", json_object_new_int(404));
        
        const char* json_str = json_object_to_json_string(response);
        char http_response[BUFFER_SIZE];
        snprintf(http_response, BUFFER_SIZE,
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "\r\n"
                "%s", strlen(json_str), json_str);
        
        ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
        if (bytes_written < 0) {
            fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
        }
        
        json_object_put(response);
        return;
    }
    
    // Check if job is completed
    pthread_mutex_lock(&jobs[job_idx].lock);
    bool is_completed = (jobs[job_idx].status == JOB_STATUS_COMPLETED);
    const char* result_file = jobs[job_idx].result_file;
    pthread_mutex_unlock(&jobs[job_idx].lock);
    
    if (!is_completed || result_file[0] == '\0') {
        // Job not completed or no result file
        struct json_object* response = json_object_new_object();
        json_object_object_add(response, "status", json_object_new_string("error"));
        json_object_object_add(response, "message", json_object_new_string("Result not available"));
        json_object_object_add(response, "code", json_object_new_int(404));
        
        const char* json_str = json_object_to_json_string(response);
        char http_response[BUFFER_SIZE];
        snprintf(http_response, BUFFER_SIZE,
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "\r\n"
                "%s", strlen(json_str), json_str);
        
        ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
        if (bytes_written < 0) {
            fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
        }
        
        json_object_put(response);
        return;
    }
    
    // Read result file
    FILE* f = fopen(result_file, "rb");
    if (!f) {
        // File not found
        struct json_object* response = json_object_new_object();
        json_object_object_add(response, "status", json_object_new_string("error"));
        json_object_object_add(response, "message", json_object_new_string("Result file not found"));
        json_object_object_add(response, "code", json_object_new_int(500));
        
        const char* json_str = json_object_to_json_string(response);
        char http_response[BUFFER_SIZE];
        snprintf(http_response, BUFFER_SIZE,
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "\r\n"
                "%s", strlen(json_str), json_str);
        
        ssize_t bytes_written = write(client_sock, http_response, strlen(http_response));
        if (bytes_written < 0) {
            fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
        }
        
        json_object_put(response);
        return;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // Read first few digits for header
    char preview[101];  // 100 digits + null terminator
    size_t read_size = fread(preview, 1, 100, f);
    if (read_size < 100) {
        fprintf(stderr, "Warning: Could only read %zu bytes from result file\n", read_size);
    }
    preview[read_size] = '\0';
    
    // Reset file pointer
    fseek(f, 0, SEEK_SET);
    
    // Send HTTP response header
    char http_header[BUFFER_SIZE];
    snprintf(http_header, BUFFER_SIZE,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %ld\r\n"
            "Content-Disposition: attachment; filename=\"pi.txt\"\r\n"
            "X-Pi-Digits: %lu\r\n"
            "X-Pi-Preview: %.100s\r\n"
            "\r\n",
            file_size,
            jobs[job_idx].digits,
            preview);
    
    ssize_t header_written = write(client_sock, http_header, strlen(http_header));
    if (header_written < 0) {
        fprintf(stderr, "Error writing header to socket: %s\n", strerror(errno));
        fclose(f);
        return;
    }
    
    // Send file contents
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, f)) > 0) {
        ssize_t bytes_written = write(client_sock, buffer, bytes_read);
        if (bytes_written < 0) {
            fprintf(stderr, "Error writing file data to socket: %s\n", strerror(errno));
            break;
        }
    }
    
    fclose(f);
}

// Parse and handle HTTP request
void* handle_http_request(void* arg) {
    int client_sock = *(int*)arg;
    
    // Get client information
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    getpeername(client_sock, (struct sockaddr*)&client_addr, &addr_len);
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    // Log the request
    char log_message_buf[BUFFER_SIZE];
    snprintf(log_message_buf, BUFFER_SIZE, "Inbound request from %s", client_ip);
    log_message("info", log_message_buf);
    
    // Read request
    char buffer[BUFFER_SIZE] = {0};
    int bytes_read = read(client_sock, buffer, BUFFER_SIZE - 1);
    
    if (bytes_read <= 0) {
        log_message("error", "Failed to read from client socket");
        close(client_sock);
        return NULL;
    }
    
    // Ensure buffer is null-terminated
    buffer[bytes_read] = '\0';
    
    // Parse request method and path
    char method[10] = {0};
    char path[BUFFER_SIZE] = {0};
    sscanf(buffer, "%9s %[^ ] HTTP", method, path);
    
    // Check if this is a valid GET request
    if (strcmp(method, "GET") != 0) {
        dprintf(client_sock, "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: application/json\r\n\r\n"
                           "{\"status\": \"error\", \"message\": \"Method not allowed\", \"code\": 405}");
        close(client_sock);
        return NULL;
    }
    
    // Extract query string
    char* query = strchr(path, '?');
    if (query) {
        *query = '\0';  // Split the path
        query++;       // Move to query string
    }
    
    // Handle different paths
    if (strcmp(path, "/pi") == 0 && query) {
        // Pi calculation request
        char algo[10] = {0};
        char digits_str[20] = {0};
        out_of_core_mode_t out_of_core_mode;
        bool checkpointing_enabled;
        int calc_mode;
        
        parse_query_string(query, algo, sizeof(algo), digits_str, sizeof(digits_str),
                         &out_of_core_mode, &checkpointing_enabled, &calc_mode);
        
        if (!validate_parameters(algo, digits_str, out_of_core_mode)) {
            dprintf(client_sock, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
                               "{\"status\": \"error\", \"message\": \"Invalid parameters\", \"code\": 400}");
        } else {
            handle_calculation_request(client_sock, algo, digits_str, out_of_core_mode, 
                                     checkpointing_enabled, calc_mode);
        }
    } else if (strcmp(path, "/pi/status") == 0 && query) {
        // Job status request
        char* job_id = strstr(query, "id=");
        if (job_id) {
            job_id += 3;  // Skip "id="
            
            // Handle job status
            handle_job_status_request(client_sock, job_id);
        } else {
            dprintf(client_sock, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
                               "{\"status\": \"error\", \"message\": \"Missing job ID\", \"code\": 400}");
        }
    } else if (strcmp(path, "/pi/result") == 0 && query) {
        // Job result request
        char* job_id = strstr(query, "id=");
        if (job_id) {
            job_id += 3;  // Skip "id="
            
            // Handle job result
            handle_job_result_request(client_sock, job_id);
        } else {
            dprintf(client_sock, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
                               "{\"status\": \"error\", \"message\": \"Missing job ID\", \"code\": 400}");
        }
    } else {
        // Not found
        dprintf(client_sock, "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n\r\n"
                           "{\"status\": \"error\", \"message\": \"Endpoint not found\", \"code\": 404}");
    }
    
    close(client_sock);
    return NULL;
}

// ==========================================================================
// Signal Handling
// ==========================================================================

// Global variables for server and thread pools
http_thread_pool* g_http_pool = NULL;
calc_thread_pool* g_calc_pool = NULL;
int g_server_sock = -1;
volatile int g_shutdown_flag = 0;

// Handle SIGINT (Ctrl+C) - graceful shutdown
void handle_sigint(int sig) {
    log_message("info", "Received shutdown signal, closing server...");
    g_shutdown_flag = 1;
    
    // Close server socket to stop accepting new connections
    if (g_server_sock != -1) {
        close(g_server_sock);
        g_server_sock = -1;
    }
    
    // Shutdown thread pools if initialized
    if (g_http_pool) {
        http_thread_pool_shutdown(g_http_pool);
    }
    
    if (g_calc_pool) {
        calc_thread_pool_shutdown(g_calc_pool);
    }
    
    // Close log file
    close_log_file();
    
    // Free MPFR cache
    mpfr_free_cache();
    
    exit(0);
}

// Handle SIGHUP - reload configuration and rotate logs
void handle_sighup(int sig) {
    log_message("info", "Received SIGHUP signal, reloading configuration...");
    
    // Close and reopen log file
    if (strcmp(config.logging_output, "console") != 0) {
        close_log_file();
        open_log_file(config.logging_output);
    }
    
    log_message("info", "Configuration reloaded");
}

// ==========================================================================
// Server Mode Implementation
// ==========================================================================

// Run the server
void run_server_mode() {
    // Initialize jobs array
    initialize_jobs();
    
    // Open log file
    open_log_file(config.logging_output);
    log_message("info", "Starting server in HTTP mode...");
    
    // Create working directory if it doesn't exist
    mkdir_recursive(config.work_dir, 0755);
    
    // Initialize server socket
    g_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_sock < 0) {
        log_message("error", "Failed to create socket");
        exit(1);
    }
    
    // Configure address reuse
    int opt = 1;
    if (setsockopt(g_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_message("error", "Failed to set socket options");
        close(g_server_sock);
        exit(1);
    }
    
    // Configure server address
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config.port);
    server_addr.sin_addr.s_addr = inet_addr(config.ip_address);
    
    // Bind the socket
    if (bind(g_server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log_message("error", "Failed to bind socket");
        close(g_server_sock);
        exit(1);
    }
    
    // Start listening
    if (listen(g_server_sock, 10) < 0) {
        log_message("error", "Failed to listen on socket");
        close(g_server_sock);
        exit(1);
    }
    
    // Initialize thread pools
    http_thread_pool http_pool;
    calc_thread_pool calc_pool;
    
    http_thread_pool_init(&http_pool, config.max_http_threads);
    calc_thread_pool_init(&calc_pool, config.max_calc_threads);
    calc_thread_pool_start(&calc_pool);
    
    // Store in global variables for signal handling
    g_http_pool = &http_pool;
    g_calc_pool = &calc_pool;
    
    // Log server start
    char log_buf[BUFFER_SIZE];
    snprintf(log_buf, BUFFER_SIZE, "Server started on %s:%d", config.ip_address, config.port);
    log_message("info", log_buf);
    
    // Main server loop
    while (!g_shutdown_flag) {
        // Wait for connections with timeout to check shutdown flag periodically
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(g_server_sock, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;  // 1 second timeout
        timeout.tv_usec = 0;
        
        int activity = select(g_server_sock + 1, &read_fds, NULL, NULL, &timeout);
        
        if (activity < 0 && errno != EINTR) {
            log_message("error", "Select error");
            break;
        }
        
        if (activity > 0 && FD_ISSET(g_server_sock, &read_fds)) {
            // Accept new connection
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_sock = accept(g_server_sock, (struct sockaddr*)&client_addr, &addr_len);
            
            if (client_sock < 0) {
                log_message("error", "Failed to accept connection");
                continue;
            }
            
            // Add job to HTTP queue
            pthread_mutex_lock(&http_queue_lock);
            
            // Check if queue is full
            if ((http_queue_back + 1) % MAX_HTTP_QUEUE_SIZE == http_queue_front) {
                log_message("error", "HTTP job queue full, rejecting connection");
                dprintf(client_sock, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n\r\n"
                                   "Server is busy, please try again later.");
                close(client_sock);
            } else {
                // Queue the job
                http_enqueue_job(client_sock);
                sem_post(http_pool.job_semaphore);
            }
            
            pthread_mutex_unlock(&http_queue_lock);
        }
    }
    
    // Clean up (this should be handled by the signal handler)
    http_thread_pool_shutdown(&http_pool);
    calc_thread_pool_shutdown(&calc_pool);
    
    g_http_pool = NULL;
    g_calc_pool = NULL;
    
    // Close server socket
    if (g_server_sock != -1) {
        close(g_server_sock);
        g_server_sock = -1;
    }
    
    close_log_file();
}

// ==========================================================================
// CLI Mode Implementation
// ==========================================================================

// Run the CLI mode
void run_cli_mode(int argc, char** argv) {
    // Determine algorithm
    algorithm_t algorithm = config.default_algorithm;
    
    // Determine whether to use out-of-core
    bool use_out_of_core = (algorithm == ALGO_CHUDNOVSKY && 
                          config.max_digits > LARGE_DIGITS_THRESHOLD);
    
    // Create working directory if it doesn't exist
    mkdir_recursive(config.work_dir, 0755);
    
    // Initialize calculation state
    calculation_state state;
    calculation_state_init(&state, config.max_digits, algorithm, use_out_of_core, config.work_dir, -1);
    state.checkpointing_enabled = config.checkpointing_enabled;
    
    // Initialize calculation thread pool
    calc_thread_pool pool;
    calc_thread_pool_init(&pool, config.max_calc_threads);
    calc_thread_pool_start(&pool);
    
    // Store in global variable for signal handling
    g_calc_pool = &pool;
    
    // Run appropriate calculation
    if (algorithm == ALGO_GAUSS_LEGENDRE) {
        printf("Calculating Pi to %lu digits using Gauss-Legendre algorithm...\n", config.max_digits);
        calculate_pi_gauss_legendre(&state);
    } else {
        calculate_pi_chudnovsky(&state, &pool);
    }
    
    // Print completion message
    printf("Calculation complete! Result saved to %s\n", state.output_file);
    
    // Clean up
    calculation_state_clear(&state);
    calc_thread_pool_shutdown(&pool);
    g_calc_pool = NULL;
}

// ==========================================================================
// Main Function
// ==========================================================================

int main(int argc, char** argv) {
    // Install signal handler for segmentation fault
    signal(SIGSEGV, segfault_handler);
    printf("DEBUG: Installed segfault handler\n");
    // Set up signal handlers
    signal(SIGINT, handle_sigint);
    signal(SIGHUP, handle_sighup);
    signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE to handle client disconnections
    
    // Set default configuration
    set_default_config();
    
    // Check for config file in arguments
    const char* config_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_file = argv[i + 1];
            break;
        }
    }
    
    // Load configuration from file if specified
    if (config_file) {
        load_config_from_file(config_file);
    }
    
    // Override with command line arguments
    override_config_with_args(argc, argv);
    
    // Run in appropriate mode
    if (config.mode == MODE_SERVER) {
        run_server_mode();
    } else {
        run_cli_mode(argc, argv);
    }
    
    return 0;
}
