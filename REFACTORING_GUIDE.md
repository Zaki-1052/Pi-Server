# Pi Calculator Refactoring Guide

## Overview
This document guides the refactoring of pi_calculator.c (7231 lines) into a modular structure with 14 modules.

## Files Created So Far ✓
1. **common.h** - Complete with all shared types, enums, constants
2. **utils.h / utils.c** - Complete with safe_path_join, mkdir_recursive, generate_uuid
3. **logging.h / logging.c** - Complete with unified_log, log_message, file operations
4. **config.h / config.c** - Complete with configuration management

## Remaining Files to Create

### 5. disk_int Module
**Files**: `disk_int.h`, `disk_int.c`

**Lines from original**: 540-1542 (disk_int structure and all operations)

**Key Components**:
- `struct disk_int` definition (line 540-553)
- Global mutex: `pthread_mutex_t g_disk_int_init_lock`
- Functions to extract:
  - `disk_int_init()` (line 914-995)
  - `disk_int_clear()` (line 998-1050)
  - `disk_int_set_mpz()` (line 1052-1134)
  - `disk_int_get_mpz()` (line 1136-1179)
  - `get_chunk_path()` (line 1181-1186)
  - `load_chunk()` (line 1188-1225)
  - `save_chunk()` (line 1227-1258)
  - `determine_optimal_chunk_size()` (line 1260-1273)
  - `disk_int_add()` (line 1275-1361)
  - `disk_int_sub_abs()` (line 1363-1420)
  - `disk_int_cmp_abs()` (line 1422-1481)
  - `add_product_to_result()` (line 1483-1540)
  - `disk_int_mul()` (line 1542-1830)

**Header (disk_int.h)**:
```c
#ifndef DISK_INT_H
#define DISK_INT_H

#include "common.h"

// Disk integer structure
struct disk_int {
    char file_path[MAX_PATH];
    size_t total_size_in_limbs;
    int sign;
    size_t chunk_size;
    size_t num_chunks;
    mp_limb_t* cache;
    size_t cache_chunk_idx;
    bool dirty;
    pthread_mutex_t lock;
};

// Function declarations
void disk_int_init(disk_int* d_int, const char* base_path);
void disk_int_clear(disk_int* d_int);
void disk_int_set_mpz(disk_int* d_int, const mpz_t value);
void disk_int_get_mpz(mpz_t result, const disk_int* d_int);
void disk_int_add(disk_int* result, const disk_int* a, const disk_int* b);
void disk_int_sub_abs(disk_int* result, const disk_int* a, const disk_int* b);
int disk_int_cmp_abs(const disk_int* a, const disk_int* b);
void disk_int_mul(disk_int* result, const disk_int* a, const disk_int* b);
void get_chunk_path(char* path, size_t path_size, const disk_int* d_int, size_t chunk_idx);
int load_chunk(disk_int* d_int, size_t chunk_idx);
void save_chunk(disk_int* d_int, size_t chunk_idx);
size_t determine_optimal_chunk_size(size_t memory_limit, size_t estimated_size);
void add_product_to_result(disk_int* result, const disk_int* a, mp_limb_t b_limb, size_t shift);

extern pthread_mutex_t g_disk_int_init_lock;

#endif // DISK_INT_H
```

### 6. algorithms Module
**Files**: `algorithms.h`, `algorithms.c`

**Lines from original**: 1832-5265

**Key Components**:
- `struct chudnovsky_state` (line 556-561)
- `chudnovsky_state_init()` (line 563-633)
- `chudnovsky_state_clear()` (line 635-663)
- `binary_split_mpz()` (line 665-822)
- `binary_split_disk()` (line 1832-2166)
- `calculate_pi_gauss_legendre()` (line 4186-4279)
- `calculate_pi_chudnovsky()` (line 4282-5265)

### 7. calc_state Module
**Files**: `calc_state.h`, `calc_state.c`

**Lines from original**: 4051-4180

**Key Components**:
- `struct calculation_state` (line 4054-4076)
- `calculation_state_init()` (line 4078-4170)
- `calculation_state_clear()` (line 4172-4179)
- `calculation_thread_func()` (line 5870-6064)
- `timeout_monitor_thread()` (line 5641-5780)

### 8. thread_pool Module (Calculation Thread Pool)
**Files**: `thread_pool.h`, `thread_pool.c`

**Lines from original**: 2168-3662

**Key Components**:
- Task type enum (line 2168-2172)
- Struct definitions (line 2174-2200)
- `calc_thread_pool_init()` (line 3381-3427)
- `calc_thread_pool_shutdown()` (line 3429-3478)
- `calc_thread_pool_add_task()` (line 3480-3498)
- `calc_thread_worker()` (line 3500-3534)
- `calc_thread_pool_start()` (line 3536-3561)
- `calc_thread_pool_submit_binary_split()` (line 3564-3592)
- `calc_task_wait()` (line 3594-3601)
- `calc_task_free()` (line 3604-3661)

### 9. http_pool Module
**Files**: `http_pool.h`, `http_pool.c`

**Lines from original**: 3664-3875

**Key Components**:
- `struct http_thread_pool` (line 3667-3674)
- Queue variables (line 3677-3681)
- `http_queue_size()` (line 3684-3690)
- `http_enqueue_job()` (line 3693-3726)
- `http_dequeue_job()` (line 3729-3769)
- `http_worker_function()` (line 3775-3795)
- `http_thread_pool_init()` (line 3798-3837)
- `http_thread_pool_shutdown()` (line 3840-3874)

### 10. job_manager Module
**Files**: `job_manager.h`, `job_manager.c`

**Lines from original**: 3877-4049

**Key Components**:
- Global job array (line 3885-3887)
- `initialize_jobs()` (line 3890-3906)
- `find_or_create_job()` (line 3959-4023)
- `update_job_status()` (line 4026-4048)

### 11. http_server Module
**Files**: `http_server.h`, `http_server.c`

**Lines from original**: 5783-6742

**Key Components**:
- `parse_query_string()` (line 5787-5835)
- `validate_parameters()` (line 5838-5867)
- `handle_calculation_request()` (line 6067-6393)
- `handle_job_status_request()` (line 6396-6496)
- `handle_job_result_request()` (line 6499-6635)
- `handle_http_request()` (line 6638-6742)

### 12. signal_handlers Module
**Files**: `signal_handlers.h`, `signal_handlers.c`

**Lines from original**: 218-6952

**Key Components**:
- Global variables (line 218-227)
- `segfault_handler()` (line 234-292)
- `perform_crash_logging()` (line 295-537)
- `handle_sigint()` (line 6759-6951)
- `handle_sighup()` (line 6954-6968)

### 13. Main pi_calculator.c
**Files**: `pi_calculator.c` (refactored version)

**Lines from original**: 6971-7231

**Key Components**:
- `run_server_mode()` (line 6975-7129)
- `run_cli_mode()` (line 7136-7188)
- `main()` (line 7194-7231)

## Global Variables to Define

Each module must define its global variables properly:

### In common.h (extern declarations):
```c
extern config_t config;
extern calculation_job jobs[MAX_JOBS];
extern pthread_mutex_t jobs_lock;
extern bool jobs_initialized;
extern http_thread_pool* g_http_pool;
extern calc_thread_pool* g_calc_pool;
extern int g_server_sock;
extern volatile bool g_shutdown_flag;
extern volatile sig_atomic_t g_segfault_occurred;
extern char g_crash_log_path[MAX_PATH];
extern FILE* log_file;
extern bool g_server_running;
extern int http_job_queue[MAX_HTTP_QUEUE_SIZE];
extern volatile int http_queue_front;
extern volatile int http_queue_back;
extern pthread_mutex_t http_queue_lock;
extern pthread_cond_t http_queue_not_full;
extern pthread_cond_t http_queue_not_empty;
extern pthread_mutex_t g_disk_int_init_lock;
```

### In respective .c files (actual definitions):
- `config.c`: config, g_server_running, g_shutdown_flag
- `logging.c`: log_file
- `job_manager.c`: jobs, jobs_lock, jobs_initialized
- `http_pool.c`: http_job_queue, http_queue_front, http_queue_back, locks
- `signal_handlers.c`: g_segfault_occurred, g_crash_log_path, g_server_sock, g_http_pool, g_calc_pool
- `disk_int.c`: g_disk_int_init_lock

## Build Order & Dependencies

1. **common.h** - No dependencies
2. **utils** - Depends on: common.h, logging.h
3. **logging** - Depends on: common.h
4. **config** - Depends on: common.h, logging.h
5. **disk_int** - Depends on: common.h, logging.h, utils.h
6. **algorithms** - Depends on: common.h, disk_int.h, thread_pool.h (forward refs)
7. **calc_state** - Depends on: common.h, algorithms.h, utils.h
8. **thread_pool** - Depends on: common.h, algorithms.h, disk_int.h
9. **http_pool** - Depends on: common.h, http_server.h (forward refs)
10. **job_manager** - Depends on: common.h, utils.h
11. **http_server** - Depends on: common.h, job_manager.h, calc_state.h, algorithms.h
12. **signal_handlers** - Depends on: common.h, logging.h, config.h, job_manager.h, http_pool.h, thread_pool.h
13. **pi_calculator.c** - Depends on: all headers

## Next Steps

1. Create each remaining module following the pattern:
   - Create .h file with function declarations
   - Create .c file with implementations
   - Extract code from original pi_calculator.c at specified line ranges
   - Update includes appropriately

2. Test compilation with `make` after creating all files

3. Verify functionality matches original

4. Keep the original pi_calculator.c as `pi_calculator.c.backup` for reference

## Compilation Command

```bash
gcc -o pi-server pi_calculator.c utils.c logging.c config.c disk_int.c \
    algorithms.c calc_state.c thread_pool.c http_pool.c job_manager.c \
    http_server.c signal_handlers.c \
    -lm -lgmp -lmpfr -ljson-c -lpthread -O3
```
