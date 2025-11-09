#ifndef DISK_INT_H
#define DISK_INT_H

// ==========================================================================
// Disk-Based Integer Module - Header File
// ==========================================================================
// This module implements arbitrary-precision integers stored on disk with
// chunking support for out-of-core arithmetic operations.

#include "common.h"
#include "utils.h"

// ==========================================================================
// Constants
// ==========================================================================

// Chunk size limits
#define MIN_CHUNK_SIZE (1024*1024)         // Minimum chunk size of 1MB
#define MAX_CHUNK_SIZE (512*1024*1024)     // Maximum chunk size of 512MB

// Define MIN and MAX macros if not already defined
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// ==========================================================================
// Structure Definitions
// ==========================================================================

// Structure for disk-based large integers with chunking support
struct disk_int {
    char file_path[MAX_PATH];   // Base path for the chunks
    size_t total_size_in_limbs; // Total size across all chunks
    int sign;                   // Sign of the number (1, 0, -1)
    size_t chunk_size;          // Size of each chunk in limbs
    size_t num_chunks;          // Number of chunks
    void* cache;                // Memory cache for active chunk
    size_t cache_chunk_idx;     // Index of cached chunk
    bool dirty;                 // Whether cache needs writing back to disk
    pthread_mutex_t lock;       // Mutex for thread safety
};

// ==========================================================================
// Function Declarations
// ==========================================================================

// Chunk management
int get_chunk_path(char* dest, size_t dest_size, const char* base_path, size_t chunk_idx);
size_t determine_optimal_chunk_size(size_t available_memory, size_t total_size);
int load_chunk(disk_int* d_int, size_t chunk_idx);
int save_chunk(disk_int* d_int, size_t chunk_idx);

// Disk integer lifecycle
void disk_int_init(disk_int* d_int, const char* base_path);
void disk_int_clear(disk_int* d_int);

// Conversion to/from GMP mpz_t
void disk_int_set_mpz(disk_int* d_int, mpz_t mpz_val);
void disk_int_get_mpz(mpz_t mpz_val, disk_int* d_int);

// Comparison
int disk_int_cmp_abs(disk_int* a, disk_int* b);

// Arithmetic operations
void disk_int_add(disk_int* result, disk_int* a, disk_int* b);
void disk_int_sub_abs(disk_int* result, disk_int* a, disk_int* b);
void disk_int_mul(disk_int* result, disk_int* a, disk_int* b);

// Helper functions for multiplication
void add_product_to_result(disk_int* result, mp_limb_t* product, size_t product_size, size_t position);

// Global lock for disk_int initialization
extern pthread_mutex_t g_disk_int_init_lock;

#endif // DISK_INT_H
