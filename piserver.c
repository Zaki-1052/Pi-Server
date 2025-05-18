// ==========================================
// Pi Calculation Server - Version 1.0
// Author: Chat GPT-4.0 (Prompting by Derick Schaefer)
// Date: 9/1/2024
// Description: This server calculates the value of Pi up to a specified number of decimal places using 
//              the Gauss-Legendre Algorithm, serves HTTP requests, and manages concurrency with threads.
// ==========================================

// ==========================================================================
// Headers
// ==========================================================================

// Standard Input/Output and Utility Libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Networking Libraries for socket handling and IP management
#include <arpa/inet.h>
#include <sys/select.h>  // For select() functionality

// Threading Libraries
#include <pthread.h>
#include <semaphore.h>   // For semaphore synchronization

// GMP-based High-Precision Math Libraries for Pi calculation
#include <mpfr.h>  // Multiple Precision Floating-Point Reliable Library

// JSON handling for request/response formatting
#include <json-c/json.h>  // JSON parsing and creation

// Signal Handling Libraries for Graceful Shutdown
#include <signal.h>  // For signal handling (e.g., SIGINT)

// Time Libraries for Logging and Time Tracking
#include <time.h>    // General time management

// ==========================================================================
// Constants and Macros
// ==========================================================================
#define BUFFER_SIZE 1024       // Buffer size for reading data
#define MAX_QUEUE_SIZE 128     // Maximum number of requests that can be queued

// ==========================================================================
// Threading and Synchronization Variables
// ==========================================================================

// Mutex to protect access to shared resources
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;  

// Semaphore to signal available jobs in the job queue
sem_t job_semaphore;

// Queue for storing incoming client socket file descriptors
int job_queue[MAX_QUEUE_SIZE];  // Job queue to store client sockets
int queue_front = 0, queue_back = 0;  // Indices for the circular queue

// Thread pool for handling incoming requests
pthread_t *thread_pool;  // Dynamically allocated pool of worker threads
int active_threads = 0;  // Current number of active threads

// ==========================================================================
// Configuration Variables (Set by config.json)
// ==========================================================================

// Server IP address and port (loaded from config.json)
char ip_address[INET_ADDRSTRLEN] = "127.0.0.1";  // Default IP address
int port = 8080;  // Default port

// Maximum number of Pi digits to calculate
int max_pi_digits = 1000000;  // Default to 1 million digits

// Precision settings for Pi calculation
int precision_bits = 128;  // Additional bits for precision in calculations

// Logging configuration (level and output)
char logging_level[10] = "info";  // Default logging level
char logging_output[256] = "console";  // Default log output (console)

// Maximum number of threads (loaded from config.json)
int max_threads = 32;  // Default max threads (will be overwritten by config)

// File pointer for logging (if output is set to file)
FILE *log_file = NULL;

// ==========================================================================
// Server State Variables
// ==========================================================================

int server_sock;  // File descriptor for the server socket

// Flag to indicate if the server is shutting down
volatile int shutdown_flag = 0;  // Set when the server receives a shutdown signal

// ==========================================================================
// Function Declarations
// ==========================================================================

// Handles incoming client requests (executed by worker threads)
void* handle_request(void* arg);
// log message declaration
void log_message(const char *level, const char *message);

// ==========================================================================
// Job Queue Management Functions
// ==========================================================================

/**
 * Enqueues a client socket into the job queue. 
 * Wraps around the queue using modulo to prevent overflow.
 * 
 * @param client_sock The client socket file descriptor to enqueue.
 */
void enqueue_job(int client_sock) {
    job_queue[queue_back] = client_sock;
    queue_back = (queue_back + 1) % MAX_QUEUE_SIZE;  // Circular buffer wrap-around
}

/**
 * Dequeues a client socket from the job queue.
 * Wraps around the queue using modulo to prevent underflow.
 * 
 * @return The client socket file descriptor that was dequeued.
 */
int dequeue_job() {
    int client_sock = job_queue[queue_front];
    queue_front = (queue_front + 1) % MAX_QUEUE_SIZE;  // Circular buffer wrap-around
    return client_sock;
}

// ==========================================================================
// Worker Thread Function with Backoff Strategy
// ==========================================================================

/**
 * The function executed by each worker thread. 
 * Continuously processes jobs from the job queue until the shutdown flag is set.
 * 
 * @param arg Not used, can be NULL.
 * @return Always returns NULL when the thread exits.
 */
void* worker_function(void* arg) {
    while (!shutdown_flag) {  // Continue processing until the server is shutting down
        sem_wait(&job_semaphore);  // Wait until there is a job in the queue

        // Double check the shutdown flag after waking up from the semaphore
        if (shutdown_flag) {  
            break;  // Exit the loop if the server is shutting down
        }

        int client_sock;
        int attempts = 0;
        int max_attempts = 5;  // Maximum number of retries to process a job

        // Backoff loop to handle job queue full condition
        while (attempts < max_attempts) {
            pthread_mutex_lock(&lock);  // Lock for accessing the queue
            if (queue_front != queue_back) {  // Check if the job queue is not empty
                client_sock = dequeue_job();
                pthread_mutex_unlock(&lock);
                break;  // Exit the backoff loop once a job is dequeued
            }
            pthread_mutex_unlock(&lock);

            // If job queue was full, back off and retry
            attempts++;
            log_message("error", "Job queue empty, backing off.");
            usleep(100000 * attempts);  // Exponential backoff: 100ms, 200ms, 300ms...
        }

        if (attempts == max_attempts) {
            log_message("error", "Max retries reached, dropping request.");
            continue;  // Drop this request and continue to the next one
        }

        // Process the client request
        handle_request(&client_sock);
    }

    return NULL;  // Thread exits here
}

// ==========================================================================
// Thread Pool Initialization
// ==========================================================================

/**
 * Initializes the thread pool and semaphore.
 * Allocates memory for the thread pool and starts the worker threads.
 */
void initialize_thread_pool() {
    sem_init(&job_semaphore, 0, 0);  // Initialize the semaphore with an initial count of 0

    // Allocate memory for the thread pool based on the configured max_threads
    thread_pool = malloc(max_threads * sizeof(pthread_t));

    // Create worker threads that will execute the worker_function
    for (int i = 0; i < max_threads; i++) {
        pthread_create(&thread_pool[i], NULL, worker_function, NULL);
    }
}

// Function to validate if the input string represents a valid integer
int is_valid_integer(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] < '0' || str[i] > '9') return 0;  // Not a digit
    }
    return 1;
}

// ==========================================================================
// Logging Function
// ==========================================================================

/**
 * Logs a message based on the specified logging level and output destination.
 * 
 * @param level The logging level (e.g., "info", "error", "debug").
 * @param message The message to be logged.
 * 
 * This function logs the message to either the console or a log file, 
 * depending on the configuration. It also includes a timestamp for each log entry.
 */
void log_message(const char *level, const char *message) {
    // Get the current time
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);  // Convert time to local time
    char timestamp[20];  // Buffer for storing the timestamp in the format YYYY-MM-DD HH:MM:SS
    strftime(timestamp, 20, "%Y-%m-%d %H:%M:%S", tm_info);  // Format the timestamp

    // Log format: [YYYY-MM-DD HH:MM:SS] level: message
    // Only log messages if the logging level matches or if the level is "debug"
    if (strcmp(logging_level, "debug") == 0 || strcmp(logging_level, level) == 0) {
        if (strcmp(logging_output, "console") == 0) {
            // Log to the console
            printf("[%s] %s: %s\n", timestamp, level, message);
        } else if (log_file) {
            // Log to the specified log file
            fprintf(log_file, "[%s] %s: %s\n", timestamp, level, message);
            fflush(log_file);  // Ensure the message is written immediately to the file
        }
    }
}

// ==========================================================================
// Configuration Loading Function
// ==========================================================================

/**
 * Loads configuration settings from a specified JSON config file.
 * 
 * This function reads the configuration parameters such as IP address, port, 
 * maximum number of Pi digits, threading parameters, and logging settings from
 * a JSON file and stores them in global configuration variables.
 * 
 * @param config_file The path to the configuration file (e.g., "config.json").
 */
void load_config(const char *config_file) {
    struct json_object *parsed_json;
    struct json_object *j_ip, *j_port, *j_max_pi_digits, *j_max_threads, *j_precision_bits, *j_logging;

    // Open the config file for reading
    FILE *fp = fopen(config_file, "r");
    if (fp == NULL) {
        fprintf(stderr, "Could not open config file: %s\n", config_file);
        exit(1);
    }

    // Read the file into a buffer
    char buffer[BUFFER_SIZE] = {0};  // Ensure the buffer is zero-initialized
    fread(buffer, 1, BUFFER_SIZE - 1, fp);  // Leave space for the null terminator
    fclose(fp);

    // Parse the JSON content
    parsed_json = json_tokener_parse(buffer);
    if (parsed_json == NULL) {
        fprintf(stderr, "Failed to parse config file: %s\n", config_file);
        exit(1);
    }

    // ======================================================================
    // Parse Individual Configuration Settings
    // ======================================================================

    // Read IP address
    if (json_object_object_get_ex(parsed_json, "ip_address", &j_ip)) {
        const char *ip = json_object_get_string(j_ip);
        if (ip != NULL) {
            strncpy(ip_address, ip, INET_ADDRSTRLEN);  // Copy IP address
        } else {
            fprintf(stderr, "Invalid or missing 'ip_address' in config\n");
            exit(1);
        }
    } else {
        fprintf(stderr, "Missing 'ip_address' in config\n");
        exit(1);
    }

    // Read port
    if (json_object_object_get_ex(parsed_json, "port", &j_port)) {
        port = json_object_get_int(j_port);
    } else {
        fprintf(stderr, "Missing 'port' in config\n");
        exit(1);
    }

    // Read max_pi_digits
    if (json_object_object_get_ex(parsed_json, "max_pi_digits", &j_max_pi_digits)) {
        max_pi_digits = json_object_get_int(j_max_pi_digits);
    } else {
        fprintf(stderr, "Missing 'max_pi_digits' in config\n");
        exit(1);
    }

    // Read max_threads
    if (json_object_object_get_ex(parsed_json, "max_threads", &j_max_threads)) {
        max_threads = json_object_get_int(j_max_threads);
    } else {
        fprintf(stderr, "Missing 'max_threads' in config\n");
        exit(1);
    }

    // Read precision_bits
    if (json_object_object_get_ex(parsed_json, "precision_bits", &j_precision_bits)) {
        precision_bits = json_object_get_int(j_precision_bits);
    } else {
        fprintf(stderr, "Missing 'precision_bits' in config\n");
        exit(1);
    }

    // ======================================================================
    // Parse Logging Configuration
    // ======================================================================

    if (json_object_object_get_ex(parsed_json, "logging", &j_logging)) {
        struct json_object *j_logging_level, *j_logging_output;

        // Read logging level
        if (json_object_object_get_ex(j_logging, "level", &j_logging_level)) {
            const char *level = json_object_get_string(j_logging_level);
            if (level != NULL) {
                strncpy(logging_level, level, sizeof(logging_level));  // Copy logging level
            } else {
                fprintf(stderr, "Invalid or missing 'level' in logging config\n");
                exit(1);
            }
        } else {
            fprintf(stderr, "Missing 'level' in logging config\n");
            exit(1);
        }

        // Read logging output
        if (json_object_object_get_ex(j_logging, "output", &j_logging_output)) {
            const char *output = json_object_get_string(j_logging_output);
            if (output != NULL) {
                strncpy(logging_output, output, sizeof(logging_output));  // Copy logging output
            } else {
                fprintf(stderr, "Invalid or missing 'output' in logging config\n");
                exit(1);
            }

            // If the logging output is not set to "console", open the log file
            if (strcmp(logging_output, "console") != 0) {
                log_file = fopen(logging_output, "a");
                if (!log_file) {
                    fprintf(stderr, "Failed to open log file: %s\n", logging_output);
                    exit(1);
                }
            }
        } else {
            fprintf(stderr, "Missing 'output' in logging config\n");
            exit(1);
        }
    } else {
        fprintf(stderr, "Missing 'logging' in config\n");
        exit(1);
    }

    // Free the parsed JSON object
    json_object_put(parsed_json);
}

// ==========================================================================
// Pi Calculation Function (Gauss-Legendre Algorithm)
// ==========================================================================

/**
 * Calculates the value of Pi using the specified precision and the Gauss-Legendre algorithm.
 * 
 * @param pi The mpfr_t variable to store the calculated value of Pi.
 * @param digits The number of digits of precision for Pi (input by the user).
 * 
 * This function uses arbitrary-precision floating-point arithmetic with the MPFR library.
 * It applies the Gauss-Legendre method to approximate Pi.
 */
void calculate_pi(mpfr_t pi, unsigned long int digits) {
    // Determine the precision needed for the calculation
    mpfr_prec_t precision = digits * 4 + precision_bits;  // Precision loaded from config

    // Initialize variables for the Gauss-Legendre algorithm
    mpfr_t a, b, t, p, a_next, b_next, t_next, pi_approx;
    mpfr_init2(a, precision);
    mpfr_init2(b, precision);
    mpfr_init2(t, precision);
    mpfr_init2(p, precision);
    mpfr_init2(a_next, precision);
    mpfr_init2(b_next, precision);
    mpfr_init2(t_next, precision);
    mpfr_init2(pi_approx, precision);

    // Set the initial values for the algorithm
    mpfr_set_ui(a, 1, MPFR_RNDN);           // a = 1
    mpfr_sqrt_ui(b, 2, MPFR_RNDN);          // b = 1 / sqrt(2)
    mpfr_ui_div(b, 1, b, MPFR_RNDN);        // b = 1 / sqrt(2)
    mpfr_set_ui(t, 1, MPFR_RNDN);           // t = 1
    mpfr_div_ui(t, t, 4, MPFR_RNDN);        // t = 1 / 4
    mpfr_set_ui(p, 1, MPFR_RNDN);           // p = 1

    // Iterate to improve the approximation of Pi
    for (int i = 0; i < 10; i++) {
        // Calculate the next values of a and b
        mpfr_add(a_next, a, b, MPFR_RNDN);  // a_next = (a + b) / 2
        mpfr_div_ui(a_next, a_next, 2, MPFR_RNDN);

        mpfr_mul(b_next, a, b, MPFR_RNDN);  // b_next = sqrt(a * b)
        mpfr_sqrt(b_next, b_next, MPFR_RNDN);

        // Update t using the difference between a and a_next
        mpfr_sub(t_next, a, a_next, MPFR_RNDN);
        mpfr_pow_ui(t_next, t_next, 2, MPFR_RNDN);  // t_next = (a - a_next)^2
        mpfr_mul(t_next, t_next, p, MPFR_RNDN);     // t_next = p * (a - a_next)^2
        mpfr_sub(t_next, t, t_next, MPFR_RNDN);     // t_next = t - p * (a - a_next)^2

        mpfr_mul_ui(p, p, 2, MPFR_RNDN);  // p *= 2

        // Update the variables for the next iteration
        mpfr_set(a, a_next, MPFR_RNDN);
        mpfr_set(b, b_next, MPFR_RNDN);
        mpfr_set(t, t_next, MPFR_RNDN);
    }

    // Final approximation of Pi: pi = (a + b)^2 / (4 * t)
    mpfr_add(pi_approx, a, b, MPFR_RNDN);   // pi_approx = a + b
    mpfr_pow_ui(pi_approx, pi_approx, 2, MPFR_RNDN);  // pi_approx = (a + b)^2
    mpfr_mul_ui(t, t, 4, MPFR_RNDN);        // t = 4 * t
    mpfr_div(pi, pi_approx, t, MPFR_RNDN);  // pi = pi_approx / t

    // Clear all MPFR variables to free memory
    mpfr_clear(a);
    mpfr_clear(b);
    mpfr_clear(t);
    mpfr_clear(p);
    mpfr_clear(a_next);
    mpfr_clear(b_next);
    mpfr_clear(t_next);
    mpfr_clear(pi_approx);
}

// ==========================================================================
// Handle Incoming Client Request
// ==========================================================================

/**
 * This function handles an incoming request from the client, validates the request,
 * performs the Pi calculation, and sends the result back to the client.
 *
 * @param arg Pointer to the client socket descriptor.
 * @return NULL when the request is processed.
 */
void* handle_request(void* arg) {
    // Step 1: Retrieve and log client information
    int client_sock = *(int*)arg;  // Extract the client socket from the argument
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    getpeername(client_sock, (struct sockaddr*)&client_addr, &addr_len);  // Get client address
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);  // Convert IP to string

    // Log inbound request from the client
    char log_buffer[BUFFER_SIZE];
    snprintf(log_buffer, BUFFER_SIZE, "Inbound Request Received from %s", client_ip);
    log_message("info", log_buffer);

    // Step 2: Initialize buffer and read the request from the client
    char buffer[BUFFER_SIZE] = {0};  // Buffer to hold client request
    int bytes_read = read(client_sock, buffer, BUFFER_SIZE - 1);  // Read client request into buffer

    // Check for read errors
    if (bytes_read < 0) {
        log_message("error", "Failed to read from client socket");
        close(client_sock);  // Close the connection
        return NULL;
    }

    buffer[BUFFER_SIZE - 1] = '\0';  // Ensure null-terminated string

    // Step 3: Validate that the request is a GET request
    if (strncmp(buffer, "GET", 3) != 0) {
        snprintf(log_buffer, BUFFER_SIZE, "Invalid API request (Not GET) from %s", client_ip);
        log_message("error", log_buffer);
        dprintf(client_sock, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"status\": \"error\", \"message\": \"Invalid API request\", \"code\": 400}\n");
        close(client_sock);  // Close the connection
        return NULL;
    }

    // Step 4: Parse query parameters (algo and digits)
    char algo[10] = {0}, digits_str[10] = {0};
    int sscanf_result = sscanf(buffer, "GET /pi?algo=%9[^&]&digits=%9s", algo, digits_str);

    if (sscanf_result != 2) {  // Ensure both parameters are parsed successfully
        log_message("error", "Failed to parse query parameters");
        dprintf(client_sock, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"status\": \"error\", \"message\": \"Invalid query parameters\", \"code\": 400}\n");
        close(client_sock);  // Close the connection
        return NULL;
    }

    // Step 5: Validate algorithm and digits input
    if (strcmp(algo, "GL") != 0 || !is_valid_integer(digits_str) || atoi(digits_str) < 1 || atoi(digits_str) > max_pi_digits) {
        snprintf(log_buffer, BUFFER_SIZE, "Invalid API request (Invalid parameters) from %s", client_ip);
        log_message("error", log_buffer);
        dprintf(client_sock, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"status\": \"error\", \"message\": \"Invalid API request\", \"code\": 400}\n");
        close(client_sock);  // Close the connection
        return NULL;
    }

    // Step 6: Log valid request
    pthread_t thread_id = pthread_self();
    int digits = atoi(digits_str);  // Convert digits to integer
    snprintf(log_buffer, BUFFER_SIZE, "Request for %d decimal places being processed on thread %lu", digits, (unsigned long)thread_id);
    log_message("info", log_buffer);

    // Step 7: Start the timer for performance measurement
    clock_t start_time = clock();

    // Step 8: Calculate Pi with precision
    mpfr_t pi;
    mpfr_init2(pi, digits * 4 + precision_bits);  // Initialize Pi with requested precision
    calculate_pi(pi, digits);  // Perform the Pi calculation

    // Step 9: Allocate memory for the Pi string and format the result
    char* pi_str = malloc(digits + 10);  // Allocate space for Pi result
    if (pi_str == NULL) {
        log_message("error", "Memory allocation failed for pi_str");
        close(client_sock);  // Close the connection
        return NULL;
    }

    // Format Pi with requested precision
    mpfr_sprintf(pi_str, "%.*Rf", digits + 1, pi);  // Store Pi in string format

    // === NEW: Write pi_str to a file ===
    // Generate a unique filename
    char datetime_str[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(datetime_str, sizeof(datetime_str), "%Y%m%d_%H%M%S", tm_info);

    char filename[128];
    snprintf(filename, sizeof(filename), "pi_%d_%s.txt", digits, datetime_str);

    FILE *fp = fopen(filename, "w");
    if (fp == NULL) {
        log_message("error", "Failed to open file for writing pi digits");
        // Respond with error JSON
        dprintf(client_sock, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n\r\n{\"status\": \"error\", \"message\": \"Failed to write pi digits to file\", \"code\": 500}\n");
        free(pi_str);
        mpfr_clear(pi);
        close(client_sock);
        return NULL;
    }
    // Write the digits to the file
    fwrite(pi_str, 1, strlen(pi_str), fp);
    fclose(fp);

    // Step 10: Measure the time taken for Pi calculation
    clock_t end_time = clock();
    double time_taken = ((double)(end_time - start_time)) / CLOCKS_PER_SEC * 1000;  // Time in milliseconds

    // Step 11: Prepare the current timestamp in ISO 8601 format
    char timestamp[25];
    strftime(timestamp, 25, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    // Step 12: Build the JSON response object
    struct json_object *json_response = json_object_new_object();
    json_object_object_add(json_response, "algorithm", json_object_new_string("GL"));
    json_object_object_add(json_response, "digits", json_object_new_int(digits));
    // json_object_object_add(json_response, "pi", json_object_new_string(pi_str)); // REMOVE THIS LINE
    json_object_object_add(json_response, "truncated", json_object_new_boolean(1));
    json_object_object_add(json_response, "time_taken", json_object_new_double(time_taken));
    json_object_object_add(json_response, "timestamp", json_object_new_string(timestamp));
    json_object_object_add(json_response, "filename", json_object_new_string(filename));
    json_object_object_add(json_response, "status", json_object_new_string("success"));
    json_object_object_add(json_response, "message", json_object_new_string("Pi digits written to file"));

    // Step 13: Send the response to the client
    const char *json_str = json_object_to_json_string(json_response);
    dprintf(client_sock, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n%s\n", json_str);

    // Log the response
    snprintf(log_buffer, BUFFER_SIZE, "Response returned to %s", client_ip);
    log_message("info", log_buffer);

    // Step 14: Clean up resources
    mpfr_clear(pi);  // Free MPFR Pi object
    free(pi_str);  // Free allocated memory for Pi string
    json_object_put(json_response);  // Free the JSON object
    close(client_sock);  // Close the client socket

    // Step 15: Decrement the active threads count
    pthread_mutex_lock(&lock);  // Lock the mutex to modify shared resources
    active_threads--;  // Decrement active threads counter
    pthread_mutex_unlock(&lock);  // Unlock the mutex

    return NULL;
}

// Function to handle SIGHUP and reopen the log file
void handle_sighup(int sig) {
    if (log_file) {
        fclose(log_file);  // Close the existing log file
    }

    // Reopen the log file with the same name
    log_file = fopen(logging_output, "a");
    if (!log_file) {
        fprintf(stderr, "Failed to reopen log file: %s\n", logging_output);
        exit(1);  // Exit if we fail to reopen the log file
    }

    log_message("info", "Log file reopened after SIGHUP signal");
}

// signal up end


// ==========================================================================
// Graceful Server Shutdown and Signal Handling
// ==========================================================================

/**
 * Signal handler for managing graceful server shutdown. 
 * This function will stop new incoming connections, wake up worker threads,
 * join them, and perform resource cleanup (e.g., closing the log file and server socket).
 *
 * @param sig The signal received (e.g., SIGINT).
 */
void handle_shutdown(int sig) {
    // Log that the shutdown signal has been received
    log_message("info", "Received shutdown signal, closing server...");
    
    // Step 1: Set the shutdown flag to notify worker threads that they should exit
    shutdown_flag = 1;

    // Step 2: Post to the semaphore to unblock any waiting threads
    // This ensures that any thread waiting for a job can wake up and check the shutdown flag
    for (int i = 0; i < max_threads; i++) {
        sem_post(&job_semaphore);  // Wake up all worker threads
    }

    // Step 3: Gracefully shut down the thread pool by joining all threads
    for (int i = 0; i < max_threads; i++) {
        log_message("info", "Attempting to join thread...");
        pthread_join(thread_pool[i], NULL);  // Join the worker thread
        log_message("info", "Thread joined successfully.");
    }

    // Step 4: Free the dynamically allocated memory for the thread pool
    if (thread_pool) {
        free(thread_pool);
        thread_pool = NULL;
    }

    // Step 5:  Clear MPFR cache
    log_message("info", "Freeing MPFR cache...");
    mpfr_free_cache();  // Free any temporary MPFR storage

    // Step 5: Close the server socket to stop accepting new connections
    if (server_sock != -1) {
        close(server_sock);
        log_message("info", "Server socket closed.");
    }

    // Step 6: Perform any additional cleanup, such as closing the log file
    if (log_file) {
        log_message("info", "Shutting down server");
        fclose(log_file);  // Close the log file
    }

    // Step 7: Exit the program gracefully
    exit(0);
}

// ==========================================================================
// Main Server Program - Entry Point
// ==========================================================================

/**
 * Main function that initializes the server, handles incoming connections,
 * and manages the thread pool. It runs in an infinite loop, waiting for client
 * connections and processing them using worker threads.
 *
 * @param argc Number of command-line arguments
 * @param argv Array of command-line arguments
 * @return int Exit code (0 for success)
 */
int main(int argc, char *argv[]) {
    // Step 1: Set up signal handling for SIGINT (Ctrl + C)
    signal(SIGINT, handle_shutdown);  // Register shutdown handler
    signal(SIGPIPE, SIG_IGN); // add to handle client issues

    // Add SIGHUP handling for log rotation
    signal(SIGHUP, handle_sighup);

    // Step 2: Load server configuration from config.json
    load_config("config.json");

    // Log the server startup
    log_message("info", "Starting server...");

    // Step 3: Initialize the server socket
    server_sock = socket(AF_INET, SOCK_STREAM, 0);  // Create the server socket
    if (server_sock < 0) {
        log_message("error", "Failed to create socket");
        exit(1);  // Exit if socket creation fails
    }

    // Configure the server address (IP and port)
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;                // Use IPv4
    server_addr.sin_port = htons(port);              // Port loaded from config
    server_addr.sin_addr.s_addr = inet_addr(ip_address);  // IP loaded from config

    // Step 4: Set socket options (SO_REUSEADDR) to reuse the address
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_message("error", "Failed to set socket options");
        close(server_sock);
        exit(1);  // Exit if setting socket options fails
    }

    // Step 5: Bind the socket to the specified IP and port
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log_message("error", "Binding socket failed");
        close(server_sock);  // Close the socket if binding fails
        exit(1);  // Exit if socket binding fails
    }

    // Step 6: Start listening for incoming connections
    if (listen(server_sock, 10) < 0) {
        log_message("error", "Failed to listen on socket");
        close(server_sock);  // Close the socket if listening fails
        exit(1);  // Exit if listen fails
    }

    log_message("info", "Server started successfully");

    // Step 7: Initialize the worker thread pool
    initialize_thread_pool();  // Creates the worker threads and sets up the pool

    // Step 8: Initialize the file descriptor sets for select()
    fd_set master_set, read_fds;
    FD_ZERO(&master_set);  // Clear the master set
    FD_SET(server_sock, &master_set);  // Add the server socket to the master set

    // Step 9: Main server loop to handle incoming connections
    while (1) {
        read_fds = master_set;  // Copy the master set to the read set for select()

        // Wait for incoming connections using select()
        select(server_sock + 1, &read_fds, NULL, NULL, NULL);

        // Check if there is an incoming connection on the server socket
        if (FD_ISSET(server_sock, &read_fds)) {
            // Accept the new client connection
            int client_sock = accept(server_sock, NULL, NULL);

            int backoff_delay = 100000;  // Initial backoff delay (100ms)

            // Lock the job queue for thread safety
            pthread_mutex_lock(&lock);
            // Check if the job queue is full (circular buffer logic)
            while ((queue_back + 1) % MAX_QUEUE_SIZE == queue_front) {
                // If the job queue is full, apply exponential backoff
                log_message("error", "Job queue full. Applying backoff before dropping request.");
                usleep(backoff_delay);  // Sleep for the backoff delay (in microseconds)

                // Double the backoff delay for the next retry
                backoff_delay *= 2;

                // Optional: Set a maximum backoff delay (e.g., 3 seconds)
                if (backoff_delay > 3000000) {  // 3 seconds in microseconds
                    break;  // Give up if the delay grows too large
                }
            }

            // Check again if there is space in the queue after backoff
            if ((queue_back + 1) % MAX_QUEUE_SIZE != queue_front) {
                // Enqueue the new job (client socket) and signal a worker thread
                enqueue_job(client_sock);
                sem_post(&job_semaphore);  // Signal that a job is available
            } else {
                // If the job queue is still full, send a 503 error to the client
                log_message("error", "Job queue full. Dropping request after backoff.");
                dprintf(client_sock, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n\r\nServer overloaded. Please try again later.\n");
                close(client_sock);  // Close the client socket
            }
            // Unlock the job queue after the operation
            pthread_mutex_unlock(&lock);
        }
    }

    // This point is never reached due to signal handling (shutdown is handled by SIGINT)
    return 0;
}
