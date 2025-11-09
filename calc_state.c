#include "common.h"
#include "calc_state.h"
#include "algorithms.h"
#include "thread_pool.h"
#include "job_manager.h"
#include "utils.h"
#include "logging.h"

// ==========================================================================
// Calculation State Implementation
// ==========================================================================

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
    
    // Ensure null termination by using snprintf
    snprintf(state->work_dir, MAX_PATH, "%s", work_dir);
    
    // Create work directory if it doesn't exist
    mkdir_recursive(state->work_dir, 0755);
    
    // Set output file path
    snprintf(state->output_file, MAX_PATH, "%s/pi_%lu.txt", work_dir, digits);
    
    // Initialize algorithm-specific data
    if (algorithm == ALGO_CHUDNOVSKY) {
        // Create path for Chudnovsky state
        char current_path[MAX_PATH];
        snprintf(current_path, MAX_PATH, "%s/current", work_dir);
        
        // Delete current directory if it's not a directory (could be a failed symlink)
        struct stat st;
        if (lstat(current_path, &st) == 0 && !S_ISDIR(st.st_mode)) {
            printf("DEBUG: Removing existing current path that is not a directory\n");
            unlink(current_path);
        }
        
        // Create the directory
        if (mkdir_recursive(current_path, 0755) != 0) {
            fprintf(stderr, "Error: Failed to create current directory: %s\n", current_path);
            return;
        }
        
        // Create subdirectories for P, Q, T
        char p_dir[MAX_PATH], q_dir[MAX_PATH], t_dir[MAX_PATH];
        if (safe_path_join(p_dir, MAX_PATH, current_path, "P") < 0 ||
            safe_path_join(q_dir, MAX_PATH, current_path, "Q") < 0 ||
            safe_path_join(t_dir, MAX_PATH, current_path, "T") < 0) {
            fprintf(stderr, "Error: Failed to create P,Q,T paths\n");
            return;
        }
        
        // Create the directories for P, Q, T
        if (mkdir_recursive(p_dir, 0755) != 0 ||
            mkdir_recursive(q_dir, 0755) != 0 ||
            mkdir_recursive(t_dir, 0755) != 0) {
            fprintf(stderr, "Error: Failed to create P,Q,T directories\n");
            return;
        }
        
        // Initialize Chudnovsky state
        chudnovsky_state_init(&state->data.chudnovsky, current_path);
        
        // Verify initialization was successful
        if (state->data.chudnovsky.P.file_path[0] == '\0' || 
            state->data.chudnovsky.Q.file_path[0] == '\0' || 
            state->data.chudnovsky.T.file_path[0] == '\0') {
            fprintf(stderr, "Error: Failed to properly initialize Chudnovsky state\n");
            return;
        }
    } else {
        // Initialize Gauss-Legendre state
        // Cast from unsigned long (digits) to mpfr_prec_t (long) with overflow check
        if (digits > (unsigned long)LONG_MAX / 4) {
            log_message("error", "Digits value too large for precision calculation");
            return;
        }
        mpfr_prec_t precision = (mpfr_prec_t)(digits * 4) + GL_PRECISION_BITS;
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


// ==========================================================================
// Timeout Monitor Thread
// ==========================================================================
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
    
    // Calculate length to ensure no truncation
    size_t dir_len = strlen(config.work_dir);
    char idx_str[32];
    snprintf(idx_str, sizeof(idx_str), "%d", job_idx);
    size_t idx_len = strlen(idx_str);
    size_t suffix_len = strlen("/job__timeout.json") + idx_len;
    
    if (dir_len + suffix_len + 1 > MAX_PATH) {
        fprintf(stderr, "Error: Timeout file path would be too long\n");
        strncpy(timeout_file, "timeout_report.json", MAX_PATH-1);
        timeout_file[MAX_PATH-1] = '\0';
    } else {
        snprintf(timeout_file, sizeof(timeout_file), "%s/job_%d_timeout.json", 
                config.work_dir, job_idx);
    }
    
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

// ==========================================================================
// Calculation Thread Function
// ==========================================================================
void* calculation_thread_func(void* arg) {
    // Initialize resources to NULL so we can safely clean up on error paths
    calc_thread_data_t* data = NULL;
    char* work_dir = NULL;
    pthread_t timeout_thread;
    bool timeout_thread_created = false;
    struct timeout_monitor_data* timeout_data = NULL;
    calculation_state state = {0};
    // TODO: Implement pool usage or remove in future update
    // calc_thread_pool pool = {0};
    bool pool_initialized = false;
    bool state_initialized = false;
    // Variable needed for tracking function result
    int result = 0;
    
    // Check for null argument
    if (!arg) {
        fprintf(stderr, "Error: Null argument passed to calculation_thread_func\n");
        // No need to set result since it's commented out
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
    char job_id[UUID_STR_LEN]; 
    
    pthread_mutex_lock(&jobs[job_idx].lock);
    algorithm = jobs[job_idx].algorithm;
    digits = jobs[job_idx].digits;
    out_of_core_mode = jobs[job_idx].out_of_core_mode;
    checkpointing_enabled = jobs[job_idx].checkpointing_enabled;
    // Use snprintf for safer string copy and guaranteed null termination
    snprintf(job_id, sizeof(job_id), "%s", jobs[job_idx].job_id);
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
            // Use explicit ceil for more predictable rounding and add safety buffer
            // This is a heuristic to estimate Chudnovsky memory needs
            double memory_estimate = (digits / 14.1816) * 8.0 * 3.0;
            size_t memory_needed = (size_t)ceil(memory_estimate * 1.1); // Add 10% safety margin
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

