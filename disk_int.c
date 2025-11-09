#include "common.h"
#include "disk_int.h"
#include "utils.h"
#include "logging.h"

// ==========================================================================
// Disk-Based Integer Implementation
// ==========================================================================

// Add a global mutex for disk_int_init static variables  
pthread_mutex_t g_disk_int_init_lock = PTHREAD_MUTEX_INITIALIZER;

// Get chunk file path from base path and chunk index
int get_chunk_path(char *dest, size_t dest_size, const char *base_path, size_t chunk_idx) {
    // Make sure we have a valid base path
    if (!base_path || base_path[0] == '\0') {
        fprintf(stderr, "Error: Invalid base path in get_chunk_path\n");
        return -1;
    }
    
    // Create a consistent naming scheme for all chunk files
    char component[32];
    snprintf(component, sizeof(component), "chunk_%zu.bin", chunk_idx);
    
    // Create the parent directory for the chunk if it doesn't exist
    mkdir_recursive(base_path, 0755);
    
    // Combine base path with the chunk file name
    int result = safe_path_join(dest, dest_size, base_path, component);
    printf("DEBUG: get_chunk_path('%s', %zu) = '%s'\n", base_path, chunk_idx, dest);
    return result;
}

// Determine optimal chunk size based on available memory and total size
size_t determine_optimal_chunk_size(size_t available_memory, size_t total_size) {
    // Default to 1MB chunks or 10% of available memory, whichever is smaller
    size_t optimal_size = MIN(1024*1024, available_memory / 10);
    
    // Don't make chunks too small relative to the total size
    if (total_size < 100 * optimal_size) {
        optimal_size = MAX(total_size / 100, MIN_CHUNK_SIZE);
    }
    
    // Make sure we're not exceeding maximum chunk size
    if (optimal_size > MAX_CHUNK_SIZE) {
        optimal_size = MAX_CHUNK_SIZE;
    }
    
    return optimal_size;
}

int load_chunk(disk_int* d_int, size_t chunk_idx) {
    if (chunk_idx >= d_int->num_chunks) {
        return -1; // Invalid chunk index
    }
    
    // If the current cached chunk is dirty, save it first
    if (d_int->dirty && d_int->cache) {
        if (save_chunk(d_int, d_int->cache_chunk_idx) != 0) {
            return -1;
        }
    }
    
    // Determine actual chunk size - the last chunk might be smaller
    size_t chunk_size = d_int->chunk_size;
    if (chunk_idx == d_int->num_chunks - 1) {
        size_t last_chunk_size = d_int->total_size_in_limbs % d_int->chunk_size;
        if (last_chunk_size > 0) {
            chunk_size = last_chunk_size;
        }
    }
    
    // Allocate cache if not already allocated
    if (!d_int->cache) {
        d_int->cache = malloc(d_int->chunk_size * sizeof(mp_limb_t));
        if (!d_int->cache) {
            return -1; // Memory allocation failed
        }
    } else if (d_int->cache_chunk_idx == chunk_idx) {
        return 0; // Chunk already loaded
    }
    
    // Construct chunk file path
    char chunk_path[MAX_PATH];
    if (get_chunk_path(chunk_path, MAX_PATH, d_int->file_path, chunk_idx) < 0) {
        return -1;
    }
    
    // Open the chunk file
    FILE* f = fopen(chunk_path, "rb");
    if (!f) {
        // If the file doesn't exist, report an error instead of silently returning zeros
        fprintf(stderr, "ERROR: load_chunk failed to open file '%s' for reading: %s\n", 
                chunk_path, strerror(errno));
        return -1; // Return error instead of silently returning zeros
    }
    
    // Read chunk data
    size_t read_items = fread(d_int->cache, sizeof(mp_limb_t), chunk_size, f);
    
    // Check for read errors before closing the file
    int ferror_result = ferror(f);
    fclose(f);
    
    // Complete read failure - likely indicates corruption
    if (read_items == 0 && chunk_size > 0) {
        fprintf(stderr, "ERROR: Complete read failure for chunk %s (expected %zu limbs, got 0)\n", 
                chunk_path, chunk_size);
        return -1; // Report error instead of silently corrupting data
    }
    
    // Error during read
    if (ferror_result) {
        fprintf(stderr, "ERROR: I/O error reading chunk %s (read %zu/%zu limbs)\n", 
                chunk_path, read_items, chunk_size);
        return -1; // Report error instead of silently corrupting data
    }
    
    // Partial read (can happen at EOF)
    if (read_items < chunk_size) {
        fprintf(stderr, "WARNING: Partial read for chunk %s (read %zu/%zu limbs). Padding with zeros.\n",
                chunk_path, read_items, chunk_size);
        // Fill the rest with zeros for partial reads
        memset((char*)d_int->cache + read_items * sizeof(mp_limb_t), 
               0, (chunk_size - read_items) * sizeof(mp_limb_t));
    }
    
    d_int->cache_chunk_idx = chunk_idx;
    d_int->dirty = false;
    
    return 0;
}

// Save a specific chunk to disk with improved error handling
int save_chunk(disk_int* d_int, size_t chunk_idx) {
    if (!d_int) {
        fprintf(stderr, "Error: Null d_int in save_chunk\n");
        return -1;
    }
    
    if (d_int->file_path[0] == '\0') {
        fprintf(stderr, "Error: Uninitialized disk integer in save_chunk\n");
        return -1;
    }
    
    if (chunk_idx >= d_int->num_chunks) {
        fprintf(stderr, "Error: Invalid chunk index %zu (max is %zu)\n", 
                chunk_idx, d_int->num_chunks > 0 ? d_int->num_chunks - 1 : 0);
        return -1; // Invalid chunk index
    }
    
    // If this chunk is not currently cached, we can't save it
    if (!d_int->cache || d_int->cache_chunk_idx != chunk_idx) {
        fprintf(stderr, "Error: Cannot save uncached chunk %zu (cache chunk is %zu)\n", 
                chunk_idx, d_int->cache_chunk_idx);
        return -1;
    }
    
    // If not dirty, no need to save
    if (!d_int->dirty) {
        return 0;
    }
    
    // Determine actual chunk size - the last chunk might be smaller
    size_t chunk_size = d_int->chunk_size;
    if (chunk_idx == d_int->num_chunks - 1) {
        size_t last_chunk_size = d_int->total_size_in_limbs % d_int->chunk_size;
        if (last_chunk_size > 0) {
            chunk_size = last_chunk_size;
        }
    }
    
    printf("DEBUG: save_chunk for disk_int at path: %s, chunk_idx: %zu, chunk_size: %zu\n", 
           d_int->file_path, chunk_idx, chunk_size);
    
    // Construct chunk file path
    char chunk_path[MAX_PATH];
    if (get_chunk_path(chunk_path, MAX_PATH, d_int->file_path, chunk_idx) < 0) {
        fprintf(stderr, "Error: Failed to construct path for chunk %zu from base %s\n", 
                chunk_idx, d_int->file_path);
        return -1;
    }
    
    printf("DEBUG: Saving chunk to: %s\n", chunk_path);
    
    // Ensure directory exists
    char dir_path[MAX_PATH];
    char* last_slash = strrchr(chunk_path, '/');
    if (last_slash) {
        // Cast to ptrdiff_t first for proper pointer arithmetic, then to size_t
        // after verifying it's positive
        ptrdiff_t diff = last_slash - chunk_path;
        if (diff < 0) {
            log_message("error", "Invalid path structure detected");
            return -1;
        }
        size_t dir_len = (size_t)diff;
        
        // TODO: Replace with strlcpy or similar safer function if available
        // strncpy doesn't guarantee null-termination if source is >= dir_len
        if (dir_len >= MAX_PATH) {
            log_message("error", "Directory path too long");
            return -1;
        }
        strncpy(dir_path, chunk_path, dir_len);
        dir_path[dir_len] = '\0';
        
        printf("DEBUG: Creating directory: %s\n", dir_path);
        if (mkdir_recursive(dir_path, 0755) != 0) {
            fprintf(stderr, "Error creating directory structure for: %s (errno: %d - %s)\n", 
                    dir_path, errno, strerror(errno));
            return -1;
        }
    }
    
    // Open chunk file for writing with proper error checking
    FILE* f = fopen(chunk_path, "wb");
    if (!f) {
        fprintf(stderr, "Error opening chunk file for writing: %s (errno: %d - %s)\n", 
                chunk_path, errno, strerror(errno));
        return -1;
    }
    
    // Write chunk data - make sure we're writing something valid
    if (chunk_size == 0 || !d_int->cache) {
        fprintf(stderr, "Warning: Attempt to write empty chunk (size=%zu, cache=%p)\n",
                chunk_size, (void*)d_int->cache);
        // Write at least one byte to create a non-empty file
        fputc(0, f);
    } else {
        size_t written = fwrite(d_int->cache, sizeof(mp_limb_t), chunk_size, f);
        if (written != chunk_size) {
            fprintf(stderr, "Error writing chunk data: wrote %zu of %zu items (errno: %d - %s)\n", 
                   written, chunk_size, errno, strerror(errno));
            fclose(f);
            return -1;
        }
    }
    
    // Flush and close file
    if (fflush(f) != 0) {
        log_message("error", "fflush failed for chunk file %s in save_chunk: %s", chunk_path, strerror(errno));
        // Attempt to close anyway, but record error
        fclose(f); // Best effort close
        return -1;
    }
    if (fclose(f) != 0) {
        log_message("error", "fclose failed for chunk file %s in save_chunk: %s", chunk_path, strerror(errno));
        return -1;
    }
    
    // Verify file was written correctly
    struct stat st;
    if (stat(chunk_path, &st) == 0) {
        if (st.st_size == 0) {
            fprintf(stderr, "Warning: Created empty chunk file: %s\n", chunk_path);
        } else {
            printf("DEBUG: Successfully wrote %lld bytes to %s\n", 
                   (long long)st.st_size, chunk_path);
        }
    } else {
        fprintf(stderr, "Warning: Could not verify chunk file: %s (errno: %d - %s)\n",
                chunk_path, errno, strerror(errno));
    }
    
    // Mark as clean
    d_int->dirty = false;
    return 0;
}

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

// Add a global mutex for disk_int_init static variables
static pthread_mutex_t g_disk_int_init_lock = PTHREAD_MUTEX_INITIALIZER;

// Initialize a disk integer
void disk_int_init(disk_int* d_int, const char* base_path) {
    printf("DEBUG: disk_int_init with base_path=%s\n", base_path ? base_path : "NULL");
    
    // Safety check
    if (!d_int) {
        fprintf(stderr, "Error: NULL disk_int pointer in disk_int_init\n");
        return;
    }
    
    // First, ensure the structure is zeroed out
    memset(d_int, 0, sizeof(disk_int));
    
    // Make sure base_path is valid
    if (!base_path || base_path[0] == '\0') {
        printf("DEBUG: Invalid base_path in disk_int_init\n");
        fprintf(stderr, "Error: Invalid base path for disk integer\n");
        d_int->file_path[0] = '\0';
        return;
    }
    
    // Create a unique directory name for this disk integer based on PID and timestamp
    static int counter = 0;
    static long timestamp = 0;
    int local_counter_val; // Use a local variable to hold the counter for snprintf
    long local_timestamp_val; // Use a local variable for the timestamp
    
    // Lock before accessing shared static variables
    pthread_mutex_lock(&g_disk_int_init_lock);
    if (timestamp == 0) {
        timestamp = time(NULL);
    }
    local_timestamp_val = timestamp; // Copy to local after ensuring initialization
    local_counter_val = counter++;   // Get current counter value then increment
    pthread_mutex_unlock(&g_disk_int_init_lock);
    
    // Create a unique path for this disk integer using local copies of values
    snprintf(d_int->file_path, MAX_PATH, "%s/int_%d_%ld_%d", 
             base_path, (int)getpid(), local_timestamp_val, local_counter_val);
    printf("DEBUG: Generated base path: %s\n", d_int->file_path);
    
    // Check if the base directory exists first
    struct stat st = {0};
    if (stat(base_path, &st) == -1) {
        // Base directory doesn't exist - try to create it
        if (mkdir_recursive(base_path, 0755) != 0) {
            fprintf(stderr, "Error: Failed to create base directory: %s (errno: %d - %s)\n", 
                   base_path, errno, strerror(errno));
            d_int->file_path[0] = '\0';
            return;
        }
    }
    
    // Now create the specific directory for this disk integer
    if (mkdir(d_int->file_path, 0755) != 0) {
        if (errno != EEXIST) {  // Only report error if it's not because directory already exists
            fprintf(stderr, "Error creating directory structure for: %s (errno: %d - %s)\n", 
                   d_int->file_path, errno, strerror(errno));
            d_int->file_path[0] = '\0';
            return;
        }
    }
    
    // Initialize mutex for thread safety
    if (pthread_mutex_init(&d_int->lock, NULL) != 0) {
        fprintf(stderr, "Error initializing mutex for disk integer\n");
        d_int->file_path[0] = '\0';
        return;
    }
    
    // Initialize the rest of the structure
    d_int->total_size_in_limbs = 0;
    d_int->sign = 0;
    d_int->chunk_size = determine_optimal_chunk_size(DEFAULT_MEMORY_LIMIT, 0);
    d_int->num_chunks = 0;
    d_int->cache = NULL;
    d_int->cache_chunk_idx = 0;
    d_int->dirty = false;
    
    // Debug output
    printf("DEBUG: Initialized chunked disk_int with optimal chunk size = %zu limbs\n", 
           d_int->chunk_size);
}

// Clear and free resources for a disk integer
void disk_int_clear(disk_int* d_int) {
    // Check if lock is already initialized
    if (!d_int || d_int->file_path[0] == '\0') {
        // Nothing to clean up - likely already cleared or not initialized
        return;
    }

    pthread_mutex_lock(&d_int->lock);
    
    // Write cache to disk if dirty
    if (d_int->dirty && d_int->cache) {
        save_chunk(d_int, d_int->cache_chunk_idx);
    }
    
    // Free cache memory
    if (d_int->cache) {
        free(d_int->cache);
        d_int->cache = NULL;
    }
    
    // Save the file path and number of chunks for cleanup after lock release
    char file_path_copy[MAX_PATH];
    strncpy(file_path_copy, d_int->file_path, MAX_PATH);
    
    // Store the actual number of chunks before clearing
    size_t num_chunks = d_int->num_chunks;
    
    // Mark as cleared by emptying the path
    d_int->file_path[0] = '\0';
    d_int->total_size_in_limbs = 0;
    d_int->sign = 0;
    d_int->chunk_size = 0;
    d_int->num_chunks = 0;
    d_int->cache = NULL;
    d_int->cache_chunk_idx = 0;
    d_int->dirty = false;
    
    pthread_mutex_unlock(&d_int->lock);
    pthread_mutex_destroy(&d_int->lock);
    
    // Delete all chunk files using the stored num_chunks value
    // This is more efficient and accurate than checking for file existence
    for (size_t i = 0; i < num_chunks; i++) {
        char chunk_path[MAX_PATH];
        if (get_chunk_path(chunk_path, MAX_PATH, file_path_copy, i) < 0) {
            fprintf(stderr, "Warning: Failed to construct path for chunk %zu\n", i);
            continue;
        }
        
        if (remove(chunk_path) != 0 && errno != ENOENT) {
            // Only print warning if error is not "file doesn't exist"
            fprintf(stderr, "Warning: Failed to delete chunk file: %s (error: %s)\n", 
                    chunk_path, strerror(errno));
        }
    }
    
    // Finally, remove the directory
    if (rmdir(file_path_copy) != 0 && errno != ENOENT) {
        fprintf(stderr, "Warning: Failed to delete directory: %s (error: %s)\n", 
                file_path_copy, strerror(errno));
    }
}

// Set a disk integer from a GMP mpz_t
void disk_int_set_mpz(disk_int* d_int, mpz_t mpz_val) {
    // Check if disk_int is properly initialized
    if (!d_int) {
        fprintf(stderr, "Error: NULL disk_int pointer in disk_int_set_mpz\n");
        return;
    }
    
    // Check if the file_path is empty - critically important validation
    if (d_int->file_path[0] == '\0') {
        fprintf(stderr, "CRITICAL ERROR: disk_int has empty file_path in disk_int_set_mpz\n");
        fprintf(stderr, "               This suggests it was previously cleared. Aborting.\n");
        // Print a stack trace if possible
        void* callstack[128];
        int frames = backtrace(callstack, 128);
        char** strs = backtrace_symbols(callstack, frames);
        for (int i = 0; i < frames; i++) {
            fprintf(stderr, "   [%d] %s\n", i, strs[i]);
        }
        free(strs);
        return;
    }
    
    pthread_mutex_lock(&d_int->lock);
    
    // Get size and sign
    size_t total_size = mpz_size(mpz_val);
    int sign = mpz_sgn(mpz_val);
    
    // Determine chunk size and number of chunks
    d_int->total_size_in_limbs = total_size;
    d_int->sign = sign;
    
    // If total size is 0, just clear everything
    if (total_size == 0) {
        d_int->num_chunks = 0;
        
        // Clear cache if exists
        if (d_int->cache) {
            free(d_int->cache);
            d_int->cache = NULL;
        }
        
        d_int->dirty = false;
        pthread_mutex_unlock(&d_int->lock);
        return;
    }
    
    // Calculate how many chunks we need
    size_t num_chunks = (total_size + d_int->chunk_size - 1) / d_int->chunk_size;
    d_int->num_chunks = num_chunks;
    
    printf("DEBUG: Setting mpz_t to disk_int with %zu chunks (total size: %zu limbs)\n", 
           num_chunks, total_size);
    
    // Free any existing cache
    if (d_int->cache) {
        free(d_int->cache);
        d_int->cache = NULL;
    }
    
    // Allocate cache for a single chunk
    d_int->cache = malloc(d_int->chunk_size * sizeof(mp_limb_t));
    if (!d_int->cache) {
        fprintf(stderr, "Error: Out of memory in disk_int_set_mpz\n");
        pthread_mutex_unlock(&d_int->lock);
        return;
    }
    
    // Write each chunk to disk
    for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
        // Determine the size of this chunk
        size_t chunk_start = chunk_idx * d_int->chunk_size;
        size_t chunk_size = d_int->chunk_size;
        
        // The last chunk might be smaller
        if (chunk_idx == num_chunks - 1) {
            size_t remaining = total_size - chunk_start;
            if (remaining < chunk_size) {
                chunk_size = remaining;
            }
        }
        
        // Extract the chunk from mpz_t using mpz_export to get all limbs at once
        // CRITICAL FIX: Use order=1 (most-significant first) so that the most
        // significant limbs are at the START of the array, making offset calculations simpler
        // and consistent with how we process the arrays in the rest of the code
        size_t count;
        mp_limb_t *all_limbs = mpz_export(NULL, &count, 1, sizeof(mp_limb_t), 0, 0, mpz_val);
        
        printf("DEBUG: disk_int_set_mpz to %s: mpz_export returned %zu limbs (total_size=%zu)\n", 
               d_int->file_path, count, total_size);
        
        if (!all_limbs) {
            // If mpz_export returns NULL, the number is zero
            memset(d_int->cache, 0, chunk_size * sizeof(mp_limb_t));
            printf("DEBUG: mpz_export returned NULL - setting all limbs to zero\n");
        } else {
            // For a non-zero number, use all the significant limbs
            
            // Always ensure we get at least one non-zero limb in the first chunk
            if (chunk_idx == 0 && count > 0) {
                // Clear the cache first
                memset(d_int->cache, 0, chunk_size * sizeof(mp_limb_t));
                
                // Copy at least the most significant limb to ensure we have a non-zero value
                // This is the critical fix: always include a non-zero limb in the first chunk
                size_t limbs_to_copy = (count < chunk_size) ? count : chunk_size;
                
                // FIXED: With order=1, the most significant limbs are at the start of all_limbs
                // For Chudnovsky values with huge factor 2^K, we want to ensure we get the
                // non-zero limbs which are now at the beginning of the array
                // Simply copy from the beginning of the array
                memcpy(d_int->cache, all_limbs, limbs_to_copy * sizeof(mp_limb_t));
                
                printf("DEBUG: Copied most significant %zu limbs (total %zu limbs)\n", 
                       limbs_to_copy, count);
                
                // Ensure we're writing non-zero data
                printf("DEBUG: First few limbs: ");
                for (size_t i = 0; i < 3 && i < chunk_size; i++) {
                    printf("%016lx ", (unsigned long)((mp_limb_t*)d_int->cache)[i]);
                }
                printf("\n");
            } else if (chunk_idx > 0 && chunk_idx < (count + d_int->chunk_size - 1) / d_int->chunk_size) {
                // For subsequent chunks that are within the range of the significant limbs
                memset(d_int->cache, 0, chunk_size * sizeof(mp_limb_t));
                
                // FIXED: For subsequent chunks, with order=1 the logic is much simpler
                // Since the most significant limbs are at the beginning, we just need to
                // calculate which slice of the array to copy based on chunk index
                
                // Simple offset calculation - each chunk gets its own slice
                size_t src_offset = chunk_idx * d_int->chunk_size;
                
                // Calculate how many limbs to copy (limit by available limbs)
                size_t limbs_to_copy = 0;
                if (src_offset < count) {
                    // This chunk starts within the available limbs
                    limbs_to_copy = (count - src_offset < chunk_size) ? 
                                   (count - src_offset) : chunk_size;
                }
                
                // Zero the cache first
                memset(d_int->cache, 0, chunk_size * sizeof(mp_limb_t));
                
                // Copy the data if there's anything to copy
                if (limbs_to_copy > 0) {
                    memcpy(d_int->cache, all_limbs + src_offset, limbs_to_copy * sizeof(mp_limb_t));
                }
                
                printf("DEBUG: Chunk %zu: copied %zu limbs from offset %zu (total %zu limbs)\n", 
                       chunk_idx, limbs_to_copy, src_offset, count);
            } else {
                // Zero out any chunks beyond the significant limbs
                memset(d_int->cache, 0, chunk_size * sizeof(mp_limb_t));
            }
            
            // Free the allocated memory
            free(all_limbs);
        }
        
        // We no longer need the band-aid fix since we properly handle leading zeros in mpz_import
        // This was previously forcing the first limb to be non-zero, but that doesn't 
        // fix the issue if we have many leading zeros (>200) in the number
        
        // Now we properly find the first non-zero limb when importing, allowing
        // large numbers with many leading zeros to be handled correctly
        
        // Set the cache chunk index and mark as dirty
        d_int->cache_chunk_idx = chunk_idx;
        d_int->dirty = true;
        
        // Save this chunk to disk
        if (save_chunk(d_int, chunk_idx) != 0) {
            fprintf(stderr, "Error: Failed to save chunk %zu to disk\n", chunk_idx);
            free(d_int->cache);
            d_int->cache = NULL;
            pthread_mutex_unlock(&d_int->lock);
            return;
        }
    }
    
    // Free cache as we don't need it anymore
    free(d_int->cache);
    d_int->cache = NULL;
    d_int->dirty = false;
    
    printf("DEBUG: Successfully set mpz_t to disk_int with %zu chunks\n", num_chunks);
    
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
        save_chunk(d_int, d_int->cache_chunk_idx);
    }
    
    // Check if there are any chunks
    if (d_int->num_chunks == 0 || d_int->total_size_in_limbs == 0) {
        mpz_set_ui(mpz_val, 0);
        pthread_mutex_unlock(&d_int->lock);
        return;
    }
    
    // Allocate temporary buffer for all limbs
    mp_limb_t* limbs = (mp_limb_t*)malloc(d_int->total_size_in_limbs * sizeof(mp_limb_t));
    if (!limbs) {
        fprintf(stderr, "Out of memory in disk_int_get_mpz\n");
        pthread_mutex_unlock(&d_int->lock);
        mpz_set_ui(mpz_val, 0); // Set to zero on error
        return;
    }
    
    // Load all chunks into the buffer
    size_t limbs_loaded = 0;
    for (size_t chunk_idx = 0; chunk_idx < d_int->num_chunks; chunk_idx++) {
        // Allocate cache if not already allocated
        if (!d_int->cache) {
            d_int->cache = malloc(d_int->chunk_size * sizeof(mp_limb_t));
            if (!d_int->cache) {
                fprintf(stderr, "Error: Memory allocation failed in disk_int_get_mpz\n");
                free(limbs);
                pthread_mutex_unlock(&d_int->lock);
                mpz_set_ui(mpz_val, 0); // Set to zero on error
                return;
            }
        }
        
        // Load this chunk
        int load_result = load_chunk(d_int, chunk_idx);
        if (load_result != 0) {
            fprintf(stderr, "ERROR: Failed to load chunk %zu in disk_int_get_mpz for %s (error code: %d)\n", 
                    chunk_idx, d_int->file_path, load_result);
            fprintf(stderr, "CRITICAL: This indicates possible data corruption and will likely affect calculation results!\n");
            
            // In production code we might consider retrying or more aggressive error handling here
            // For now, report error and set to zero, but make it very clear in the logs
            free(limbs);
            pthread_mutex_unlock(&d_int->lock);
            mpz_set_ui(mpz_val, 0); // Set to zero on error
            return;
        }
        
        // Determine chunk size - last chunk might be smaller
        size_t chunk_size = d_int->chunk_size;
        if (chunk_idx == d_int->num_chunks - 1) {
            size_t last_chunk_size = d_int->total_size_in_limbs % d_int->chunk_size;
            if (last_chunk_size > 0) {
                chunk_size = last_chunk_size;
            }
        }
        
        // Copy chunk to the buffer
        memcpy(limbs + limbs_loaded, d_int->cache, chunk_size * sizeof(mp_limb_t));
        limbs_loaded += chunk_size;
    }
    
    // Free cache as we've loaded everything
    if (d_int->cache) {
        free(d_int->cache);
        d_int->cache = NULL;
    }
    
    // FIXED: Don't strip trailing zeros - use the exact number of limbs we wrote to disk
    // This is critical because for Chudnovsky algorithm values with large 2^k factors,
    // we might have many zeros in high order positions but still have valid non-zero data
    
    // Use the full size as recorded in the disk_int, which is what we originally wrote
    size_t import_size = d_int->total_size_in_limbs;
    
    // No need to scan for zeros - we already know how many limbs there are
    // Using the exact recorded size ensures we faithfully preserve the original number
    
    // FIXED: Use order=1 to match how we export limbs, for consistent ordering
    if (import_size > 0) {
        mpz_import(mpz_val, import_size, 1, sizeof(mp_limb_t), 0, 0, limbs);
    } else {
        mpz_set_ui(mpz_val, 0);
    }
    
    // Set the correct sign - using import_size instead of effective_size
    if (d_int->sign < 0 && import_size > 0) {
        mpz_neg(mpz_val, mpz_val);
    }
    
    // Verify the value is non-zero after import, especially for P and T
    if (mpz_sgn(mpz_val) == 0 && d_int->total_size_in_limbs > 0) {
        // If the value is zero but we expected non-zero data, this indicates potential data corruption
        // This should be rare now with the fixed mpz_import logic, but log it for diagnostics
        fprintf(stderr, "WARNING: Zero value detected after import for %s (total_size: %zu limbs)\n", 
                d_int->file_path, d_int->total_size_in_limbs);
        
        // No need to attempt recovery as we've fixed the root cause
        // Just log the issue and continue with the zero value
    }
    
    free(limbs);
    
    pthread_mutex_unlock(&d_int->lock);
}

// Compare the absolute values of two disk integers
// Returns: 1 if |a| > |b|, 0 if |a| == |b|, -1 if |a| < |b|
int disk_int_cmp_abs(disk_int* a, disk_int* b) {
    if (a == b) {
        return 0; // An object is always equal to itself in magnitude
    }
    // First check if either a or b is zero (without locks)
    if (a->total_size_in_limbs == 0 || a->sign == 0) {
        if (b->total_size_in_limbs == 0 || b->sign == 0) {
            return 0; // Both are zero, they're equal
        }
        return -1; // a is zero, b is non-zero, so |a| < |b|
    }
    
    if (b->total_size_in_limbs == 0 || b->sign == 0) {
        return 1; // b is zero, a is non-zero, so |a| > |b|
    }
    
    // Compare by total size in limbs first (still no locks needed)
    if (a->total_size_in_limbs > b->total_size_in_limbs) {
        return 1;
    } else if (a->total_size_in_limbs < b->total_size_in_limbs) {
        return -1;
    }
    
    // If we're here, both have the same number of limbs
    // We need to compare chunk by chunk, starting from the most significant chunks
    
    // Acquire locks in a consistent order to prevent deadlocks
    // Always lock the object with the lower memory address first
    disk_int *first, *second;
    int swap_result = 0;
    
    if ((uintptr_t)a < (uintptr_t)b) {
        first = a;
        second = b;
    } else {
        first = b;
        second = a;
        swap_result = 1; // Will need to negate result if we swap
    }
    
    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);
    
    // Flush any dirty cache data to disk
    if (a->dirty && a->cache) {
        save_chunk(a, a->cache_chunk_idx);
    }
    if (b->dirty && b->cache) {
        save_chunk(b, b->cache_chunk_idx);
    }
    
    // Start comparing from the most significant chunk (highest index)
    int result = 0;
    for (size_t i = a->num_chunks; i > 0; i--) {
        size_t chunk_idx = i - 1;
        
        // Allocate cache for a if not already allocated
        if (!a->cache) {
            a->cache = malloc(a->chunk_size * sizeof(mp_limb_t));
            if (!a->cache) {
                fprintf(stderr, "Error: Memory allocation failed in disk_int_cmp_abs for a->cache\n");
                pthread_mutex_unlock(&second->lock);
                pthread_mutex_unlock(&first->lock);
                return 0; // Error case, assume equal
            }
        }
        
        // Allocate cache for b if not already allocated
        if (!b->cache) {
            b->cache = malloc(b->chunk_size * sizeof(mp_limb_t));
            if (!b->cache) {
                fprintf(stderr, "Error: Memory allocation failed in disk_int_cmp_abs for b->cache\n");
                pthread_mutex_unlock(&second->lock);
                pthread_mutex_unlock(&first->lock);
                return 0; // Error case, assume equal
            }
        }
        
        // Load chunks
        if (load_chunk(a, chunk_idx) != 0 || load_chunk(b, chunk_idx) != 0) {
            fprintf(stderr, "Error: Failed to load chunks in disk_int_cmp_abs\n");
            pthread_mutex_unlock(&second->lock);
            pthread_mutex_unlock(&first->lock);
            return 0; // Error case, assume equal
        }
        
        // Determine chunk sizes - last chunks might be smaller
        size_t chunk_a_size = (chunk_idx == a->num_chunks - 1 && 
                          a->total_size_in_limbs % a->chunk_size > 0) ?
                          a->total_size_in_limbs % a->chunk_size : a->chunk_size;
        
        size_t chunk_b_size = (chunk_idx == b->num_chunks - 1 && 
                          b->total_size_in_limbs % b->chunk_size > 0) ?
                          b->total_size_in_limbs % b->chunk_size : b->chunk_size;
        
        // Since we already checked total size, the chunk sizes should be the same
        // for the most significant chunk, but we'll double-check
        size_t compare_size = MIN(chunk_a_size, chunk_b_size);
        
        // Compare limbs from most significant to least significant
        for (size_t j = compare_size; j > 0; j--) {
            mp_limb_t a_limb = ((mp_limb_t*)a->cache)[j-1];
            mp_limb_t b_limb = ((mp_limb_t*)b->cache)[j-1];
            
            if (a_limb > b_limb) {
                result = 1;
                goto cleanup;
            } else if (a_limb < b_limb) {
                result = -1;
                goto cleanup;
            }
            // If limbs are equal, continue to next limb
        }
        
        // If we're here and haven't returned, all limbs in this chunk are equal
        // Continue to the next chunk
    }
    
    // If we've compared all limbs and they're all equal, the numbers are equal
    result = 0;
    
cleanup:
    pthread_mutex_unlock(&second->lock);
    pthread_mutex_unlock(&first->lock);
    
    // If we swapped a and b, negate the result
    return swap_result ? -result : result;
}

// Subtract the absolute value of b from the absolute value of a, assuming |a| >= |b|
// result = |a| - |b|
void disk_int_sub_abs(disk_int* result, disk_int* a, disk_int* b) {
    // If |a| < |b|, we shouldn't be here - the caller should ensure |a| >= |b|
    // For safety, let's double-check and handle it properly
    if (disk_int_cmp_abs(a, b) < 0) {
        fprintf(stderr, "Error: disk_int_sub_abs called with |a| < |b|, which is not allowed\n");
        // Set result to zero
        result->total_size_in_limbs = 0;
        result->sign = 0;
        result->num_chunks = 0;
        if (result->cache) {
            free(result->cache);
            result->cache = NULL;
        }
        return;
    }
    
    // Handle special cases
    if (b->total_size_in_limbs == 0 || b->sign == 0) {
        // If b is zero, result = |a|
        // We need to copy a to result
        mpz_t mpz_a;
        mpz_init(mpz_a);
        disk_int_get_mpz(mpz_a, a);
        mpz_abs(mpz_a, mpz_a); // Get absolute value
        disk_int_set_mpz(result, mpz_a);
        mpz_clear(mpz_a);
        return;
    }
    
    if (a->total_size_in_limbs == 0 || a->sign == 0) {
        // If a is zero, result is zero
        result->total_size_in_limbs = 0;
        result->sign = 0;
        result->num_chunks = 0;
        if (result->cache) {
            free(result->cache);
            result->cache = NULL;
        }
        return;
    }
    
    // If the numbers are small enough, use in-memory subtraction
    size_t memory_available = DEFAULT_MEMORY_LIMIT * 1024 * 1024; // Convert to bytes
    size_t memory_needed = (a->total_size_in_limbs + b->total_size_in_limbs) * sizeof(mp_limb_t) * 3;
    
    if (memory_needed <= memory_available) {
        // We can fit the operation in memory
        mpz_t mpz_a, mpz_b, mpz_result;
        mpz_init(mpz_a);
        mpz_init(mpz_b);
        mpz_init(mpz_result);
        
        disk_int_get_mpz(mpz_a, a);
        disk_int_get_mpz(mpz_b, b);
        
        // Make both positive
        mpz_abs(mpz_a, mpz_a);
        mpz_abs(mpz_b, mpz_b);
        
        // Subtract b from a
        mpz_sub(mpz_result, mpz_a, mpz_b);
        
        // Store result
        disk_int_set_mpz(result, mpz_result);
        
        mpz_clear(mpz_a);
        mpz_clear(mpz_b);
        mpz_clear(mpz_result);
        return;
    }
    
    // True chunked subtraction algorithm
    printf("DEBUG: Using chunked subtraction algorithm\n");
    
    // Clear result first - make sure we're starting fresh
    // but keep the file path
    char file_path[MAX_PATH];
    strncpy(file_path, result->file_path, MAX_PATH);
    
    disk_int_clear(result);
    strncpy(result->file_path, file_path, MAX_PATH);
    
    // Create directory for result if it doesn't exist
    mkdir_recursive(result->file_path, 0755);
    
    // Initialize result with the same structure as a
    result->chunk_size = a->chunk_size;
    result->total_size_in_limbs = a->total_size_in_limbs; // Initial estimate, will adjust at the end
    result->sign = 1; // Always positive since we're computing |a| - |b|
    result->num_chunks = a->num_chunks;
    
    // Allocate cache for operands and result
    if (!a->cache) {
        a->cache = malloc(a->chunk_size * sizeof(mp_limb_t));
        if (!a->cache) {
            fprintf(stderr, "Error: Out of memory in disk_int_sub_abs for a->cache\n");
            return;
        }
    }
    
    if (!b->cache) {
        b->cache = malloc(b->chunk_size * sizeof(mp_limb_t));
        if (!b->cache) {
            fprintf(stderr, "Error: Out of memory in disk_int_sub_abs for b->cache\n");
            return;
        }
    }
    
    if (!result->cache) {
        result->cache = malloc(result->chunk_size * sizeof(mp_limb_t));
        if (!result->cache) {
            fprintf(stderr, "Error: Out of memory in disk_int_sub_abs for result->cache\n");
            return;
        }
    }
    
    // Keep track of borrow between chunks
    mp_limb_t borrow = 0;
    
    // Process each chunk
    for (size_t i = 0; i < result->num_chunks; i++) {
        // Load chunks
        if (load_chunk(a, i) != 0) {
            fprintf(stderr, "Error: Failed to load chunk %zu from a in disk_int_sub_abs\n", i);
            return;
        }
        
        // Determine chunk size for a
        size_t chunk_a_size = (i == a->num_chunks - 1 && 
                             a->total_size_in_limbs % a->chunk_size > 0) ?
                             a->total_size_in_limbs % a->chunk_size : a->chunk_size;
        
        // Load chunk from b if it exists
        mp_limb_t* chunk_b = NULL;
        size_t chunk_b_size = 0;
        
        if (i < b->num_chunks) {
            if (load_chunk(b, i) != 0) {
                fprintf(stderr, "Error: Failed to load chunk %zu from b in disk_int_sub_abs\n", i);
                return;
            }
            
            // Determine chunk size for b
            chunk_b_size = (i == b->num_chunks - 1 && 
                           b->total_size_in_limbs % b->chunk_size > 0) ?
                           b->total_size_in_limbs % b->chunk_size : b->chunk_size;
            
            chunk_b = (mp_limb_t*)b->cache;
        } else {
            // If we've processed all chunks of b, we just need to handle any remaining borrow
            chunk_b = NULL;
            chunk_b_size = 0;
        }
        
        // Prepare result chunk
        memset(result->cache, 0, result->chunk_size * sizeof(mp_limb_t));
        
        // Perform subtraction for this chunk with borrow
        for (size_t j = 0; j < chunk_a_size; j++) {
            mp_limb_t a_limb = ((mp_limb_t*)a->cache)[j];
            mp_limb_t b_limb = (j < chunk_b_size) ? chunk_b[j] : 0;
            
            // Subtract b_limb from a_limb
            mp_limb_t sub_result = a_limb - b_limb;
            
            // Check if this subtraction generated a borrow
            mp_limb_t sub_borrow = (sub_result > a_limb) ? 1 : 0;
            
            // Subtract previous borrow
            if (borrow > 0) {
                mp_limb_t old_sub = sub_result;
                sub_result = sub_result - borrow;
                // Check if this subtraction generated a new borrow
                if (sub_result > old_sub) {
                    sub_borrow = 1;
                }
            }
            
            // Store result
            ((mp_limb_t*)result->cache)[j] = sub_result;
            
            // Update borrow for next limb
            borrow = sub_borrow;
        }
        
        // Save this chunk
        result->cache_chunk_idx = i;
        result->dirty = true;
        if (save_chunk(result, i) != 0) {
            fprintf(stderr, "Error: Failed to save chunk %zu in disk_int_sub_abs\n", i);
            return;
        }
    }
    
    // If we still have a borrow after processing all chunks, something is wrong
    // This shouldn't happen if |a| >= |b|
    if (borrow > 0) {
        fprintf(stderr, "Warning: Unexpected borrow at the end of disk_int_sub_abs. This suggests |a| < |b|\n");
    }
    
    // Find the correct total size by locating the most significant non-zero limb
    size_t most_sig_chunk = result->num_chunks;
    mp_limb_t* most_sig_limb = NULL;
    
    // Scan chunks from most significant to least, looking for first non-zero chunk
    for (size_t i = result->num_chunks; i > 0; i--) {
        size_t chunk_idx = i - 1;
        
        if (load_chunk(result, chunk_idx) != 0) {
            fprintf(stderr, "Error: Failed to load chunk %zu in disk_int_sub_abs (finalization)\n", chunk_idx);
            continue; // Try the next chunk
        }
        
        // Check if this chunk has any non-zero limbs
        size_t chunk_size = (chunk_idx == result->num_chunks - 1 && 
                           result->total_size_in_limbs % result->chunk_size > 0) ?
                           result->total_size_in_limbs % result->chunk_size : result->chunk_size;
        
        bool found_non_zero = false;
        for (size_t j = chunk_size; j > 0; j--) {
            if (((mp_limb_t*)result->cache)[j-1] != 0) {
                // We found a non-zero limb
                most_sig_chunk = chunk_idx;
                most_sig_limb = &((mp_limb_t*)result->cache)[j-1];
                found_non_zero = true;
                break;
            }
        }
        
        if (found_non_zero) {
            break;
        }
    }
    
    // Adjust total size
    if (most_sig_limb != NULL) {
        // Calculate offset within the chunk using proper pointer subtraction
        ptrdiff_t diff = most_sig_limb - (mp_limb_t*)result->cache;
        if (diff < 0) {
            log_message("error", "Invalid limb pointer detected");
            return;
        }
        size_t offset = (size_t)diff;
        result->total_size_in_limbs = most_sig_chunk * result->chunk_size + offset + 1;
        result->num_chunks = most_sig_chunk + 1;
    } else {
        // All limbs are zero, result is zero
        result->total_size_in_limbs = 0;
        result->sign = 0;
        result->num_chunks = 0;
    }
    
    printf("DEBUG: Chunked subtraction completed successfully\n");
}

// Add two disk integers: result = a + b
void disk_int_add(disk_int* result, disk_int* a, disk_int* b) {
    // Handle cases with zero - only check total_size_in_limbs, never sign
    if (a->total_size_in_limbs == 0) {
        // a is zero, result = b
        mpz_t mpz_b;
        mpz_init(mpz_b);
        disk_int_get_mpz(mpz_b, b);
        disk_int_set_mpz(result, mpz_b);
        mpz_clear(mpz_b);
        return;
    }
    
    if (b->total_size_in_limbs == 0) {
        // b is zero, result = a
        mpz_t mpz_a;
        mpz_init(mpz_a);
        disk_int_get_mpz(mpz_a, a);
        disk_int_set_mpz(result, mpz_a);
        mpz_clear(mpz_a);
        return;
    }
    
    // If either input is not chunked, use the old method
    if (a->num_chunks == 0 || b->num_chunks == 0) {
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
        return;
    }
    
    // Handle different sign combinations using the new comparison and subtraction functions
    if (a->sign != b->sign) {
        // Different signs: result = the larger value - the smaller value, with appropriate sign
        int cmp = disk_int_cmp_abs(a, b);
        
        if (cmp == 0) {
            // |a| == |b|, so a + b = 0
            result->total_size_in_limbs = 0;
            result->sign = 0;
            result->num_chunks = 0;
            if (result->cache) {
                free(result->cache);
                result->cache = NULL;
            }
            return;
        } else if (cmp > 0) {
            // |a| > |b|, so sign of result is sign of a
            disk_int_sub_abs(result, a, b);
            result->sign = a->sign;
            return;
        } else {
            // |a| < |b|, so sign of result is sign of b
            disk_int_sub_abs(result, b, a);
            result->sign = b->sign;
            return;
        }
    }
    
    // If we're here, signs are the same, so we use regular addition
    // The sign of the result will be the same as a and b
    
    // Determine if we can operate in chunks (using available memory)
    size_t memory_available = DEFAULT_MEMORY_LIMIT * 1024 * 1024; // Convert to bytes
    size_t max_size = MAX(a->total_size_in_limbs, b->total_size_in_limbs);
    size_t memory_needed = max_size * sizeof(mp_limb_t) * 3; // For a, b, and result
    
    if (memory_needed <= memory_available) {
        // We can fit the operation in memory, use the standard approach
        mpz_t mpz_a, mpz_b, mpz_result;
        mpz_init(mpz_a);
        mpz_init(mpz_b);
        mpz_init(mpz_result);
        
        disk_int_get_mpz(mpz_a, a);
        disk_int_get_mpz(mpz_b, b);
        mpz_add(mpz_result, mpz_a, mpz_b);
        disk_int_set_mpz(result, mpz_result);
        
        mpz_clear(mpz_a);
        mpz_clear(mpz_b);
        mpz_clear(mpz_result);
        return;
    }
    
    // True chunked addition algorithm for same sign operands
    printf("DEBUG: Using chunked addition algorithm for same sign operands\n");
    
    // Prepare the result disk_int
    size_t result_chunk_size = determine_optimal_chunk_size(memory_available / 3, 
                                                           MAX(a->total_size_in_limbs, 
                                                               b->total_size_in_limbs) + 1);
    
    // Ensure the chunk sizes are compatible or use a common denominator
    size_t common_chunk_size = result_chunk_size;
    result->chunk_size = common_chunk_size;
    
    // Make sure result is properly initialized
    if (result->file_path[0] == '\0') {
        fprintf(stderr, "Error: Result disk_int not initialized before addition\n");
        return;
    }
    
    // Estimate the total size and initialize chunks
    size_t max_chunks = MAX(a->num_chunks, b->num_chunks) + 1; // +1 for potential carry
    result->num_chunks = max_chunks;
    result->total_size_in_limbs = (max_chunks - 1) * common_chunk_size +
                                  MAX(a->total_size_in_limbs % common_chunk_size,
                                      b->total_size_in_limbs % common_chunk_size) + 1;
    
    // Allocate cache for each disk_int if not already allocated
    if (!a->cache) {
        a->cache = malloc(a->chunk_size * sizeof(mp_limb_t));
        if (!a->cache) {
            fprintf(stderr, "Error: Out of memory in disk_int_add for a->cache\n");
            return;
        }
    }
    
    if (!b->cache) {
        b->cache = malloc(b->chunk_size * sizeof(mp_limb_t));
        if (!b->cache) {
            fprintf(stderr, "Error: Out of memory in disk_int_add for b->cache\n");
            free(a->cache);
            a->cache = NULL;
            return;
        }
    }
    
    if (!result->cache) {
        result->cache = malloc(result->chunk_size * sizeof(mp_limb_t));
        if (!result->cache) {
            fprintf(stderr, "Error: Out of memory in disk_int_add for result->cache\n");
            free(a->cache);
            free(b->cache);
            a->cache = NULL;
            b->cache = NULL;
            return;
        }
    }
    
    // Initialize carry
    mp_limb_t carry = 0;
    
    // Process each chunk from least to most significant
    for (size_t i = 0; i < max_chunks; i++) {
        // Load chunk A
        mp_limb_t* chunk_a = NULL;
        size_t chunk_a_size = 0;
        
        if (i < a->num_chunks) {
            if (load_chunk(a, i) != 0) {
                fprintf(stderr, "Error: Failed to load chunk %zu from a\n", i);
                goto cleanup;
            }
            chunk_a = (mp_limb_t*)a->cache;
            chunk_a_size = (i == a->num_chunks - 1 && 
                            a->total_size_in_limbs % a->chunk_size > 0) ?
                            a->total_size_in_limbs % a->chunk_size : a->chunk_size;
        } else if (carry == 0) {
            // No more chunks in a and no carry, we're done if b has no more chunks
            if (i >= b->num_chunks) {
                break;
            }
            // Allocate a zero chunk for a
            chunk_a = (mp_limb_t*)calloc(common_chunk_size, sizeof(mp_limb_t));
            if (!chunk_a) {
                fprintf(stderr, "Error: Out of memory in disk_int_add for temporary chunk_a\n");
                goto cleanup;
            }
            chunk_a_size = common_chunk_size;
        } else {
            // We have a carry but no more chunks in a
            chunk_a = (mp_limb_t*)calloc(common_chunk_size, sizeof(mp_limb_t));
            if (!chunk_a) {
                fprintf(stderr, "Error: Out of memory in disk_int_add for temporary chunk_a\n");
                goto cleanup;
            }
            chunk_a_size = common_chunk_size;
        }
        
        // Load chunk B
        mp_limb_t* chunk_b = NULL;
        size_t chunk_b_size = 0;
        
        if (i < b->num_chunks) {
            if (load_chunk(b, i) != 0) {
                fprintf(stderr, "Error: Failed to load chunk %zu from b\n", i);
                if (chunk_a != a->cache) free(chunk_a);
                goto cleanup;
            }
            chunk_b = (mp_limb_t*)b->cache;
            chunk_b_size = (i == b->num_chunks - 1 && 
                            b->total_size_in_limbs % b->chunk_size > 0) ?
                            b->total_size_in_limbs % b->chunk_size : b->chunk_size;
        } else if (carry == 0 && chunk_a == a->cache) {
            // No more chunks in b, no carry, and a has a real chunk
            // Just copy the chunk from a to result
            memcpy(result->cache, chunk_a, chunk_a_size * sizeof(mp_limb_t));
            
            result->cache_chunk_idx = i;
            result->dirty = true;
            
            // Save the chunk
            if (save_chunk(result, i) != 0) {
                fprintf(stderr, "Error: Failed to save result chunk %zu\n", i);
                goto cleanup;
            }
            
            continue;
        } else {
            // We have a carry or no more chunks in b
            chunk_b = (mp_limb_t*)calloc(common_chunk_size, sizeof(mp_limb_t));
            if (!chunk_b) {
                fprintf(stderr, "Error: Out of memory in disk_int_add for temporary chunk_b\n");
                if (chunk_a != a->cache) free(chunk_a);
                goto cleanup;
            }
            chunk_b_size = common_chunk_size;
        }
        
        // Perform addition for this chunk with carry
        mp_limb_t new_carry = 0;
        size_t chunk_result_size = MAX(chunk_a_size, chunk_b_size);
        
        for (size_t j = 0; j < chunk_result_size; j++) {
            mp_limb_t a_val = (j < chunk_a_size) ? chunk_a[j] : 0;
            mp_limb_t b_val = (j < chunk_b_size) ? chunk_b[j] : 0;
            
            // Add with carry
            mp_limb_t sum = a_val + b_val + carry;
            
            // Check for overflow
            if (sum < a_val || (sum == a_val && (b_val > 0 || carry > 0))) {
                new_carry = 1;
            } else {
                new_carry = 0;
            }
            
            // Store result
            ((mp_limb_t*)result->cache)[j] = sum;
        }
        
        // If we have a new carry and we've filled the chunk, add an extra limb
        if (new_carry && chunk_result_size < common_chunk_size) {
            ((mp_limb_t*)result->cache)[chunk_result_size] = 1;
            chunk_result_size++;
            new_carry = 0;
        }
        
        // Free temporary chunks if they were allocated
        if (chunk_a != a->cache) free(chunk_a);
        if (chunk_b != b->cache) free(chunk_b);
        
        // Set the result chunk index and mark as dirty
        result->cache_chunk_idx = i;
        result->dirty = true;
        
        // Save the chunk
        if (save_chunk(result, i) != 0) {
            fprintf(stderr, "Error: Failed to save result chunk %zu\n", i);
            goto cleanup;
        }
        
        // Update carry for next chunk
        carry = new_carry;
    }
    
    // Handle final carry if needed
    if (carry > 0) {
        // Allocate a new chunk for the carry
        memset(result->cache, 0, result->chunk_size * sizeof(mp_limb_t));
        ((mp_limb_t*)result->cache)[0] = carry;
        
        result->cache_chunk_idx = max_chunks;
        result->dirty = true;
        
        // Save the chunk
        if (save_chunk(result, max_chunks) != 0) {
            fprintf(stderr, "Error: Failed to save carry chunk\n");
            goto cleanup;
        }
        
        // Update num_chunks
        result->num_chunks = max_chunks + 1;
    } else {
        // No final carry, just make sure num_chunks is set correctly
        result->num_chunks = max_chunks;
    }
    
    // Determine the correct sign based on the larger number
    if (a->sign == b->sign) {
        result->sign = a->sign;
    } else {
        // Signs differ, need to determine which absolute value is larger
        // This is a simplification - for a true implementation, we'd need to compare
        if (a->total_size_in_limbs > b->total_size_in_limbs) {
            result->sign = a->sign;
        } else if (b->total_size_in_limbs > a->total_size_in_limbs) {
            result->sign = b->sign;
        } else {
            // Same number of limbs, would need to compare the actual values
            // For simplicity, we'll use a->sign as a default
            result->sign = a->sign;
        }
    }
    
    // Success
    printf("DEBUG: Chunked addition completed successfully\n");
    return;
    
cleanup:
    // Cleanup on error
    if (a->cache) {
        free(a->cache);
        a->cache = NULL;
    }
    
    if (b->cache) {
        free(b->cache);
        b->cache = NULL;
    }
    
    if (result->cache) {
        free(result->cache);
        result->cache = NULL;
    }
}

// Helper function to add product of two chunks to result at specified position
// ===================================================================
// Add a product to the result at specified position
// ===================================================================
// This critical function adds a multi-limb product to a disk_int at a specified position.
// It handles all boundary conditions, including carrying across chunk boundaries and
// proper size adjustment. Essentially, it performs: result += product << position
void add_product_to_result(disk_int* result, mp_limb_t* product, size_t product_size, size_t position) {
    // ===================================================================
    // Initialization and preparation
    // ===================================================================
    
    // Map the product position to the appropriate chunk and offset
    size_t start_chunk = position / result->chunk_size;             // Which chunk to start with
    size_t offset_in_chunk = position % result->chunk_size;         // Offset within that chunk
    
    // Calculate how many chunks we need to update
    // We may need an extra chunk for the final carry
    size_t chunks_needed = (offset_in_chunk + product_size + result->chunk_size - 1) / result->chunk_size;
    
    // Initialize carry tracking
    mp_limb_t carry = 0;                                            // Tracks carries across limbs
    size_t product_idx = 0;                                         // Current position in product array
    
    // ===================================================================
    // Main addition loop: Process each affected chunk
    // ===================================================================
    for (size_t i = 0; i < chunks_needed; i++) {
        size_t current_chunk = start_chunk + i;                     // Chunk we're currently processing
        
        // Ensure the result has enough chunks allocated
        if (current_chunk >= result->num_chunks) {
            result->num_chunks = current_chunk + 1;                 // Expand result as needed
        }
        
        // Load the current chunk from result or create a new one if it doesn't exist
        if (load_chunk(result, current_chunk) != 0) {
            // Chunk doesn't exist, create a new zeroed chunk
            memset(result->cache, 0, result->chunk_size * sizeof(mp_limb_t));
            result->cache_chunk_idx = current_chunk;
        }
        
        // Determine the range within this chunk to update
        size_t start_pos = (i == 0) ? offset_in_chunk : 0;          // Starting position in chunk
        // How many limbs to add in this chunk - either rest of chunk or rest of product
        size_t limbs_to_add = MIN(result->chunk_size - start_pos, product_size - product_idx);
        
        // ===================================================================
        // Limb-by-limb addition with carry propagation within chunk
        // ===================================================================
        for (size_t j = 0; j < limbs_to_add; j++) {
            // Get the current values
            mp_limb_t old_val = ((mp_limb_t*)result->cache)[start_pos + j]; // Existing value
            mp_limb_t prod_val = product[product_idx];                     // Value from product
            
            // Two-step addition with carry handling:
            
            // Step 1: Add the previous carry to the existing value
            mp_limb_t sum = old_val + carry;                        // Add previous carry
            mp_limb_t carry1 = (sum < old_val) ? 1 : 0;             // Detect carry from first addition
                                                                     // If sum < old_val, an overflow occurred
            
            // Step 2: Add the product value to the intermediate sum
            mp_limb_t new_sum = sum + prod_val;                     // Add product value
            mp_limb_t carry2 = (new_sum < sum) ? 1 : 0;             // Detect carry from second addition
            
            // Combine both carries for next iteration
            carry = carry1 + carry2;                                // Total carry for next limb
            
            // Store the result in the chunk
            ((mp_limb_t*)result->cache)[start_pos + j] = new_sum;   // Update result
            product_idx++;                                          // Move to next product limb
        }
        
        // ===================================================================
        // Handle remaining carry within current chunk
        // ===================================================================
        if (carry > 0 && start_pos + limbs_to_add < result->chunk_size) {
            // Continue propagating carry within the current chunk if there's space
            size_t k = start_pos + limbs_to_add;                    // Start after the last limb we added
            
            // Keep propagating carry until it's consumed or we run out of space
            while (carry > 0 && k < result->chunk_size) {
                mp_limb_t old_val = ((mp_limb_t*)result->cache)[k]; // Get existing value
                mp_limb_t new_val = old_val + carry;                // Add carry
                
                // Update carry for next position
                carry = (new_val < old_val) ? 1 : 0;                // Detect if we generated a new carry
                
                // Store updated value
                ((mp_limb_t*)result->cache)[k] = new_val;
                k++;
            }
        }
        
        // Persist the updated chunk to disk
        result->dirty = true;
        if (save_chunk(result, current_chunk) != 0) {
            fprintf(stderr, "Error: Failed to save chunk %zu in add_product_to_result\n", current_chunk);
            return;
        }
    }
    
    // ===================================================================
    // Handle final carry propagation across additional chunks
    // ===================================================================
    // If we still have a carry after processing all planned chunks,
    // we need to create or load additional chunks to handle it
    size_t extra_chunk = start_chunk + chunks_needed;               // Next chunk after planned ones
    
    while (carry > 0) {
        // Either create a new chunk or load an existing one
        if (extra_chunk >= result->num_chunks) {
            // Create a new zero-filled chunk
            result->num_chunks = extra_chunk + 1;
            memset(result->cache, 0, result->chunk_size * sizeof(mp_limb_t));
            result->cache_chunk_idx = extra_chunk;
        } else {
            // Load the existing chunk
            if (load_chunk(result, extra_chunk) != 0) {
                // If load fails, create a new zeroed chunk
                memset(result->cache, 0, result->chunk_size * sizeof(mp_limb_t));
                result->cache_chunk_idx = extra_chunk;
            }
        }
        
        // Add carry to the first limb of this chunk
        mp_limb_t old_val = ((mp_limb_t*)result->cache)[0];
        mp_limb_t new_val = old_val + carry;
        
        // Update carry based on whether adding to the first limb generated a new carry
        carry = (new_val < old_val) ? 1 : 0;
        
        // Store the updated value
        ((mp_limb_t*)result->cache)[0] = new_val;
        
        // If we still have a carry, propagate it through this chunk
        size_t k = 1;
        while (carry > 0 && k < result->chunk_size) {
            old_val = ((mp_limb_t*)result->cache)[k];
            new_val = old_val + carry;
            
            // Update carry
            carry = (new_val < old_val) ? 1 : 0;
            
            // Store the updated value
            ((mp_limb_t*)result->cache)[k] = new_val;
            k++;
        }
        
        // Persist the updated chunk
        result->dirty = true;
        if (save_chunk(result, extra_chunk) != 0) {
            fprintf(stderr, "Error: Failed to save chunk %zu in add_product_to_result\n", extra_chunk);
            return;
        }
        
        // Move to next chunk if we still have a carry
        extra_chunk++;
    }
    
    // ===================================================================
    // Update total size of the result
    // ===================================================================
    // We need to find the highest non-zero limb to set the correct total size
    
    // Calculate the potential size based on the number of chunks
    size_t potential_new_size = (result->num_chunks - 1) * result->chunk_size;
    
    // Load the last chunk to find the most significant non-zero limb
    if (load_chunk(result, result->num_chunks - 1) == 0) {
        // Scan from most significant to least significant limb
        int highest_limb = -1;
        for (size_t i = result->chunk_size; i > 0; i--) {
            if (((mp_limb_t*)result->cache)[i-1] != 0) {
                highest_limb = (int)(i-1);                          // Found highest non-zero limb
                break;
            }
        }
        
        // If we found a non-zero limb, update the total size
        if (highest_limb >= 0) {
            // Add the offset of the highest non-zero limb within the last chunk
            potential_new_size += highest_limb + 1;
            
            // Only update if the new size is larger
            if (potential_new_size > result->total_size_in_limbs) {
                result->total_size_in_limbs = potential_new_size;
            }
        }
    }
}

// Multiply two disk integers with memory optimization
void disk_int_mul(disk_int* result, disk_int* a, disk_int* b) {
    // For cases where either number is zero
    // Only check total_size_in_limbs - sign==0 is NOT a reliable indicator!
    if (a->total_size_in_limbs == 0 || b->total_size_in_limbs == 0) {
        // Set result to zero
        result->total_size_in_limbs = 0;
        result->sign = 0;
        result->num_chunks = 0;
        // Free any existing cache
        if (result->cache) {
            free(result->cache);
            result->cache = NULL;
        }
        return;
    }

    // Check if numbers are small enough for in-memory multiplication
    size_t memory_available = DEFAULT_MEMORY_LIMIT * 1024 * 1024; // Convert to bytes
    size_t memory_needed = (a->total_size_in_limbs + b->total_size_in_limbs) * sizeof(mp_limb_t) * 3;
    
    if (memory_needed <= memory_available) {
        // We can fit the operation in memory, use the standard approach
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
        return;
    }
    
    // True chunked multiplication algorithm
    printf("DEBUG: Using chunked multiplication algorithm\n");
    
    // Clear result first - make sure we're starting fresh
    // but keep the file path
    char file_path[MAX_PATH];
    strncpy(file_path, result->file_path, MAX_PATH);
    
    disk_int_clear(result);
    strncpy(result->file_path, file_path, MAX_PATH);
    
    // Create directory for result if it doesn't exist
    mkdir_recursive(result->file_path, 0755);
    
    // Initialize result
    result->chunk_size = determine_optimal_chunk_size(memory_available / 3, 
                                                     MAX(a->total_size_in_limbs, b->total_size_in_limbs));
    result->total_size_in_limbs = a->total_size_in_limbs + b->total_size_in_limbs;
    result->sign = a->sign * b->sign; // Multiply signs
    result->num_chunks = (result->total_size_in_limbs + result->chunk_size - 1) / result->chunk_size;
    
    // Allocate temporary buffers
    mp_limb_t* product = (mp_limb_t*)malloc(sizeof(mp_limb_t) * 2 * result->chunk_size);
    if (!product) {
        fprintf(stderr, "Error: Out of memory for chunk product\n");
        return;
    }
    
    // Allocate cache for result if not already allocated
    if (!result->cache) {
        result->cache = malloc(result->chunk_size * sizeof(mp_limb_t));
        if (!result->cache) {
            fprintf(stderr, "Error: Out of memory for result cache\n");
            free(product);
            return;
        }
    }
    
    // Schoolbook multiplication algorithm: multiply each chunk of a with each chunk of b
    for (size_t i = 0; i < a->num_chunks; i++) {
        // Load chunk from a
        if (load_chunk(a, i) != 0) {
            fprintf(stderr, "Error: Failed to load chunk %zu from a\n", i);
            free(product);
            return;
        }
        
        // Determine chunk size for a
        size_t chunk_a_size = (i == a->num_chunks - 1 && 
                              a->total_size_in_limbs % a->chunk_size > 0) ?
                              a->total_size_in_limbs % a->chunk_size : a->chunk_size;
        
        for (size_t j = 0; j < b->num_chunks; j++) {
            // Load chunk from b
            if (load_chunk(b, j) != 0) {
                fprintf(stderr, "Error: Failed to load chunk %zu from b\n", j);
                free(product);
                return;
            }
            
            // Determine chunk size for b
            size_t chunk_b_size = (j == b->num_chunks - 1 && 
                                  b->total_size_in_limbs % b->chunk_size > 0) ?
                                  b->total_size_in_limbs % b->chunk_size : b->chunk_size;
            
            // Clear product buffer
            memset(product, 0, sizeof(mp_limb_t) * 2 * result->chunk_size);
            
            // Multiply these chunks (limb by limb)
            size_t product_size = chunk_a_size + chunk_b_size;
            
            // ===================================================================
            // Double-width multiplication implementation
            // ===================================================================
            // This section implements a critical multiplication routine that computes
            // the full double-width (128-bit) product of two mp_limb_t values (64-bit).
            // The implementation uses different strategies based on the platform:
            
            // ARM64-optimized implementation using native instructions
            #ifdef __aarch64__
            // ARM64 provides hardware support for full-width multiplication via:
            // - 'mul' instruction for the low 64 bits of the product
            // - 'umulh' instruction for the high 64 bits of the product
            #define umul_ppmm(high, low, a, b) \
                __asm__ ("mul %0, %2, %3\n\t" \
                         "umulh %1, %2, %3" \
                         : "=&r" (low), "=r" (high) \
                         : "r" (a), "r" (b) \
                         : "cc")
            #else
            // Fallback implementation for non-ARM64 platforms
            static inline void umul_ppmm(mp_limb_t* high_ptr, mp_limb_t* low_ptr, mp_limb_t a, mp_limb_t b) {
                #if defined(__GNUC__) && defined(__SIZEOF_INT128__)
                    // GCC/Clang with 128-bit integer support
                    // This is the most reliable and efficient implementation for non-ARM64
                    // platforms, using the compiler's built-in 128-bit integer type
                    unsigned __int128 p = (unsigned __int128)a * b;
                    *low_ptr = (mp_limb_t)p;                         // Low 64 bits
                    *high_ptr = (mp_limb_t)(p >> (sizeof(mp_limb_t) * 8)); // High 64 bits
                #else
                    // Manual implementation for platforms without 128-bit integer support
                    // Uses the "grade school" multiplication algorithm with 32-bit limbs
                    // to compute the full 64x64→128 bit product
                    
                    // Split each 64-bit limb into high and low 32-bit parts
                    const int limb_bits = sizeof(mp_limb_t) * 8;     // Total bits (64)
                    const int half_bits = limb_bits / 2;             // Half bits (32)
                    const mp_limb_t lower_mask = ((mp_limb_t)1 << half_bits) - 1; // 0xFFFFFFFF
                    
                    // Extract high and low 32-bit parts
                    mp_limb_t a_high = a >> half_bits;               // Upper 32 bits of a
                    mp_limb_t a_low = a & lower_mask;                // Lower 32 bits of a
                    mp_limb_t b_high = b >> half_bits;               // Upper 32 bits of b
                    mp_limb_t b_low = b & lower_mask;                // Lower 32 bits of b
                    
                    // Compute the four partial 32x32→64 bit products
                    mp_limb_t low = a_low * b_low;                   // LL: Lower x Lower
                    mp_limb_t mid1 = a_low * b_high;                 // LH: Lower x Higher
                    mp_limb_t mid2 = a_high * b_low;                 // HL: Higher x Lower
                    mp_limb_t high = a_high * b_high;                // HH: Higher x Higher
                    
                    // Combine partial products with proper carry handling
                    mp_limb_t mid = mid1 + mid2;                     // Add "middle" terms
                    if (mid < mid1) {
                        // If there was a carry in mid sum, add it to high part
                        high += ((mp_limb_t)1 << half_bits);         // Carry = 2^32
                    }
                    
                    // Add upper bits of mid to high
                    high += (mid >> half_bits);                      // High bits of middle
                    
                    // Combine lower bits of mid with low
                    mp_limb_t mid_low_shifted = (mid & lower_mask) << half_bits; // Shifted to bits 32-63
                    *low_ptr = low + mid_low_shifted;                // Lower 64 bits of result
                    
                    // If there was a carry from low + mid_low_shifted, propagate to high
                    if (*low_ptr < low) {
                        high += 1;                                   // Add carry to high
                    }
                    
                    *high_ptr = high;                                // Upper 64 bits of result
                #endif
            }
            #endif
            
            // ===================================================================
            // Main multiplication loop - Long multiplication algorithm implementation
            // ===================================================================
            // This implements the standard "long multiplication" algorithm with proper
            // carry propagation working across multiple limbs
            
            // Outer loop: Multiply each limb in chunk_a by all limbs in chunk_b
            for (size_t k = 0; k < chunk_a_size; k++) {
                mp_limb_t a_limb = ((mp_limb_t*)a->cache)[k];  // Current limb from a
                mp_limb_t carry = 0;                           // Initialize carry for this row
                
                // Skip work entirely if a_limb is zero (optimization)
                if (a_limb == 0) {
                    continue;
                }
                
                // Inner loop: Multiply a_limb by each limb in chunk_b
                for (size_t m = 0; m < chunk_b_size; m++) {
                    mp_limb_t b_limb = ((mp_limb_t*)b->cache)[m];  // Current limb from b
                    mp_limb_t prod_hi, prod_lo;                    // Double-width product
                    
                    // Compute full double-width product of a_limb × b_limb
                    // This gives us a 128-bit result split into prod_hi (upper 64 bits)
                    // and prod_lo (lower 64 bits)
                    #ifdef __aarch64__
                    umul_ppmm(prod_hi, prod_lo, a_limb, b_limb);   // ARM64 version
                    #else
                    umul_ppmm(&prod_hi, &prod_lo, a_limb, b_limb); // Non-ARM64 version
                    #endif
                    
                    // First, add the existing product value to the low part
                    // This is the value already in the accumulator at this position
                    mp_limb_t old_prod_lo = prod_lo;               // Save for carry detection
                    prod_lo += product[k + m];                     // Add existing product
                    
                    // Check if we generated a carry when adding product[k + m]
                    if (prod_lo < old_prod_lo) {
                        prod_hi += 1;                              // Propagate carry to high part
                    }
                    
                    // Next, add the carry from the previous position
                    old_prod_lo = prod_lo;                         // Save again for carry detection
                    prod_lo += carry;                              // Add previous carry
                    
                    // Check if we generated a carry when adding the previous carry
                    if (prod_lo < old_prod_lo) {
                        prod_hi += 1;                              // Propagate carry to high part
                    }
                    
                    // Store the result at the appropriate position
                    product[k + m] = prod_lo;                      // Store low part
                    carry = prod_hi;                               // High part becomes carry for next position
                }
                
                // Handle final carry from the inner loop
                if (carry > 0) {
                    // Add carry to the position after the last b_limb
                    mp_limb_t old_val = product[k + chunk_b_size];
                    product[k + chunk_b_size] += carry;
                    
                    // Check if adding carry generated a new carry
                    if (product[k + chunk_b_size] < old_val) {
                        // We have another carry to propagate
                        // This requires special handling since it can ripple through
                        // multiple positions if they're all at their maximum value
                        size_t pos = k + chunk_b_size + 1;
                        
                        // Propagate carry through consecutive limbs with value 0
                        // (which means they just overflowed from 0xFFFFFFFFFFFFFFFF to 0)
                        while (pos < 2 * result->chunk_size && product[pos-1] == 0) {
                            product[pos]++;                        // Add carry to this position
                            if (product[pos] != 0) break;          // If no new overflow, we're done
                            pos++;                                 // Otherwise, continue to next position
                        }
                    }
                }
            }
            
            // Add this product to the result at the appropriate position
            size_t position = i * a->chunk_size + j * b->chunk_size;
            add_product_to_result(result, product, product_size, position);
        }
    }
    
    // Clean up
    free(product);
    
    // Success
    printf("DEBUG: Chunked multiplication completed successfully\n");
}
