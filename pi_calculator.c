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
#include <stdarg.h>

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

// Define MIN and MAX macros
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define MAX_CHUNK_SIZE (512*1024*1024)     // Maximum chunk size of 512MB
#define MAX_JOBS 100                       // Maximum concurrent calculation jobs
#define MAX_JOB_AGE_SECONDS 86400          // Maximum age of completed jobs (1 day)
#define MAX_JOB_ERROR_MSG 256              // Maximum length of job error message

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

// Logging levels
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO 1
#define LOG_LEVEL_WARNING 2
#define LOG_LEVEL_ERROR 3
#define LOG_LEVEL_CRITICAL 4

// AARCH64 specific
#define CACHE_LINE_SIZE 64                 // Typical L1 cache line size for AARCH64

// String constants
#define ALGORITHM_GL "GL"                  // Gauss-Legendre algorithm identifier
#define ALGORITHM_CH "CH"                  // Chudnovsky algorithm identifier

// Calculation modes
#define CALC_MODE_SYNC 0                   // Synchronous calculation
#define CALC_MODE_ASYNC 1                  // Asynchronous calculation

// Forward declarations for key functions
void* timeout_monitor_thread(void* arg);

// Forward declarations of essential types and globals
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

// Job status enum
typedef enum {
    JOB_STATUS_QUEUED,
    JOB_STATUS_RUNNING,
    JOB_STATUS_COMPLETED,
    JOB_STATUS_FAILED,
    JOB_STATUS_CANCELED
} job_status_t;

// Calculation job structure forward declaration
typedef struct calculation_job {
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
} calculation_job;

// Global job management
extern calculation_job jobs[MAX_JOBS];
extern pthread_mutex_t jobs_lock;
extern bool g_server_running;
extern volatile bool g_shutdown_flag;

// Config structure forward declaration
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
    int log_level;                         // Logging level (numeric)
    struct {
        int level;                         // Logging level (numeric) - keep for compatibility
    } logging;
    
    // Disk and checkpointing
    char work_dir[MAX_PATH];               // Working directory
    bool checkpointing_enabled;            // Whether checkpointing is enabled
    unsigned long checkpoint_interval;     // Interval between checkpoints (seconds)
    
    // Execution mode
    execution_mode_t mode;                 // Server or CLI mode
} config_t;

extern config_t config;

// Forward declaration for logging
typedef enum {
    LOG_OUTPUT_CONSOLE,
    LOG_OUTPUT_FILE
} log_output_t;

// Global variables
volatile sig_atomic_t g_segfault_occurred = 0; // Flag to indicate a segmentation fault
char g_crash_log_path[MAX_PATH]; // Path where crash log should be written

// Forward declarations of structs
typedef struct disk_int disk_int;
typedef struct calc_thread_pool calc_thread_pool;

// Global variables (after typedefs)
extern calc_thread_pool* g_calc_pool;

// Function prototypes
int safe_path_join(char* dest, size_t dest_size, const char* base_path, const char* component);
int save_chunk(disk_int* d_int, size_t chunk_idx);
int mkdir_recursive(const char* path, mode_t mode);
int convert_log_level_string_to_int(const char* level_str);
void perform_crash_logging(const char *crash_log_path);
void unified_log(int level, const char* format, ...);

// Async-signal safe handler for segmentation faults
void segfault_handler(int sig) {
    // Set the global flag to indicate the crash
    g_segfault_occurred = 1;
    
    // Generate a timestamp-based filename for the crash log
    // We'll use a static filename since we can't use sprintf in signal handlers
    const char *crash_file = "pi_calculator_crash.log";
    
    // Store the path for the monitoring thread to use
    int i;
    for (i = 0; i < MAX_PATH - 1 && crash_file[i] != '\0'; i++) {
        g_crash_log_path[i] = crash_file[i];
    }
    g_crash_log_path[i] = '\0';
    
    // Write a minimal message to stderr using only async-signal safe functions
    const char *msg = "CRITICAL: Caught segmentation fault. Attempting crash logging via fork()...\n";
    write(STDERR_FILENO, msg, strlen(msg));
    
    // Use fork() to create a separate process for crash logging
    // This is async-signal safe according to POSIX
    pid_t pid = fork();
    
    if (pid == -1) {
        // Fork failed, simply exit
        const char *fork_fail_msg = "CRITICAL: Fork failed for crash logging, exiting.\n";
        write(STDERR_FILENO, fork_fail_msg, strlen(fork_fail_msg));
        _exit(1);
    } 
    else if (pid == 0) {
        // Child process - perform crash logging
        perform_crash_logging(g_crash_log_path);
        _exit(0);  // Child exits after logging
    } 
    else {
        // Parent process - exit immediately with error code
        const char *parent_msg = "Parent process exiting. Check crash log for details.\n";
        write(STDERR_FILENO, parent_msg, strlen(parent_msg));
        _exit(1);
    }
}

// Non-signal function that can be called by a monitoring thread to perform thorough crash logging
void perform_crash_logging(const char *crash_log_path) {
    char buf[256];
    time_t now;
    struct tm *tm_info;
    FILE *crash_log = NULL;
    
    // Get current time for the log
    time(&now);
    tm_info = localtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Create a crash log file
    crash_log = fopen(crash_log_path, "w");
    
    // Log to stderr and crash file if available
    fprintf(stderr, "[%s] CRITICAL: Caught signal (segmentation fault)\n", buf);
    if (crash_log) {
        fprintf(crash_log, "[%s] CRITICAL: Caught signal (segmentation fault)\n", buf);
    }
    
    // Process info
    fprintf(stderr, "Process ID: %d\n", getpid());
    if (crash_log) {
        fprintf(crash_log, "Process ID: %d\n", getpid());
        fprintf(crash_log, "Timestamp: %ld\n", (long)now);
    }
    
    // System information
    fprintf(stderr, "System information:\n");
    if (crash_log) {
        fprintf(crash_log, "System information:\n");
        
        // Get OS information if possible
#ifdef __APPLE__
        fprintf(crash_log, "OS: macOS\n");
#elif defined(__linux__)
        fprintf(crash_log, "OS: Linux\n");
#else
        fprintf(crash_log, "OS: Unknown\n");
#endif
    }
    
    // Log stack trace if possible
    fprintf(stderr, "Stack trace:\n");
    if (crash_log) {
        fprintf(crash_log, "Stack trace:\n");
    }
    
    // Platform-specific backtrace handling
#ifdef __APPLE__
    fprintf(stderr, "Stack trace not available on macOS in segfault handler\n");
    if (crash_log) {
        fprintf(crash_log, "Stack trace not available on macOS in segfault handler\n");
    }
#else
    // Try different backtrace utilities
    system("backtrace $PPID 2>/dev/null || "
           "gdb -ex 'set pagination 0' -ex 'thread apply all bt' -ex 'quit' -p $$ 2>/dev/null || "
           "echo 'Backtrace utilities not available'");
#endif
    
    // Log memory info if possible
    fprintf(stderr, "Memory info:\n");
    if (crash_log) {
        fprintf(crash_log, "Memory info:\n");
    }
    
    // Use platform-independent approach to get memory info
#ifdef __APPLE__
    system("ps -o pid,vsz,rss,command -p $$ 2>/dev/null >> pi_calculator_crash.log || echo 'Memory info not available'");
#else
    system("cat /proc/self/status 2>/dev/null | grep -i 'VmSize\\|VmRSS' >> pi_calculator_crash.log || "
           "ps -o pid,vsz,rss,cmd -p $$ 2>/dev/null >> pi_calculator_crash.log || "
           "echo 'Memory info not available'");
#endif
    
    // Log active jobs information
    fprintf(stderr, "Active job information:\n");
    if (crash_log) {
        fprintf(crash_log, "Active job information:\n");
    }
    
    // Safely access global job data if possible
    if (pthread_mutex_trylock(&jobs_lock) == 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].job_id[0] != '\0' && jobs[i].status == JOB_STATUS_RUNNING) {
                // Log job info
                fprintf(stderr, "  Job ID: %s, Algorithm: %s, Digits: %lu, Progress: %.2f%%\n", 
                        jobs[i].job_id, 
                        jobs[i].algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre", 
                        jobs[i].digits,
                        jobs[i].progress * 100.0);
                
                if (crash_log) {
                    fprintf(crash_log, "  Job ID: %s, Algorithm: %s, Digits: %lu, Progress: %.2f%%\n", 
                            jobs[i].job_id, 
                            jobs[i].algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre", 
                            jobs[i].digits,
                            jobs[i].progress * 100.0);
                }
                
                // Attempt to save a checkpoint for this job
                char checkpoint_path[MAX_PATH];
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/job_%s_crash_checkpoint.json",
                         config.work_dir, jobs[i].job_id);
                
                FILE *checkpoint = fopen(checkpoint_path, "w");
                if (checkpoint) {
                    fprintf(checkpoint, "{\n");
                    fprintf(checkpoint, "  \"job_id\": \"%s\",\n", jobs[i].job_id);
                    fprintf(checkpoint, "  \"algorithm\": \"%s\",\n", 
                            jobs[i].algorithm == ALGO_CHUDNOVSKY ? "CH" : "GL");
                    fprintf(checkpoint, "  \"digits\": %lu,\n", jobs[i].digits);
                    fprintf(checkpoint, "  \"progress\": %.6f,\n", jobs[i].progress);
                    fprintf(checkpoint, "  \"status\": \"crashed\",\n");
                    fprintf(checkpoint, "  \"crash_time\": %ld,\n", (long)now);
                    fprintf(checkpoint, "  \"error\": \"Calculation terminated by segmentation fault\"\n");
                    fprintf(checkpoint, "}\n");
                    fclose(checkpoint);
                    
                    fprintf(stderr, "  Saved crash checkpoint for job %s to %s\n", 
                            jobs[i].job_id, checkpoint_path);
                    if (crash_log) {
                        fprintf(crash_log, "  Saved crash checkpoint for job %s to %s\n", 
                                jobs[i].job_id, checkpoint_path);
                    }
                }
                
                // Update job status if we can safely lock it
                if (pthread_mutex_trylock(&jobs[i].lock) == 0) {
                    jobs[i].status = JOB_STATUS_FAILED;
                    strncpy(jobs[i].error_message, "Calculation terminated by segmentation fault", 
                            sizeof(jobs[i].error_message)-1);
                    jobs[i].error_message[sizeof(jobs[i].error_message)-1] = '\0';
                    pthread_mutex_unlock(&jobs[i].lock);
                }
            }
        }
        pthread_mutex_unlock(&jobs_lock);
    } else {
        fprintf(stderr, "  Could not access job information (mutex locked)\n");
        if (crash_log) {
            fprintf(crash_log, "  Could not access job information (mutex locked)\n");
        }
    }
    
    // Log global state info
    fprintf(stderr, "Global state:\n");
    fprintf(stderr, "  Server running: %s\n", g_server_running ? "Yes" : "No");
    fprintf(stderr, "  Shutdown requested: %s\n", g_shutdown_flag ? "Yes" : "No");
    if (crash_log) {
        fprintf(crash_log, "Global state:\n");
        fprintf(crash_log, "  Server running: %s\n", g_server_running ? "Yes" : "No");
        fprintf(crash_log, "  Shutdown requested: %s\n", g_shutdown_flag ? "Yes" : "No");
    }
    
    // Close crash log if open
    if (crash_log) {
        fprintf(crash_log, "\nEnd of crash log\n");
        fclose(crash_log);
        fprintf(stderr, "Crash information saved to %s\n", crash_log_path);
    }
}

// Structure definition for timeout monitor thread data
struct timeout_monitor_data {
    time_t start_time;
    time_t max_execution_time;
    volatile bool* timed_out_flag;
    int job_idx;
};

// ==========================================================================
// Type Definitions and Data Structures
// ==========================================================================

// Use config_t structure already defined earlier in the code

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
    time_t max_execution_time;  // Maximum execution time in seconds (0 = no limit)
} calc_thread_data_t;

// ==========================================================================
// Disk-Based Integer Implementation
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

// Get chunk file path from base path and chunk index
int get_chunk_path(char *dest, size_t dest_size, const char *base_path, size_t chunk_idx) {
    char component[32];
    snprintf(component, sizeof(component), "chunk_%zu.bin", chunk_idx);
    return safe_path_join(dest, dest_size, base_path, component);
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

// Load a specific chunk into memory
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
        // If the file doesn't exist, initialize it with zeros
        memset(d_int->cache, 0, chunk_size * sizeof(mp_limb_t));
        d_int->cache_chunk_idx = chunk_idx;
        d_int->dirty = true;
        return 0;
    }
    
    // Read chunk data
    size_t read_items = fread(d_int->cache, sizeof(mp_limb_t), chunk_size, f);
    fclose(f);
    
    if (read_items != chunk_size) {
        // Handle read error or partial read
        // Fill the rest with zeros if partial read
        if (read_items < chunk_size) {
            memset((char*)d_int->cache + read_items * sizeof(mp_limb_t), 
                   0, (chunk_size - read_items) * sizeof(mp_limb_t));
        }
    }
    
    d_int->cache_chunk_idx = chunk_idx;
    d_int->dirty = false;
    
    return 0;
}

// Save a specific chunk to disk
int save_chunk(disk_int* d_int, size_t chunk_idx) {
    if (chunk_idx >= d_int->num_chunks) {
        return -1; // Invalid chunk index
    }
    
    // If this chunk is not currently cached, we can't save it
    if (!d_int->cache || d_int->cache_chunk_idx != chunk_idx) {
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
    
    // Construct chunk file path
    char chunk_path[MAX_PATH];
    if (get_chunk_path(chunk_path, MAX_PATH, d_int->file_path, chunk_idx) < 0) {
        return -1;
    }
    
    // Ensure directory exists
    char dir_path[MAX_PATH];
    char* last_slash = strrchr(chunk_path, '/');
    if (last_slash) {
        size_t dir_len = last_slash - chunk_path;
        strncpy(dir_path, chunk_path, dir_len);
        dir_path[dir_len] = '\0';
        
        if (mkdir_recursive(dir_path, 0755) != 0) {
            unified_log(LOG_LEVEL_ERROR, "Error creating directory structure for: %s", chunk_path);
            return -1;
        }
    }
    
    // Open chunk file for writing
    FILE* f = fopen(chunk_path, "wb");
    if (!f) {
        unified_log(LOG_LEVEL_ERROR, "Error opening chunk file for writing: %s", chunk_path);
        return -1;
    }
    
    // Write chunk data
    size_t written = fwrite(d_int->cache, sizeof(mp_limb_t), chunk_size, f);
    fclose(f);
    
    if (written != chunk_size) {
        unified_log(LOG_LEVEL_ERROR, "Error writing chunk data to file: %s", chunk_path);
        return -1;
    }
    
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
    
    // Store the base path for this disk integer
    // We'll create a unique directory for this disk integer's chunks
    static int counter = 0;
    snprintf(d_int->file_path, MAX_PATH, "%s/int_%d_%lu", 
             base_path, (int)getpid(), (unsigned long)counter++);
    printf("DEBUG: Generated base path: %s\n", d_int->file_path);
    
    // Create directory structure for this disk integer
    if (mkdir_recursive(d_int->file_path, 0755) != 0) {
        fprintf(stderr, "Error creating directory structure for: %s\n", d_int->file_path);
        d_int->file_path[0] = '\0';
        return;
    }
    
    // Initialize chunking parameters
    d_int->total_size_in_limbs = 0;
    d_int->sign = 0;
    d_int->chunk_size = determine_optimal_chunk_size(DEFAULT_MEMORY_LIMIT, 0);
    d_int->num_chunks = 0;
    d_int->cache = NULL;
    d_int->cache_chunk_idx = 0;
    d_int->dirty = false;
    
    pthread_mutex_init(&d_int->lock, NULL);
    
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
    if (!d_int || d_int->file_path[0] == '\0') {
        fprintf(stderr, "Error: Disk integer not properly initialized\n");
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
        
        // Extract the chunk from mpz_t
        for (size_t i = 0; i < chunk_size; i++) {
            size_t limb_idx = chunk_start + i;
            if (limb_idx < total_size) {
                ((mp_limb_t*)d_int->cache)[i] = mpz_getlimbn(mpz_val, limb_idx);
            } else {
                // Should not happen, but just in case
                ((mp_limb_t*)d_int->cache)[i] = 0;
            }
        }
        
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
        if (load_chunk(d_int, chunk_idx) != 0) {
            fprintf(stderr, "Error: Failed to load chunk %zu in disk_int_get_mpz\n", chunk_idx);
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
    
    // Set the mpz_t value
    mpz_import(mpz_val, d_int->total_size_in_limbs, -1, sizeof(mp_limb_t), 0, 0, limbs);
    
    // Set the correct sign
    if (d_int->sign < 0) {
        mpz_neg(mpz_val, mpz_val);
    }
    
    free(limbs);
    
    pthread_mutex_unlock(&d_int->lock);
}

// Compare the absolute values of two disk integers
// Returns: 1 if |a| > |b|, 0 if |a| == |b|, -1 if |a| < |b|
int disk_int_cmp_abs(disk_int* a, disk_int* b) {
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
        // Calculate offset within the chunk
        size_t offset = most_sig_limb - (mp_limb_t*)result->cache;
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
    // Handle cases with zero
    if (a->total_size_in_limbs == 0 || a->sign == 0) {
        // a is zero, result = b
        mpz_t mpz_b;
        mpz_init(mpz_b);
        disk_int_get_mpz(mpz_b, b);
        disk_int_set_mpz(result, mpz_b);
        mpz_clear(mpz_b);
        return;
    }
    
    if (b->total_size_in_limbs == 0 || b->sign == 0) {
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
    if ((a->total_size_in_limbs == 0 || a->sign == 0) || 
        (b->total_size_in_limbs == 0 || b->sign == 0)) {
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
        
        // Use a flag to track errors for proper cleanup
        int error_occurred = 0;
        
        // P = (6*b - 5) * (2*b - 1) * (6*b - 1)
        mpz_set_ui(t1, 6 * b - 5);
        mpz_set_ui(t2, 2 * b - 1);
        mpz_mul(t3, t1, t2);
        mpz_set_ui(t1, 6 * b - 1);
        mpz_mul(P, t3, t1);
        
        // Check for errors in computation
        if (mpz_sgn(P) == 0) {
            fprintf(stderr, "Warning: Zero P value in base case of binary_split_mpz\n");
            mpz_set_ui(P, 1);
            error_occurred = 1;
        }
        
        // Q = b^3 * C^3 / 24
        mpz_set_ui(t1, b);
        mpz_pow_ui(t2, t1, 3);
        // Fix: Use string constant for C3_OVER_24 instead of overflowing macro
        if (mpz_set_str(t1, C3_OVER_24_STR, 10) != 0) {
            fprintf(stderr, "Error: Failed to set C3_OVER_24_STR in binary_split_mpz\n");
            error_occurred = 1;
            mpz_set_ui(t1, 1);  // Set a safe default
        }
        mpz_mul(Q, t2, t1);
        
        // Check for errors in computation
        if (mpz_sgn(Q) == 0) {
            fprintf(stderr, "Warning: Zero Q value in base case of binary_split_mpz\n");
            mpz_set_ui(Q, 1);
            error_occurred = 1;
        }
        
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
            if (p_str && q_str && t_str) {
                printf("Base case [%lu,%lu]: P=%s, Q=%s, T=%s\n", a, b, p_str, q_str, t_str);
                free(p_str);
                free(q_str);
                free(t_str);
            } else {
                fprintf(stderr, "Error: Failed to allocate string memory in binary_split_mpz\n");
                error_occurred = 1;
            }
        }
        
        // Always clean up temporary variables
        mpz_clear(t1);
        mpz_clear(t2);
        mpz_clear(t3);
        
        // If an error occurred, make sure we have safe values
        if (error_occurred) {
            if (mpz_sgn(P) == 0) mpz_set_ui(P, 1);
            if (mpz_sgn(Q) == 0) mpz_set_ui(Q, 1);
            // T can be zero, no need to check
        }
    } else {
        // Recursive case
        unsigned long m = (a + b) / 2;
        mpz_t P1, Q1, T1, P2, Q2, T2, tmp1, tmp2;
        int error_occurred = 0;
        
        // Initialize all variables first to ensure we can clean up properly
        mpz_init(P1);
        mpz_init(Q1);
        mpz_init(T1);
        mpz_init(P2);
        mpz_init(Q2);
        mpz_init(T2);
        mpz_init(tmp1);
        mpz_init(tmp2);
        
        // Initialize with safe defaults in case recursion fails
        mpz_set_ui(P1, 1);
        mpz_set_ui(Q1, 1);
        mpz_set_ui(T1, 0);
        mpz_set_ui(P2, 1);
        mpz_set_ui(Q2, 1);
        mpz_set_ui(T2, 0);
        
        // Compute left half
        binary_split_mpz(P1, Q1, T1, a, m);
        
        // Check for errors in the left half - properly handle error path
        if (mpz_sgn(P1) == 0) {
            fprintf(stderr, "Warning: Zero P1 value after left recursion in binary_split_mpz [%lu,%lu]\n", a, m);
            mpz_set_ui(P1, 1);
            error_occurred = 1;
        }
        if (mpz_sgn(Q1) == 0) {
            fprintf(stderr, "Warning: Zero Q1 value after left recursion in binary_split_mpz [%lu,%lu]\n", a, m);
            mpz_set_ui(Q1, 1);
            error_occurred = 1;
        }
        
        // Compute right half
        binary_split_mpz(P2, Q2, T2, m, b);
        
        // Check for errors in the right half - properly handle error path
        if (mpz_sgn(P2) == 0) {
            fprintf(stderr, "Warning: Zero P2 value after right recursion in binary_split_mpz [%lu,%lu]\n", m, b);
            mpz_set_ui(P2, 1);
            error_occurred = 1;
        }
        if (mpz_sgn(Q2) == 0) {
            fprintf(stderr, "Warning: Zero Q2 value after right recursion in binary_split_mpz [%lu,%lu]\n", m, b);
            mpz_set_ui(Q2, 1);
            error_occurred = 1;
        }
        
        // Combine results: P = P1 * P2, Q = Q1 * Q2, T = T1 * Q2 + T2 * P1
        // Use try/catch pattern with goto for error handling
        mpz_mul(P, P1, P2);
        if (mpz_sgn(P) == 0) {
            fprintf(stderr, "Warning: Combined P is zero in binary_split_mpz [%lu,%lu]\n", a, b);
            mpz_set_ui(P, 1);
            error_occurred = 1;
        }
        
        mpz_mul(Q, Q1, Q2);
        if (mpz_sgn(Q) == 0) {
            fprintf(stderr, "Warning: Combined Q is zero in binary_split_mpz [%lu,%lu]\n", a, b);
            mpz_set_ui(Q, 1);
            error_occurred = 1;
        }
        
        mpz_mul(tmp1, T1, Q2);
        mpz_mul(tmp2, T2, P1);
        mpz_add(T, tmp1, tmp2);
        
        // Debug output for small values
        if (b < 5) {
            char *p_str = mpz_get_str(NULL, 10, P);
            char *q_str = mpz_get_str(NULL, 10, Q);
            char *t_str = mpz_get_str(NULL, 10, T);
            if (p_str && q_str && t_str) {
                printf("Recursive case [%lu,%lu]: P=%s, Q=%s, T=%s\n", a, b, p_str, q_str, t_str);
                free(p_str);
                free(q_str);
                free(t_str);
            } else {
                fprintf(stderr, "Error: Failed to allocate string memory in binary_split_mpz\n");
                error_occurred = 1;
            }
        }
        
        // Clean up all mpz_t variables even if an error occurred
        mpz_clear(P1);
        mpz_clear(Q1);
        mpz_clear(T1);
        mpz_clear(P2);
        mpz_clear(Q2);
        mpz_clear(T2);
        mpz_clear(tmp1);
        mpz_clear(tmp2);
        
        // Set safe default values if an error occurred
        if (error_occurred) {
            if (mpz_sgn(P) == 0) mpz_set_ui(P, 1);
            if (mpz_sgn(Q) == 0) mpz_set_ui(Q, 1);
            // T can be zero
        }
    }
    
    // Sanity check final results
    if (mpz_sgn(P) == 0) {
        fprintf(stderr, "Warning: Result P is zero in binary_split_mpz [%lu,%lu]\n", a, b);
        mpz_set_ui(P, 1);
    }
    
    if (mpz_sgn(Q) == 0) {
        fprintf(stderr, "Warning: Result Q is zero in binary_split_mpz [%lu,%lu]\n", a, b);
        mpz_set_ui(Q, 1);
    }
}

// Binary splitting on disk for large ranges
void binary_split_disk(chudnovsky_state* result, unsigned long a, unsigned long b, 
                       const char* base_path, size_t memory_threshold) {
    // Track created directories to ensure proper cleanup
    bool left_dir_created = false;
    bool right_dir_created = false;
    bool temp1_dir_created = false;
    bool temp2_dir_created = false;
    char left_path[MAX_PATH] = {0};
    char right_path[MAX_PATH] = {0};
    char temp1_path[MAX_PATH] = {0};
    char temp2_path[MAX_PATH] = {0};
    bool left_state_initialized = false;
    bool right_state_initialized = false;
    bool temp1_initialized = false;
    bool temp2_initialized = false;
    
    if (config.log_level <= LOG_LEVEL_DEBUG) {
        printf("DEBUG: binary_split_disk a=%lu, b=%lu, base_path=%s\n", a, b, base_path ? base_path : "NULL");
    }
    
    // Safety check for invalid range
    if (a >= b) {
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
    if (config.log_level <= LOG_LEVEL_DEBUG) {
        printf("DEBUG: Range check b-a=%lu, threshold=%zu\n", b-a, memory_threshold);
    }
    
    if (b - a <= memory_threshold) {
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Computing in memory\n");
        }
        
        mpz_t P, Q, T;
        mpz_init(P);
        mpz_init(Q);
        mpz_init(T);
        
        binary_split_mpz(P, Q, T, a, b);
        
        // Debug output for small ranges
        if (b - a < 5 && config.log_level <= LOG_LEVEL_DEBUG) {
            char *p_str = mpz_get_str(NULL, 10, P);
            char *q_str = mpz_get_str(NULL, 10, Q);
            char *t_str = mpz_get_str(NULL, 10, T);
            if (p_str && q_str && t_str) {
                printf("Binary split disk [%lu,%lu]: P=%s, Q=%s, T=%s\n", a, b, p_str, q_str, t_str);
                free(p_str);
                free(q_str);
                free(t_str);
            }
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
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Splitting range at m=%lu\n", m);
        }
        
        // Create paths for left and right results
        snprintf(left_path, MAX_PATH, "%s/left", base_path);
        snprintf(right_path, MAX_PATH, "%s/right", base_path);
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: left_path=%s, right_path=%s\n", left_path, right_path);
        }
        
        // Create directories with error checking
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Creating directory: %s\n", left_path);
        }
        
        if (mkdir_recursive(left_path, 0755) == 0) {
            left_dir_created = true;
        } else {
            fprintf(stderr, "Warning: Failed to create directory: %s (%s)\n", 
                    left_path, strerror(errno));
            goto cleanup;
        }
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Creating directory: %s\n", right_path);
        }
        
        if (mkdir_recursive(right_path, 0755) == 0) {
            right_dir_created = true;
        } else {
            fprintf(stderr, "Warning: Failed to create directory: %s (%s)\n", 
                    right_path, strerror(errno));
            goto cleanup;
        }
        
        // Compute left and right halves
        chudnovsky_state left_state, right_state;
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Initializing left_state with path: %s\n", left_path);
        }
        
        chudnovsky_state_init(&left_state, left_path);
        left_state_initialized = true;
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Initializing right_state with path: %s\n", right_path);
        }
        
        chudnovsky_state_init(&right_state, right_path);
        right_state_initialized = true;
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Recursively calling binary_split_disk for left half\n");
        }
        
        binary_split_disk(&left_state, a, m, left_path, memory_threshold);
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Recursively calling binary_split_disk for right half\n");
        }
        
        binary_split_disk(&right_state, m, b, right_path, memory_threshold);
        
        // Create temp paths
        snprintf(temp1_path, MAX_PATH, "%s/temp1", base_path);
        snprintf(temp2_path, MAX_PATH, "%s/temp2", base_path);
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: temp1_path=%s, temp2_path=%s\n", temp1_path, temp2_path);
        }
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Creating directory: %s\n", temp1_path);
        }
        
        if (mkdir_recursive(temp1_path, 0755) == 0) {
            temp1_dir_created = true;
        } else {
            fprintf(stderr, "Warning: Failed to create directory: %s (%s)\n", 
                    temp1_path, strerror(errno));
            goto cleanup;
        }
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Creating directory: %s\n", temp2_path);
        }
        
        if (mkdir_recursive(temp2_path, 0755) == 0) {
            temp2_dir_created = true;
        } else {
            fprintf(stderr, "Warning: Failed to create directory: %s (%s)\n", 
                    temp2_path, strerror(errno));
            goto cleanup;
        }
        
        // Initialize temporary disk integers
        disk_int temp1, temp2;
        disk_int_init(&temp1, temp1_path);
        temp1_initialized = true;
        
        disk_int_init(&temp2, temp2_path);
        temp2_initialized = true;
        
        // Combine results using disk operations
        // P = P1 * P2
        disk_int_mul(&result->P, &left_state.P, &right_state.P);
        
        // Q = Q1 * Q2
        disk_int_mul(&result->Q, &left_state.Q, &right_state.Q);
        
        // T = T1 * Q2 + T2 * P1
        disk_int_mul(&temp1, &left_state.T, &right_state.Q);
        disk_int_mul(&temp2, &right_state.T, &left_state.P);
        disk_int_add(&result->T, &temp1, &temp2);
        
    cleanup:
        // Clean up resources in reverse order of creation
        
        // Clean up temporary disk integers
        if (temp2_initialized) {
            disk_int_clear(&temp2);
        }
        
        if (temp1_initialized) {
            disk_int_clear(&temp1);
        }
        
        // Clean up the states which will remove their files
        if (right_state_initialized) {
            chudnovsky_state_clear(&right_state);
        }
        
        if (left_state_initialized) {
            chudnovsky_state_clear(&left_state);
        }
        
        // Force removal of any files that might be left in the directories
        if (temp2_dir_created) {
            // Remove any leftover files in temp2 directory
            char cleanup_cmd[MAX_PATH * 2];
            snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -f %s/*", temp2_path);
            system(cleanup_cmd);
            
            // Now try to remove the directory
            if (rmdir(temp2_path) != 0) {
                fprintf(stderr, "Warning: Failed to remove temp2 directory: %s (%s)\n", 
                        temp2_path, strerror(errno));
            }
        }
        
        if (temp1_dir_created) {
            // Remove any leftover files in temp1 directory
            char cleanup_cmd[MAX_PATH * 2];
            snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -f %s/*", temp1_path);
            system(cleanup_cmd);
            
            // Now try to remove the directory
            if (rmdir(temp1_path) != 0) {
                fprintf(stderr, "Warning: Failed to remove temp1 directory: %s (%s)\n", 
                        temp1_path, strerror(errno));
            }
        }
        
        if (right_dir_created) {
            // Remove any leftover files in right directory
            char cleanup_cmd[MAX_PATH * 2];
            snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -f %s/*", right_path);
            system(cleanup_cmd);
            
            // Now try to remove the directory
            if (rmdir(right_path) != 0) {
                fprintf(stderr, "Warning: Failed to remove right directory: %s (%s)\n", 
                        right_path, strerror(errno));
            }
        }
        
        if (left_dir_created) {
            // Remove any leftover files in left directory
            char cleanup_cmd[MAX_PATH * 2];
            snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -f %s/*", left_path);
            system(cleanup_cmd);
            
            // Now try to remove the directory
            if (rmdir(left_path) != 0) {
                fprintf(stderr, "Warning: Failed to remove left directory: %s (%s)\n", 
                        left_path, strerror(errno));
            }
        }
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

// ==========================================================================
// Job Management for Asynchronous Calculations
// ==========================================================================

//
// Below, we use the previously defined calculation_job structure
//

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
    
    // For small digit counts, use a simplified in-memory approach with binary splitting
    if (state->digits <= 1000) {
        printf("Using in-memory binary splitting approach for small digit count...\n");
        
        // Calculate enough terms for the desired precision
        unsigned long terms = (unsigned long)ceil(state->digits / 14.0) + 1;
        printf("Will compute %lu terms using binary splitting\n", terms);
        
        // Initialize variables for binary splitting
        mpz_t P, Q, T;
        mpz_init(P);
        mpz_init(Q);
        mpz_init(T);
        
        // Perform binary splitting algorithm (in memory)
        binary_split_mpz(P, Q, T, 0, terms);
        
        printf("Binary splitting complete\n");
        printf("P size: %zu digits\n", mpz_sizeinbase(P, 10));
        printf("Q size: %zu digits\n", mpz_sizeinbase(Q, 10));
        printf("T size: %zu digits\n", mpz_sizeinbase(T, 10));
        
        // Compute pi using the correct Chudnovsky formula: pi = (Q * C^(3/2)) / (12 * T)
        mpfr_t pi, temp_sqrt, temp_div;
        mpfr_prec_t precision = (mpfr_prec_t)(state->digits * 4);
        
        // Initialize MPFR variables
        mpfr_init2(pi, precision);          // Final pi value
        mpfr_init2(temp_sqrt, precision);   // For C^(3/2)
        mpfr_init2(temp_div, precision);    // For 12*T
        
        // Initialize constants
        mpfr_t mpfr_C, mpfr_D;
        mpfr_init2(mpfr_C, precision);  // C = 640320
        mpfr_init2(mpfr_D, precision);  // D = 12
        
        // Set constant values
        mpfr_set_ui(mpfr_C, C, MPFR_RNDN);
        mpfr_set_ui(mpfr_D, 12, MPFR_RNDN);
        
        // Calculate C^(3/2)
        mpfr_sqrt(temp_sqrt, mpfr_C, MPFR_RNDN);          // sqrt(C)
        mpfr_mul(temp_sqrt, temp_sqrt, mpfr_C, MPFR_RNDN); // C^(3/2)
        
        // Convert Q to mpfr and multiply by C^(3/2)
        mpfr_set_z(pi, Q, MPFR_RNDN);                     // pi = Q
        mpfr_mul(pi, pi, temp_sqrt, MPFR_RNDN);           // pi = Q * C^(3/2)
        
        // Convert T to mpfr and multiply by 12
        mpfr_set_z(temp_div, T, MPFR_RNDN);               // temp_div = T
        mpfr_mul(temp_div, temp_div, mpfr_D, MPFR_RNDN);  // temp_div = 12 * T
        
        // Final division: pi = (Q * C^(3/2)) / (12 * T)
        mpfr_div(pi, pi, temp_div, MPFR_RNDN);
        
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
        
        // Clean up variables
        mpz_clear(P);
        mpz_clear(Q);
        mpz_clear(T);
        mpfr_clear(pi);
        mpfr_clear(temp_sqrt);
        mpfr_clear(temp_div);
        mpfr_clear(mpfr_C);
        mpfr_clear(mpfr_D);
        
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
    
    // Pi = Q * C^(3/2) / (12 * T)  - Correct Chudnovsky formula with factor of 12
    mpz_t mpz_P, mpz_Q, mpz_T;
    mpfr_t mpfr_pi, mpfr_C, mpfr_temp, mpfr_D;
    
    mpz_init(mpz_P);
    mpz_init(mpz_Q);
    mpz_init(mpz_T);
    
    // Determine required precision
    mpfr_prec_t precision = (mpfr_prec_t)(state->digits * 4);
    
    mpfr_init2(mpfr_pi, precision);
    mpfr_init2(mpfr_C, precision);
    mpfr_init2(mpfr_temp, precision);
    mpfr_init2(mpfr_D, precision);
    
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
    
    // D = 12 (Chudnovsky algorithm constant)
    mpfr_set_ui(mpfr_D, 12, MPFR_RNDN);
    
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
    
    // Convert T to mpfr and multiply by D=12
    mpfr_set_z(mpfr_temp, mpz_T, MPFR_RNDN);
    mpfr_mul(mpfr_temp, mpfr_temp, mpfr_D, MPFR_RNDN);  // 12 * T
    
    // pi = pi / (12 * T)
    mpfr_div(mpfr_pi, mpfr_pi, mpfr_temp, MPFR_RNDN);
    
    // Debug output
    mpfr_printf("Final pi value: %.10Rf\n", mpfr_pi);
    
    // Clean up
    mpz_clear(mpz_P);
    mpz_clear(mpz_Q);
    mpz_clear(mpz_T);
    mpfr_clear(mpfr_C);
    mpfr_clear(mpfr_D);
    
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
                // Got invalid result, generate a detailed error report
                time_t now = time(NULL);
                char time_str[32];
                struct tm *tm_info = localtime(&now);
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
                
                fprintf(f, "ERROR: Pi calculation failed to produce valid result\n\n");
                fprintf(f, "Diagnostic Information:\n");
                fprintf(f, "  Timestamp: %s\n", time_str);
                fprintf(f, "  Algorithm: %s\n", state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre");
                fprintf(f, "  Requested digits: %lu\n", state->digits);
                fprintf(f, "  Out-of-core mode: %s\n", state->out_of_core ? "Yes" : "No");
                fprintf(f, "  Invalid result value: '%s'\n", str_pi);
                fprintf(f, "  Working directory: %s\n", state->work_dir);
                fprintf(f, "  Memory allocation check: %s\n", mpfr_mp_memory_cleanup() ? "Leaks detected" : "No leaks detected");
                
                // Log additional error info
                fprintf(stderr, "Error: Pi calculation failed to produce valid result at %s\n", time_str);
                fprintf(stderr, "  Algorithm: %s, Digits: %lu, Result: '%s'\n", 
                        state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre",
                        state->digits, str_pi);
                
                // Create error report file for diagnostics
                char error_file[MAX_PATH];
                snprintf(error_file, sizeof(error_file), "%s/error_report_%ld.json", 
                         state->work_dir, (long)now);
                
                FILE* error_report = fopen(error_file, "w");
                if (error_report) {
                    fprintf(error_report, "{\n");
                    fprintf(error_report, "  \"error\": \"Pi calculation failed to produce valid result\",\n");
                    fprintf(error_report, "  \"timestamp\": %ld,\n", (long)now);
                    fprintf(error_report, "  \"algorithm\": \"%s\",\n", 
                            state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre");
                    fprintf(error_report, "  \"digits\": %lu,\n", state->digits);
                    fprintf(error_report, "  \"out_of_core\": %s,\n", state->out_of_core ? "true" : "false");
                    fprintf(error_report, "  \"invalid_result\": \"%s\",\n", str_pi);
                    fprintf(error_report, "  \"work_dir\": \"%s\",\n", state->work_dir);
                    fprintf(error_report, "  \"job_id\": %d\n", state->job_idx);
                    fprintf(error_report, "}\n");
                    fclose(error_report);
                    
                    fprintf(stderr, "  Error report saved to: %s\n", error_file);
                }
                
                // Update job status if this is part of a job
                if (state->job_idx >= 0) {
                    char error_message[MAX_JOB_ERROR_MSG];
                    snprintf(error_message, sizeof(error_message), 
                             "Calculation failed: Invalid result produced ('%s')", str_pi);
                    update_job_status(state->job_idx, JOB_STATUS_FAILED, 1.0, error_message);
                }
            }
            
            // Free the string allocated by mpfr_get_str
            mpfr_free_str(str_pi);
        } else {
            // Handle NULL result from mpfr_get_str with detailed error report
            time_t now = time(NULL);
            char time_str[32];
            struct tm *tm_info = localtime(&now);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            
            fprintf(f, "ERROR: Pi calculation failed (mpfr_get_str returned NULL)\n\n");
            fprintf(f, "Diagnostic Information:\n");
            fprintf(f, "  Timestamp: %s\n", time_str);
            fprintf(f, "  Algorithm: %s\n", state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre");
            fprintf(f, "  Requested digits: %lu\n", state->digits);
            fprintf(f, "  Out-of-core mode: %s\n", state->out_of_core ? "Yes" : "No");
            fprintf(f, "  Working directory: %s\n", state->work_dir);
            fprintf(f, "  MPFR version: %s\n", mpfr_get_version());
            fprintf(f, "  Memory allocation check: %s\n", mpfr_mp_memory_cleanup() ? "Leaks detected" : "No leaks detected");
            
            // Log error details
            fprintf(stderr, "Error: mpfr_get_str returned NULL in Pi calculation at %s\n", time_str);
            fprintf(stderr, "  Algorithm: %s, Digits: %lu\n", 
                    state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre",
                    state->digits);
            
            // Create error report file for diagnostics
            char error_file[MAX_PATH];
            snprintf(error_file, sizeof(error_file), "%s/error_report_null_%ld.json", 
                     state->work_dir, (long)now);
            
            FILE* error_report = fopen(error_file, "w");
            if (error_report) {
                fprintf(error_report, "{\n");
                fprintf(error_report, "  \"error\": \"Pi calculation failed (mpfr_get_str returned NULL)\",\n");
                fprintf(error_report, "  \"timestamp\": %ld,\n", (long)now);
                fprintf(error_report, "  \"algorithm\": \"%s\",\n", 
                        state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre");
                fprintf(error_report, "  \"digits\": %lu,\n", state->digits);
                fprintf(error_report, "  \"out_of_core\": %s,\n", state->out_of_core ? "true" : "false");
                fprintf(error_report, "  \"work_dir\": \"%s\",\n", state->work_dir);
                fprintf(error_report, "  \"mpfr_version\": \"%s\",\n", mpfr_get_version());
                fprintf(error_report, "  \"job_id\": %d\n", state->job_idx);
                fprintf(error_report, "}\n");
                fclose(error_report);
                
                fprintf(stderr, "  Error report saved to: %s\n", error_file);
            }
            
            // Update job status if this is part of a job
            if (state->job_idx >= 0) {
                char error_message[MAX_JOB_ERROR_MSG];
                snprintf(error_message, sizeof(error_message), 
                         "Calculation failed: mpfr_get_str returned NULL, error report: %s", 
                         error_file);
                update_job_status(state->job_idx, JOB_STATUS_FAILED, 1.0, error_message);
            }
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

// Unified logging function that works in both server and CLI modes
void unified_log(int level, const char* format, ...) {
    // Get level string representation
    const char *level_str;
    switch (level) {
        case LOG_LEVEL_DEBUG:
            level_str = "debug";
            break;
        case LOG_LEVEL_INFO:
            level_str = "info";
            break;
        case LOG_LEVEL_WARNING:
            level_str = "warning";
            break;
        case LOG_LEVEL_ERROR:
            level_str = "error";
            break;
        case LOG_LEVEL_CRITICAL:
            level_str = "critical";
            break;
        default:
            level_str = "unknown";
    }
    
    // Get current time
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[20];
    strftime(timestamp, 20, "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Format the variadic message
    char formatted_message[BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(formatted_message, BUFFER_SIZE, format, args);
    va_end(args);
    
    // Create log message with format: [timestamp] level: message
    char log_output[BUFFER_SIZE];
    snprintf(log_output, BUFFER_SIZE, "[%s] %s: %s", timestamp, level_str, formatted_message);
    
    // Check if in server mode and logging is initialized
    if (config.mode == MODE_SERVER && log_file) {
        // Output to log file in server mode
        fprintf(log_file, "%s\n", log_output);
        fflush(log_file);
        
        // Also output critical and errors to stderr
        if (level >= LOG_LEVEL_ERROR) {
            fprintf(stderr, "%s\n", log_output);
        }
    } else {
        // In CLI mode or early initialization, output to stderr
        // Only output if the log level is at or above the configured level
        if (level >= config.log_level) {
            fprintf(stderr, "%s\n", log_output);
        }
    }
}

// Log message with timestamp and level (supports printf-style formats)
// Legacy function for backward compatibility
void log_message(const char* level, const char* format, ...) {
    // Convert level string to int
    int int_level;
    if (strcasecmp(level, "debug") == 0) {
        int_level = LOG_LEVEL_DEBUG;
    } else if (strcasecmp(level, "info") == 0) {
        int_level = LOG_LEVEL_INFO;
    } else if (strcasecmp(level, "warning") == 0) {
        int_level = LOG_LEVEL_WARNING;
    } else if (strcasecmp(level, "error") == 0) {
        int_level = LOG_LEVEL_ERROR;
    } else if (strcasecmp(level, "critical") == 0) {
        int_level = LOG_LEVEL_CRITICAL;
    } else {
        int_level = LOG_LEVEL_INFO; // Default to info
    }
    
    // Forward to unified_log
    va_list args;
    va_start(args, format);
    char formatted_message[BUFFER_SIZE];
    vsnprintf(formatted_message, BUFFER_SIZE, format, args);
    va_end(args);
    
    unified_log(int_level, "%s", formatted_message);
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

// Global state flags
bool g_server_running = false;  // Server active flag
volatile bool g_shutdown_flag = false;   // Shutdown requested flag

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
    // Initialize logging level
    config.log_level = LOG_LEVEL_INFO;
    strncpy(config.logging_output, "console", sizeof(config.logging_output));
    config.logging.level = LOG_LEVEL_INFO; // Set default integer level
    
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
            if (level) {
                // Set log level from command line argument
                // Also set the integer value
                config.log_level = convert_log_level_string_to_int(level);
            }
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

// Convert string log level to integer log level
int convert_log_level_string_to_int(const char* level) {
    if (!level) return LOG_LEVEL_INFO; // Default

    if (strcasecmp(level, "debug") == 0) {
        return LOG_LEVEL_DEBUG;
    } else if (strcasecmp(level, "info") == 0) {
        return LOG_LEVEL_INFO;
    } else if (strcasecmp(level, "error") == 0) {
        return LOG_LEVEL_ERROR;
    }
    
    // Default to INFO if unknown
    return LOG_LEVEL_INFO;
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
    
    // Make sure the string log level is converted to its integer equivalent
    config.log_level = convert_log_level_string_to_int(config.logging_level);
    
    // If non-option arguments exist and in server mode, switch to CLI mode
    if (optind < argc && config.mode == MODE_SERVER) {
        config.mode = MODE_CLI;
        
        // Non-option argument is the digit count
        config.max_digits = strtoul(argv[optind], NULL, 10);
    }
}

// Thread function to monitor calculation timeout
void* timeout_monitor_thread(void* arg) {
    // Make thread cancelable
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    
    // Extract the timeout data
    struct timeout_monitor_data* data = (struct timeout_monitor_data*)arg;
    
    if (!data) {
        fprintf(stderr, "Error: Null argument passed to timeout_monitor_thread\n");
        return NULL;
    }
    
    // Copy the data to local variables
    time_t start_time = data->start_time;
    time_t max_execution_time = data->max_execution_time;
    volatile bool* timed_out_flag = data->timed_out_flag;
    int job_idx = data->job_idx;
    
    // Free the data structure since we've copied what we need
    free(data);
    data = NULL;  // Avoid use-after-free
    
    // Calculate the end time
    time_t end_time = start_time + max_execution_time;
    
    // Log the timeout information
    char timeout_msg[256];
    char *timestr = ctime(&end_time);
    if (timestr) {
        timestr[strlen(timestr)-1] = '\0';  // Remove trailing newline
        snprintf(timeout_msg, sizeof(timeout_msg), 
                "Timeout monitor started for job %d: %ld seconds (until %s)", 
                job_idx, max_execution_time, timestr);
    } else {
        snprintf(timeout_msg, sizeof(timeout_msg), 
                "Timeout monitor started for job %d: %ld seconds", 
                job_idx, max_execution_time);
    }
    log_message("debug", "%s", timeout_msg);
    
    // Get job info for better logging
    unsigned long digits = 0;
    algorithm_t algorithm = ALGO_GAUSS_LEGENDRE;
    
    pthread_mutex_lock(&jobs_lock);
    if (job_idx >= 0 && job_idx < MAX_JOBS && jobs[job_idx].job_id[0] != '\0') {
        pthread_mutex_lock(&jobs[job_idx].lock);
        digits = jobs[job_idx].digits;
        algorithm = jobs[job_idx].algorithm;
        pthread_mutex_unlock(&jobs[job_idx].lock);
    }
    pthread_mutex_unlock(&jobs_lock);
    
    // Loop until the timeout is reached, checking every second
    while (time(NULL) < end_time) {
        // Sleep for a short time to reduce CPU usage
        sleep(1);
        
        // Check if the thread should be canceled
        pthread_testcancel();
        
        // Check if job is already done (no need for timeout anymore)
        bool job_completed = false;
        
        pthread_mutex_lock(&jobs_lock);
        if (job_idx >= 0 && job_idx < MAX_JOBS && jobs[job_idx].job_id[0] != '\0') {
            pthread_mutex_lock(&jobs[job_idx].lock);
            job_completed = (jobs[job_idx].status == JOB_STATUS_COMPLETED || 
                           jobs[job_idx].status == JOB_STATUS_FAILED);
            pthread_mutex_unlock(&jobs[job_idx].lock);
        }
        pthread_mutex_unlock(&jobs_lock);
        
        if (job_completed) {
            log_message("debug", "Job %d already completed, exiting timeout monitor", job_idx);
            return NULL;
        }
    }
    
    // If we get here, the timeout has been reached
    char algorithm_str[3] = "??";
    if (algorithm == ALGO_CHUDNOVSKY) {
        strcpy(algorithm_str, "CH");
    } else if (algorithm == ALGO_GAUSS_LEGENDRE) {
        strcpy(algorithm_str, "GL");
    }
    
    log_message("warning", "Calculation timed out after %ld seconds for job %d (%s, %lu digits)", 
               max_execution_time, job_idx, algorithm_str, digits);
    
    fprintf(stderr, "Calculation timed out after %ld seconds for job %d (%s, %lu digits)\n", 
            max_execution_time, job_idx, algorithm_str, digits);
    
    // Create a timeout report file
    char timeout_file[MAX_PATH];
    snprintf(timeout_file, sizeof(timeout_file), "%s/job_%d_timeout.json", 
             config.work_dir, job_idx);
    
    FILE* f = fopen(timeout_file, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"job_id\": \"%d\",\n", job_idx);
        fprintf(f, "  \"timeout\": %ld,\n", max_execution_time);
        fprintf(f, "  \"algorithm\": \"%s\",\n", algorithm_str);
        fprintf(f, "  \"digits\": %lu,\n", digits);
        fprintf(f, "  \"start_time\": %ld,\n", (long)start_time);
        fprintf(f, "  \"end_time\": %ld,\n", (long)end_time);
        fprintf(f, "  \"actual_time\": %ld\n", (long)time(NULL));
        fprintf(f, "}\n");
        fclose(f);
    }
    
    // Set the timed out flag to true - use atomic operation to ensure visibility
    __atomic_store_n(timed_out_flag, true, __ATOMIC_SEQ_CST);
    
    // Update the job status
    char error_message[256];
    snprintf(error_message, sizeof(error_message), 
             "Calculation timed out after %ld seconds (%s, %lu digits)", 
             max_execution_time, algorithm_str, digits);
    
    update_job_status(job_idx, JOB_STATUS_FAILED, 1.0, error_message);
    
    return NULL;
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
    // Initialize resources to NULL so we can safely clean up on error paths
    calc_thread_data_t* data = NULL;
    char* work_dir = NULL;
    pthread_t timeout_thread;
    bool timeout_thread_created = false;
    struct timeout_monitor_data* timeout_data = NULL;
    calculation_state state = {0};
    calc_thread_pool pool = {0};
    bool pool_initialized = false;
    bool state_initialized = false;
    int result = 0;
    
    // Check for null argument
    if (!arg) {
        fprintf(stderr, "Error: Null argument passed to calculation_thread_func\n");
        result = -1;
        goto cleanup;
    }
    
    // Extract data safely
    data = (calc_thread_data_t*)arg;
    int job_idx = data->job_idx;
    time_t max_execution_time = data->max_execution_time;
    time_t start_time = time(NULL);
    
    // Validate job index
    if (job_idx < 0 || job_idx >= MAX_JOBS) {
        fprintf(stderr, "Error: Invalid job index %d in calculation_thread_func\n", job_idx);
        result = -1;
        goto cleanup;
    }
    
    // Get job info - use a local copy to minimize lock time
    algorithm_t algorithm;
    unsigned long digits;
    out_of_core_mode_t out_of_core_mode;
    bool checkpointing_enabled;
    char job_id[37]; // UUID is 36 chars + null
    
    pthread_mutex_lock(&jobs[job_idx].lock);
    algorithm = jobs[job_idx].algorithm;
    digits = jobs[job_idx].digits;
    out_of_core_mode = jobs[job_idx].out_of_core_mode;
    checkpointing_enabled = jobs[job_idx].checkpointing_enabled;
    strncpy(job_id, jobs[job_idx].job_id, sizeof(job_id));
    job_id[sizeof(job_id) - 1] = '\0'; // Ensure null termination
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
    work_dir = (char*)malloc(MAX_PATH);
    if (!work_dir) {
        fprintf(stderr, "Error: Failed to allocate memory for work directory path\n");
        update_job_status(job_idx, JOB_STATUS_FAILED, 0.0, "Memory allocation failure");
        result = -1;
        goto cleanup;
    }
    
    char job_component[64]; // UUID is 36 chars + "job_" prefix
    snprintf(job_component, sizeof(job_component), "job_%s", job_id);
    if (safe_path_join(work_dir, MAX_PATH, config.work_dir, job_component) < 0) {
        fprintf(stderr, "Error: Path too long for job directory\n");
        update_job_status(job_idx, JOB_STATUS_FAILED, 0.0, "Path too long for job directory");
        result = -1;
        goto cleanup;
    }
    
    // Create the directory, check for errors
    if (mkdir_recursive(work_dir, 0755) != 0) {
        fprintf(stderr, "Error: Failed to create job directory: %s\n", work_dir);
        update_job_status(job_idx, JOB_STATUS_FAILED, 0.0, "Failed to create job directory");
        result = -1;
        goto cleanup;
    }
    
    // Initialize calculation state
    calculation_state_init(&state, digits, algorithm, use_out_of_core, work_dir, job_idx);
    state_initialized = true;
    state.checkpointing_enabled = checkpointing_enabled;
    
    // Use the global calculation thread pool instead of creating a local one
    if (!g_calc_pool) {
        fprintf(stderr, "Error: Global calculation thread pool is not initialized\n");
        pthread_mutex_lock(&jobs[job_idx].lock);
        jobs[job_idx].status = JOB_STATUS_FAILED;
        strncpy(jobs[job_idx].error_message, "Global calculation thread pool not initialized", 
                sizeof(jobs[job_idx].error_message)-1);
        jobs[job_idx].error_message[sizeof(jobs[job_idx].error_message)-1] = '\0';
        pthread_mutex_unlock(&jobs[job_idx].lock);
        result = -1;
        goto cleanup;
    }
    pool_initialized = false; // We're using the global pool, not initializing our own
    
    // Update job status
    update_job_status(job_idx, JOB_STATUS_RUNNING, 0.0, NULL);
    
    // Set up timeout checker variable
    volatile bool calculation_timed_out = false;
    
    // Create a timeout monitor thread if a maximum execution time is specified
    if (max_execution_time > 0) {
        // Setup a separate thread to monitor the timeout
        timeout_data = (struct timeout_monitor_data*)malloc(sizeof(struct timeout_monitor_data));
        
        if (timeout_data) {
            timeout_data->start_time = start_time;
            timeout_data->max_execution_time = max_execution_time;
            timeout_data->timed_out_flag = &calculation_timed_out;
            timeout_data->job_idx = job_idx;
            
            int thread_result = pthread_create(&timeout_thread, NULL, 
                                             timeout_monitor_thread, 
                                             timeout_data);
            timeout_thread_created = (thread_result == 0);
            
            if (!timeout_thread_created) {
                fprintf(stderr, "Warning: Failed to create timeout monitor thread\n");
                // Continue without timeout thread - not a fatal error
            }
        } else {
            fprintf(stderr, "Warning: Failed to allocate memory for timeout data\n");
            // Continue without timeout thread - not a fatal error
        }
    }
    
    // Perform the calculation based on algorithm
    if (algorithm == ALGO_GAUSS_LEGENDRE) {
        calculate_pi_gauss_legendre(&state);
    } else {
        calculate_pi_chudnovsky(&state, g_calc_pool);
    }
    
    // Check if calculation timed out
    if (calculation_timed_out) {
        // If calculation timed out, update job status to failed
        update_job_status(job_idx, JOB_STATUS_FAILED, 1.0, "Calculation timed out");
    } else {
        // Update job status - assume success if we got here
        update_job_status(job_idx, JOB_STATUS_COMPLETED, 1.0, NULL);
    }
    
cleanup:
    // If we created a timeout thread, cancel it since the calculation is complete
    if (timeout_thread_created) {
        pthread_cancel(timeout_thread);
        pthread_join(timeout_thread, NULL);
        // timeout_data is freed by the timeout thread, don't free it here
        timeout_data = NULL;
    } else if (timeout_data) {
        // If we failed to create the thread but allocated the data, free it
        free(timeout_data);
        timeout_data = NULL;
    }
    
    // Clean up resources in reverse order of allocation
    // We're using the global pool, so no need to shut down our own pool
    
    if (state_initialized) {
        calculation_state_clear(&state);
    }
    
    if (work_dir) {
        free(work_dir);
    }
    
    if (data) {
        free(data);
    }
    
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
        
        // Use the global calculation thread pool, not a local one
        if (!g_calc_pool) {
            fprintf(stderr, "Error: Global calculation thread pool is not initialized\n");
            
            // Send HTTP 500 error to client
            json_object_object_add(response, "status", json_object_new_string("error"));
            json_object_object_add(response, "message", json_object_new_string("Internal server error: Calculation service unavailable"));
            json_object_object_add(response, "code", json_object_new_int(500));
            
            const char* json_str = json_object_to_json_string(response);
            char http_response_buf[BUFFER_SIZE];
            snprintf(http_response_buf, BUFFER_SIZE,
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n\r\n%s", 
                    strlen(json_str), json_str);
            
            write(client_sock, http_response_buf, strlen(http_response_buf));
            json_object_put(response);
            close(client_sock);
            return;
        }
        
        // Perform calculation
        if (algorithm == ALGO_GAUSS_LEGENDRE) {
            calculate_pi_gauss_legendre(&state);
        } else {
            calculate_pi_chudnovsky(&state, g_calc_pool);
        }
        
        // No need to clean up the global thread pool
        
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
        
        // Set a timeout based on digit count with more granular timeouts for larger calculations
        // The timeout calculation follows a logarithmic scale to account for the computational complexity
        if (digits > 10000000000UL) {
            thread_data->max_execution_time = 32400; // 9 hours for >10B digits
        } else if (digits > 8000000000UL) {
            thread_data->max_execution_time = 28800; // 8 hours for >8B digits
        } else if (digits > 6000000000UL) {
            thread_data->max_execution_time = 25200; // 7 hours for >6B digits
        } else if (digits > 5000000000UL) {
            thread_data->max_execution_time = 21600; // 6 hours for >5B digits
        } else if (digits > 3000000000UL) {
            thread_data->max_execution_time = 19800; // 5.5 hours for >3B digits
        } else if (digits > 1000000000UL) {
            thread_data->max_execution_time = 18000; // 5 hours for >1B digits
        } else if (digits > 500000000UL) {
            thread_data->max_execution_time = 14400; // 4 hours for >500M digits
        } else if (digits > 100000000UL) {
            thread_data->max_execution_time = 10800; // 3 hours for >100M digits
        } else if (digits > 10000000UL) {
            thread_data->max_execution_time = 9000;  // 2.5 hours for >10M digits
        } else if (digits > 1000000UL) {
            thread_data->max_execution_time = 7200;  // 2 hours for >1M digits
        } else if (digits > 500000UL) {
            thread_data->max_execution_time = 5400;  // 1.5 hours for >500K digits
        } else if (digits > 100000UL) {
            thread_data->max_execution_time = 3600;  // 1 hour for >100K digits
        } else if (digits > 50000UL) {
            thread_data->max_execution_time = 1800;  // 30 minutes for >50K digits
        } else if (digits > 10000UL) {
            thread_data->max_execution_time = 600;   // 10 minutes for >10K digits
        } else if (digits > 1000UL) {
            thread_data->max_execution_time = 300;   // 5 minutes for >1K digits
        } else {
            thread_data->max_execution_time = 180;   // 3 minutes for small calculations
        }
        
        log_message("debug", "Setting timeout for %lu digits to %ld seconds", 
                  digits, thread_data->max_execution_time);
        
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
// Make g_shutdown_flag volatile for atomic access
extern volatile bool g_shutdown_flag;

// Handle SIGINT (Ctrl+C) - graceful shutdown
// Static flag to prevent multiple calls to handle_sigint
static volatile sig_atomic_t shutdown_in_progress = 0;

void handle_sigint(int sig) {
    time_t now = time(NULL);
    
    // Use atomic operation to set the flag and check if shutdown is already in progress
    if (__atomic_exchange_n(&shutdown_in_progress, 1, __ATOMIC_SEQ_CST)) {
        // If shutdown is already in progress, force exit
        fprintf(stderr, "Forced shutdown due to repeated interrupt signal\n");
        _exit(1);
    }
    
    log_message("info", "Received shutdown signal (%d), initiating graceful shutdown...", sig);
    
    // Set global shutdown flag to stop main loop - use atomic operation
    __atomic_store_n(&g_shutdown_flag, 1, __ATOMIC_SEQ_CST);
    
    // Create a shutdown log for debugging
    char shutdown_log_path[MAX_PATH];
    snprintf(shutdown_log_path, sizeof(shutdown_log_path), "%s/shutdown_log_%ld.json", 
             config.work_dir, (long)now);
    
    // Create directory if needed
    mkdir_recursive(config.work_dir, 0755);
    
    FILE* shutdown_log = fopen(shutdown_log_path, "w");
    if (shutdown_log) {
        fprintf(shutdown_log, "{\n");
        fprintf(shutdown_log, "  \"shutdown_time\": %ld,\n", (long)now);
        fprintf(shutdown_log, "  \"signal\": %d,\n", sig);
        fprintf(shutdown_log, "  \"active_jobs\": [\n");
    }
    
    // Close server socket to stop accepting new connections
    if (g_server_sock != -1) {
        log_message("info", "Closing server socket...");
        close(g_server_sock);
        g_server_sock = -1;
    }
    
    // Attempt to save in-progress calculations before shutting down
    log_message("info", "Saving in-progress calculations...");
    
    int job_count = 0;
    pthread_mutex_lock(&jobs_lock);
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].job_id[0] != '\0' && jobs[i].status == JOB_STATUS_RUNNING) {
            job_count++;
            
            // Create a checkpoint file for this job
            char checkpoint_path[MAX_PATH];
            snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/job_%s_shutdown_checkpoint.json",
                     config.work_dir, jobs[i].job_id);
            
            FILE* checkpoint = fopen(checkpoint_path, "w");
            if (checkpoint) {
                fprintf(checkpoint, "{\n");
                fprintf(checkpoint, "  \"job_id\": \"%s\",\n", jobs[i].job_id);
                fprintf(checkpoint, "  \"algorithm\": \"%s\",\n", 
                        jobs[i].algorithm == ALGO_CHUDNOVSKY ? "CH" : "GL");
                fprintf(checkpoint, "  \"digits\": %lu,\n", jobs[i].digits);
                fprintf(checkpoint, "  \"progress\": %.6f,\n", jobs[i].progress);
                fprintf(checkpoint, "  \"status\": \"interrupted\",\n");
                fprintf(checkpoint, "  \"shutdown_time\": %ld,\n", (long)now);
                fprintf(checkpoint, "  \"error\": \"Calculation interrupted by server shutdown\"\n");
                fprintf(checkpoint, "}\n");
                fclose(checkpoint);
                
                log_message("info", "Saved checkpoint for job %s to %s", 
                           jobs[i].job_id, checkpoint_path);
            }
            
            // Add to shutdown log
            if (shutdown_log) {
                if (job_count > 1) {
                    fprintf(shutdown_log, ",\n");
                }
                
                fprintf(shutdown_log, "    {\n");
                fprintf(shutdown_log, "      \"job_id\": \"%s\",\n", jobs[i].job_id);
                fprintf(shutdown_log, "      \"algorithm\": \"%s\",\n", 
                        jobs[i].algorithm == ALGO_CHUDNOVSKY ? "CH" : "GL");
                fprintf(shutdown_log, "      \"digits\": %lu,\n", jobs[i].digits);
                fprintf(shutdown_log, "      \"progress\": %.6f,\n", jobs[i].progress);
                fprintf(shutdown_log, "      \"checkpoint_path\": \"%s\"\n", checkpoint_path);
                fprintf(shutdown_log, "    }");
            }
            
            // Mark jobs as interrupted
            pthread_mutex_lock(&jobs[i].lock);
            jobs[i].status = JOB_STATUS_FAILED;
            strncpy(jobs[i].error_message, "Calculation interrupted by server shutdown", 
                    sizeof(jobs[i].error_message)-1);
            jobs[i].error_message[sizeof(jobs[i].error_message)-1] = '\0';  // Ensure null termination
            pthread_mutex_unlock(&jobs[i].lock);
        }
    }
    pthread_mutex_unlock(&jobs_lock);
    
    // Complete shutdown log
    if (shutdown_log) {
        fprintf(shutdown_log, "\n  ],\n");
        fprintf(shutdown_log, "  \"thread_pools\": {\n");
        fprintf(shutdown_log, "    \"http_pool\": %s,\n", g_http_pool ? "active" : "inactive");
        fprintf(shutdown_log, "    \"calc_pool\": %s\n", g_calc_pool ? "active" : "inactive");
        fprintf(shutdown_log, "  }\n");
        fprintf(shutdown_log, "}\n");
        fclose(shutdown_log);
        
        log_message("info", "Saved shutdown log to %s", shutdown_log_path);
    }
    
    // Shutdown thread pools if initialized
    if (g_http_pool) {
        log_message("info", "Shutting down HTTP thread pool...");
        http_thread_pool_shutdown(g_http_pool);
        g_http_pool = NULL;
    }
    
    if (g_calc_pool) {
        log_message("info", "Shutting down calculation thread pool...");
        calc_thread_pool_shutdown(g_calc_pool);
        
        // Free the memory allocated for the pool in run_server_mode
        // Note: In CLI mode, g_calc_pool points to a stack variable, so we check for the server mode
        if (config.mode == MODE_SERVER) {
            free(g_calc_pool);
        }
        g_calc_pool = NULL;
    }
    
    // Clean up any temporary files in the working directory
    // (Note: this is a best-effort cleanup, might not get everything)
    char cleanup_cmd[MAX_PATH * 2];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "find %s -type d -name 'temp*' -exec rm -rf {} \\; 2>/dev/null || true", 
             config.work_dir);
    system(cleanup_cmd);
    
    // Flush and close log file
    log_message("info", "Closing log file and cleaning up resources...");
    close_log_file();
    
    // Free MPFR cache
    mpfr_free_cache();
    
    // Final log to stderr
    fprintf(stderr, "Pi-Server shutdown complete.\n");
    
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
    // We'll use the global calc thread pool
    
    http_thread_pool_init(&http_pool, config.max_http_threads);
    // Initialize the global calc thread pool
    g_calc_pool = (calc_thread_pool*)malloc(sizeof(calc_thread_pool));
    if (!g_calc_pool) {
        fprintf(stderr, "Failed to allocate memory for calculation thread pool\n");
        close(g_server_sock);
        http_thread_pool_shutdown(&http_pool);
        exit(1);  // Fatal error, directly exit instead of returning
    }
    calc_thread_pool_init(g_calc_pool, config.max_calc_threads);
    calc_thread_pool_start(g_calc_pool);
    
    // Store HTTP pool in global variable for signal handling
    g_http_pool = &http_pool;
    // g_calc_pool is already set
    
    // Log server start
    char log_buf[BUFFER_SIZE];
    snprintf(log_buf, BUFFER_SIZE, "Server started on %s:%d", config.ip_address, config.port);
    log_message("info", log_buf);
    
    // Main server loop
    while (!g_shutdown_flag) {
        if (g_segfault_occurred) {
            perform_crash_logging(g_crash_log_path);
            // Consider more cleanup or just exit
            _exit(2); // Different exit code for crash
        }
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
            // http_enqueue_job now handles its own locking
            int enqueued = 1; // Assume success
            
            // Enqueue the job
            http_enqueue_job(client_sock);
            
            // Check if the client socket was enqueued successfully
            // (http_enqueue_job will log errors if the queue is full)
            if (enqueued) {
                // Signal a worker thread that there's a new job
                sem_post(http_pool.job_semaphore);
                
                if (config.log_level <= LOG_LEVEL_DEBUG) {
                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                    fprintf(stderr, "Debug: Accepted connection from %s:%d\n",
                            client_ip, ntohs(client_addr.sin_port));
                }
            } else {
                // This shouldn't happen with the current implementation,
                // but left for future extensions
                log_message("error", "Failed to enqueue client connection");
                dprintf(client_sock, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n\r\n"
                                   "Server is busy, please try again later.");
                close(client_sock);
            }
        }
    }
    
    // Clean up (this should be handled by the signal handler)
    http_thread_pool_shutdown(&http_pool);
    // Clean up the global calculation thread pool
    if (g_calc_pool) {
        calc_thread_pool_shutdown(g_calc_pool);
        free(g_calc_pool);
        g_calc_pool = NULL;
    }
    
    g_http_pool = NULL;
    // g_calc_pool is already set to NULL above
    
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

    if (g_segfault_occurred) {
        perform_crash_logging(g_crash_log_path);
        _exit(2);
    }
    
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
