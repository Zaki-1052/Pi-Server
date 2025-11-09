#include "common.h"
#include "signal_handlers.h"
#include "job_manager.h"
#include "http_pool.h"
#include "thread_pool.h"
#include "utils.h"
#include "logging.h"
#include "config.h"

// ==========================================================================
// Signal Handlers Implementation
// ==========================================================================

void perform_crash_logging(const char *crash_log_path);
void unified_log(int level, const char* format, ...);
void log_message(const char* level, const char* format, ...);

// Async-signal safe handler for segmentation faults
void segfault_handler(int sig) {
    // Document why parameter is unused
    // Signal number is not used but required by signal handler signature
    (void)sig;
    
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
    ssize_t bytes_written = write(STDERR_FILENO, msg, strlen(msg));
    // We're in a signal handler, so we can't do much if write fails
    
    // Use fork() to create a separate process for crash logging
    // This is async-signal safe according to POSIX
    pid_t pid = fork();
    
    if (pid == -1) {
        // Fork failed, simply exit
        const char *fork_fail_msg = "CRITICAL: Fork failed for crash logging, exiting.\n";
        ssize_t bytes_written = write(STDERR_FILENO, fork_fail_msg, strlen(fork_fail_msg));
        // In signal handler context, we can't do much if write fails
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
        ssize_t bytes_written = write(STDERR_FILENO, parent_msg, strlen(parent_msg));
        // In signal handler context, we can't do much if write fails
        _exit(1);
    }
}

// Non-signal function that can be called by a monitoring thread to perform thorough crash logging
void perform_crash_logging(const char *crash_log_path) {
    char buf[256];
    // Variable for tracking system command results
    int sys_result;
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
    // For other systems, try different backtrace utilities
    sys_result = system("backtrace $PPID 2>/dev/null || "
                       "gdb -ex 'set pagination 0' -ex 'thread apply all bt' -ex 'quit' -p $$ 2>/dev/null || "
                       "echo 'Backtrace utilities not available'");
    if (sys_result == -1) {
        fprintf(stderr, "Failed to execute backtrace command: %s\n", strerror(errno));
        if (crash_log) fprintf(crash_log, "Failed to execute backtrace command: %s\n", strerror(errno));
    } else if (WIFEXITED(sys_result) && WEXITSTATUS(sys_result) != 0) {
        // This command pipeline returns non-zero if backtrace or gdb are not found, 
        // or if they fail. This is not necessarily a critical error for the logger itself.
        fprintf(stderr, "Backtrace command pipeline exited with status %d\n", WEXITSTATUS(sys_result));
        if (crash_log) fprintf(crash_log, "Backtrace command pipeline exited with status %d\n", WEXITSTATUS(sys_result));
    }
#endif

    // Log memory info if possible
    fprintf(stderr, "Memory info:\n");
    if (crash_log) {
        fprintf(crash_log, "Memory info:\n");
    }
    
    // Use platform-independent approach to get memory info
#ifdef __APPLE__
    sys_result = system("ps -o pid,vsz,rss,command -p $$ 2>/dev/null >> pi_calculator_crash.log || echo 'Memory info not available'");
    if (sys_result == -1) {
        fprintf(stderr, "Failed to execute memory info command (Apple): %s\n", strerror(errno));
        if (crash_log) fprintf(crash_log, "Failed to execute memory info command (Apple): %s\n", strerror(errno));
    } else if (WIFEXITED(sys_result) && WEXITSTATUS(sys_result) != 0) {
        fprintf(stderr, "Memory info command (Apple) exited with status %d\n", WEXITSTATUS(sys_result));
        if (crash_log) fprintf(crash_log, "Memory info command (Apple) exited with status %d\n", WEXITSTATUS(sys_result));
    }
#else
    sys_result = system("cat /proc/self/status 2>/dev/null | grep -i 'VmSize\\|VmRSS' >> pi_calculator_crash.log || "
                      "ps -o pid,vsz,rss,cmd -p $$ 2>/dev/null >> pi_calculator_crash.log || "
                      "echo 'Memory info not available'");
    if (sys_result == -1) {
        fprintf(stderr, "Failed to execute memory info command (Linux): %s\n", strerror(errno));
        if (crash_log) fprintf(crash_log, "Failed to execute memory info command (Linux): %s\n", strerror(errno));
    } else if (WIFEXITED(sys_result) && WEXITSTATUS(sys_result) != 0) {
        fprintf(stderr, "Memory info command (Linux) exited with status %d\n", WEXITSTATUS(sys_result));
        if (crash_log) fprintf(crash_log, "Memory info command (Linux) exited with status %d\n", WEXITSTATUS(sys_result));
    }
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
                
                // Calculate length to ensure no truncation
                size_t dir_len = strlen(config.work_dir);
                size_t id_len = strlen(jobs[i].job_id);
                size_t suffix_len = strlen("/job__crash_checkpoint.json") + id_len;
                
                // Check if the path would exceed the buffer size
                if (dir_len + suffix_len + 1 > MAX_PATH) {
                    fprintf(stderr, "Error: Checkpoint path would be too long\n");
                } else {
                    // Use a more defensive approach to prevent truncation
                    char filename[MAX_PATH/2];
                    snprintf(filename, sizeof(filename), "job_%s_crash_checkpoint.json", jobs[i].job_id);
                    
                    // Ensure we have enough space for path + filename
                    if (dir_len + 1 + strlen(filename) < MAX_PATH) {
                        snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/%s",
                                config.work_dir, filename);
                    } else {
                        // If it would exceed the buffer, use just the filename in current directory
                        log_message("warning", "Checkpoint path too long in perform_crash_logging, using filename only for job %s", jobs[i].job_id);
                        snprintf(checkpoint_path, MAX_PATH, "%s", filename);
                    }
                    
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
                        if (fclose(checkpoint) != 0) {
                            fprintf(stderr, "Error closing crash checkpoint file %s: %s\n", checkpoint_path, strerror(errno));
                            if (crash_log) fprintf(crash_log, "Error closing crash checkpoint file %s: %s\n", checkpoint_path, strerror(errno));
                        }
                        
                        fprintf(stderr, "  Saved crash checkpoint for job %s to %s\n", 
                                jobs[i].job_id, checkpoint_path);
                        if (crash_log) {
                            fprintf(crash_log, "  Saved crash checkpoint for job %s to %s\n", 
                                    jobs[i].job_id, checkpoint_path);
                        }
                    } else {
                        // Failed to open checkpoint file
                        fprintf(stderr, "Error: Failed to open crash checkpoint file %s: %s\n", checkpoint_path, strerror(errno));
                        if (crash_log) {
                            fprintf(crash_log, "Error: Failed to open crash checkpoint file %s: %s\n", checkpoint_path, strerror(errno));
                        }
                    }
                } // Close the else block
                
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
        if (fclose(crash_log) != 0) {
            // Not much to do if the main crash log itself fails to close, but log to stderr.
            fprintf(stderr, "Error closing main crash_log file %s: %s\n", crash_log_path, strerror(errno));
        }
        fprintf(stderr, "Crash information saved to %s\n", crash_log_path);
    }
}

// ==========================================================================
// Signal Handlers (SIGINT, SIGHUP)
// ==========================================================================
// Signal Handling
// ==========================================================================

// Global variables for server and thread pools
http_thread_pool* g_http_pool = NULL;
calc_thread_pool* g_calc_pool = NULL;
int g_server_sock = -1;
// Access g_shutdown_flag which is defined earlier in the file
// There's no need to redeclare it with extern

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
    
    // Calculate length to ensure no truncation
    size_t dir_len = strlen(config.work_dir);
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%ld", (long)now);
    size_t time_len = strlen(time_str);
    size_t suffix_len = strlen("/shutdown_log_.json") + time_len;
    
    if (dir_len + suffix_len + 1 > MAX_PATH) {
        fprintf(stderr, "Error: Shutdown log path would be too long\n");
        strncpy(shutdown_log_path, "shutdown_log.json", MAX_PATH-1);
        shutdown_log_path[MAX_PATH-1] = '\0';
    } else {
        snprintf(shutdown_log_path, sizeof(shutdown_log_path), "%s/shutdown_log_%ld.json", 
                config.work_dir, (long)now);
    }
    
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
            
            // Calculate length to ensure no truncation
            size_t cp_dir_len = strlen(config.work_dir);
            size_t job_id_len = strlen(jobs[i].job_id);
            size_t cp_suffix_len = strlen("/job__shutdown_checkpoint.json") + job_id_len;
            
            if (cp_dir_len + cp_suffix_len + 1 > MAX_PATH) {
                fprintf(stderr, "Error: Checkpoint path would be too long for job %s\n", jobs[i].job_id);
                // Use a simple filename instead
                snprintf(checkpoint_path, sizeof(checkpoint_path), "job_%s_shutdown.json", jobs[i].job_id);
            } else {
                // Use a more defensive approach to prevent truncation
                char filename[MAX_PATH/2];
                snprintf(filename, sizeof(filename), "job_%s_shutdown_checkpoint.json", jobs[i].job_id);
                
                // Ensure we have enough space for path + filename
                if (cp_dir_len + 1 + strlen(filename) < MAX_PATH) {
                    snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/%s",
                            config.work_dir, filename);
                } else {
                    // If it would exceed the buffer, use just the filename in current directory
                    strncpy(checkpoint_path, filename, MAX_PATH-1);
                    checkpoint_path[MAX_PATH-1] = '\0';
                }
            }
            
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
    int sys_result = system(cleanup_cmd);
    if (sys_result == -1) {
        log_message("warning", "system call for final cleanup failed: %s", strerror(errno));
    } else if (WIFEXITED(sys_result) && WEXITSTATUS(sys_result) != 0) {
        // The command uses "|| true", so it should ideally not return a non-zero exit status
        // unless 'true' itself fails, which is highly unlikely.
        // However, if the find command has issues before "|| true", it might still log to its stderr.
        log_message("warning", "system call for final cleanup exited with status %d", WEXITSTATUS(sys_result));
    }
    
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
    // Document why parameter is unused
    // Signal number is not used but required by signal handler signature
    (void)sig;
    
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
    // Document why parameters are unused
    // Command line arguments have already been processed by the main function
    (void)argc;
    (void)argv;
    
    // Initialize jobs array to ensure it's available for crash handling
    initialize_jobs();
    
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

