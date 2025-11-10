// ==========================================
// Pi Calculator Server - Refactored Main
// ==========================================
//
// This is the refactored main entry point for the Pi Calculator Server.
// Most functionality has been extracted into separate modules.

#include "common.h"
#include "utils.h"
#include "logging.h"
#include "config.h"
#include "disk_int.h"
#include "algorithms.h"
#include "thread_pool.h"
#include "http_pool.h"
#include "job_manager.h"
#include "calc_state.h"
#include "http_server.h"
#include "signal_handlers.h"

// ==========================================================================
// Global Variable Definitions
// ==========================================================================

// Configuration
config_t config;

// Job management (arrays defined here, extern declared in common.h)
calculation_job jobs[MAX_JOBS];
pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
bool jobs_initialized = false;

// Thread pools
http_thread_pool* g_http_pool = NULL;
calc_thread_pool* g_calc_pool = NULL;

// Server socket
int g_server_sock = -1;

// Shutdown and crash flags
volatile sig_atomic_t g_shutdown_flag = 0;
volatile sig_atomic_t g_segfault_occurred = 0;
char g_crash_log_path[MAX_PATH] = {0};

// Logging
FILE* log_file = NULL;

// Server running flag
bool g_server_running = false;

// ==========================================================================
// Main Functions
// ==========================================================================

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

        // Check if SIGHUP was received and handle config reload
        if (sighup_received) {
            handle_sighup_reload();
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

    // Shutdown flag was set - perform graceful cleanup in main thread
    // This is now safe because we're in the main thread, not a signal handler
    perform_graceful_shutdown();

    // Clean up remaining resources
    http_thread_pool_shutdown(&http_pool);
    g_http_pool = NULL;

    // Close server socket if still open
    if (g_server_sock != -1) {
        close(g_server_sock);
        g_server_sock = -1;
    }
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
