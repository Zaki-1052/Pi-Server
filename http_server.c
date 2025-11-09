#include "common.h"
#include "http_server.h"
#include "job_manager.h"
#include "calc_state.h"
#include "algorithms.h"
#include "utils.h"
#include "logging.h"
#include "config.h"

// ==========================================================================
// HTTP Server Implementation
// ==========================================================================

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
    snprintf(query_copy, sizeof(query_copy), "%s", query); 
    // query_copy[sizeof(query_copy) - 1] = '\0'; // Ensure null termination, snprintf handles this if space permits.
                                               // If query is >= sizeof(query_copy), it will be truncated.
    
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
                snprintf(algo, algo_size, "%s", value);
            } else if (strcmp(key, "digits") == 0) {
                snprintf(digits_str, digits_size, "%s", value);
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
    // Document why parameter is unused
    // out_of_core_mode is not validated here but kept for future implementation
    (void)out_of_core_mode;
    
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
            // Use explicit ceil for more predictable rounding and add safety buffer
            double memory_estimate = (digits / 14.1816) * 8.0 * 3.0;
            size_t memory_needed = (size_t)ceil(memory_estimate * 1.1); // Add 10% safety margin
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
            
            ssize_t bytes_written = write(client_sock, http_response_buf, strlen(http_response_buf));
            if (bytes_written < 0) {
                fprintf(stderr, "Error writing to socket: %s\n", strerror(errno));
            }
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
    // Use ssize_t to match the return type of read() system call
    // This prevents potential truncation on 64-bit systems
    ssize_t bytes_read = read(client_sock, buffer, BUFFER_SIZE - 1);
    
    if (bytes_read <= 0) {
        log_message("error", "Failed to read from client socket");
        close(client_sock);
        return NULL;
    }
    
    // Ensure buffer is null-terminated
    buffer[bytes_read] = '\0';
    
    // Parse request method and path
    char method[10] = {0}; // Max 9 chars for method + null
    char path[BUFFER_SIZE] = {0};
    // Use width specifier for path to prevent overflow. BUFFER_SIZE is 4096.
    // %4095[^ ] ensures at most 4095 chars are read into path, leaving space for null.
    sscanf(buffer, "%9s %4095[^ ] HTTP", method, path); 
    
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
