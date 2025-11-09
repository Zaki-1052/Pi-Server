#include "config.h"
#include "logging.h"
#include <strings.h>

// ==========================================================================
// Configuration Management Implementation
// ==========================================================================

// Global configuration
config_t config;

// Global state flags
bool g_server_running = false;
volatile bool g_shutdown_flag = false;

// Set default configuration values
void set_default_config(void) {
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
    config.log_level = LOG_LEVEL_INFO;
    strncpy(config.logging_output, "console", sizeof(config.logging_output));
    config.logging.level = LOG_LEVEL_INFO;

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
        config.memory_limit = (size_t)json_object_get_int64(j_memory_limit) * 1024 * 1024 * 1024;
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
                config.memory_limit = (size_t)atol(optarg) * 1024 * 1024 * 1024;
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
        config.max_digits = strtoul(argv[optind], NULL, 10);
    }
}
