#ifndef COMMON_H
#define COMMON_H

// ==========================================================================
// Common Header File - Shared Types, Enums, Constants, and Structures
// ==========================================================================

// Standard includes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <limits.h>

// Network includes
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// GMP and MPFR includes
#include <gmp.h>
#include <mpfr.h>

// JSON parsing
#include <json-c/json.h>

// System includes
#include <sys/wait.h>
#include <execinfo.h>

// getopt for command line parsing
#include <getopt.h>

// ==========================================================================
// Constants
// ==========================================================================

// Buffer sizes
#define BUFFER_SIZE 4096
#define MAX_PATH 4096
#define MAX_JOBS 100
#define MAX_HTTP_QUEUE_SIZE 256

// UUID constants
#define UUID_NUM_BYTES 16
#define UUID_STR_LEN 37  // 36 chars + null terminator

// Job management
#define MAX_JOB_ERROR_MSG 256
#define MAX_JOB_AGE_SECONDS 3600  // 1 hour

// Algorithm constants for Chudnovsky
#define A 13591409
#define B 545140134
#define C 640320
#define D 12

// Gauss-Legendre algorithm constants
#define GL_DEFAULT_ITERATIONS 10
#define GL_PRECISION_BITS 100

// Memory and thresholds
#define DEFAULT_MEMORY_LIMIT (8ULL * 1024 * 1024 * 1024)  // 8 GB
#define LARGE_DIGITS_THRESHOLD 100000
#define SMALL_DIGITS_THRESHOLD 1000

// Algorithm identifiers (strings)
#define ALGORITHM_GL "gl"
#define ALGORITHM_CH "ch"

// Calculation mode constants
#define CALC_MODE_SYNC 0
#define CALC_MODE_ASYNC 1

// ==========================================================================
// Enumerations
// ==========================================================================

// Log level enumeration
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARNING = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_CRITICAL = 4
} log_level_t;

// Algorithm types
typedef enum {
    ALGO_GAUSS_LEGENDRE,
    ALGO_CHUDNOVSKY
} algorithm_t;

// Out-of-core modes
typedef enum {
    OOC_AUTO,       // Automatically determine based on size
    OOC_FORCE,      // Force out-of-core mode
    OOC_DISABLE     // Disable out-of-core mode
} out_of_core_mode_t;

// Execution modes
typedef enum {
    MODE_CLI,       // Command-line mode
    MODE_SERVER     // HTTP server mode
} execution_mode_t;

// Job status enumeration
typedef enum {
    JOB_STATUS_QUEUED,
    JOB_STATUS_RUNNING,
    JOB_STATUS_COMPLETED,
    JOB_STATUS_FAILED,
    JOB_STATUS_CANCELED
} job_status_t;

// ==========================================================================
// Structure Definitions
// ==========================================================================

// Configuration structure
typedef struct {
    // Server settings
    char ip_address[INET_ADDRSTRLEN];
    int port;
    int max_http_threads;
    int max_calc_threads;

    // Calculation settings
    unsigned long max_digits;
    size_t memory_limit;
    algorithm_t default_algorithm;
    int gl_iterations;
    int gl_precision_bits;

    // Logging settings
    int log_level;
    char logging_level[20];
    char logging_output[256];
    struct {
        int level;
    } logging;

    // Directory settings
    char work_dir[MAX_PATH];
    bool checkpointing_enabled;
    int checkpoint_interval;

    // Execution mode
    execution_mode_t mode;
} config_t;

// Forward declarations for disk_int
struct disk_int;
typedef struct disk_int disk_int;

// Forward declarations for chudnovsky_state
struct chudnovsky_state;
typedef struct chudnovsky_state chudnovsky_state;

// Forward declarations for calculation_state
struct calculation_state;
typedef struct calculation_state calculation_state;

// Forward declarations for thread pools
struct calc_thread_pool;
typedef struct calc_thread_pool calc_thread_pool;

struct http_thread_pool;
typedef struct http_thread_pool http_thread_pool;

// Forward declarations for calc_task
struct calc_task;
typedef struct calc_task calc_task;

// Calculation job structure (for async calculations)
typedef struct {
    char job_id[UUID_STR_LEN];
    algorithm_t algorithm;
    unsigned long digits;
    out_of_core_mode_t out_of_core_mode;
    bool checkpointing_enabled;
    job_status_t status;
    double progress;
    time_t creation_time;
    time_t start_time;
    time_t end_time;
    char result_file[MAX_PATH];
    char error_message[MAX_JOB_ERROR_MSG];
    pthread_mutex_t lock;
} calculation_job;

// Thread data for async calculations
typedef struct {
    int job_idx;
    time_t max_execution_time;
} calc_thread_data_t;

// Timeout monitor data
struct timeout_monitor_data {
    time_t start_time;
    time_t max_execution_time;
    volatile bool* timed_out_flag;
    int job_idx;
};

// ==========================================================================
// Global Variable Declarations (extern)
// ==========================================================================

// Configuration
extern config_t config;

// Job management
extern calculation_job jobs[MAX_JOBS];
extern pthread_mutex_t jobs_lock;
extern bool jobs_initialized;

// Thread pools
extern http_thread_pool* g_http_pool;
extern calc_thread_pool* g_calc_pool;

// Server socket
extern int g_server_sock;

// Shutdown and crash flags
extern volatile bool g_shutdown_flag;
extern volatile sig_atomic_t g_segfault_occurred;
extern char g_crash_log_path[MAX_PATH];

// Logging
extern FILE* log_file;

// Server running flag
extern bool g_server_running;

// HTTP queue
extern int http_job_queue[MAX_HTTP_QUEUE_SIZE];
extern volatile int http_queue_front;
extern volatile int http_queue_back;
extern pthread_mutex_t http_queue_lock;
extern pthread_cond_t http_queue_not_full;
extern pthread_cond_t http_queue_not_empty;

// Disk int initialization lock
extern pthread_mutex_t g_disk_int_init_lock;

#endif // COMMON_H
