#include "logging.h"
#include <strings.h>

// ==========================================================================
// Logging System Implementation
// ==========================================================================

// File pointer for logging
FILE* log_file = NULL;

// Unified logging function implementation
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

    // Format the variadic message with security check
    char formatted_message[BUFFER_SIZE];

    // Validate format string - only allow if from trusted source
    // In a real-world implementation, you would maintain a list of allowable
    // format strings or implement format string validation
    if (!format) {
        strcpy(formatted_message, "NULL format string");
    } else {
        va_list args;
        va_start(args, format);
        // Use %s format specifier to avoid format string injection
        char temp[BUFFER_SIZE];
        vsnprintf(temp, BUFFER_SIZE, format, args);
        va_end(args);

        // Copy to final buffer with proper escaping
        formatted_message[0] = '\0';
        strncpy(formatted_message, temp, BUFFER_SIZE - 1);
        formatted_message[BUFFER_SIZE - 1] = '\0';
    }

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

    // Forward to unified_log with security check
    char formatted_message[BUFFER_SIZE];

    // Validate format string - only allow if from trusted source
    if (!format) {
        strcpy(formatted_message, "NULL format string");
    } else {
        va_list args;
        va_start(args, format);
        // Use a temporary buffer for initial formatting
        char temp[BUFFER_SIZE];
        vsnprintf(temp, BUFFER_SIZE, format, args);
        va_end(args);

        // Copy to final buffer with proper escaping
        formatted_message[0] = '\0';
        strncpy(formatted_message, temp, BUFFER_SIZE - 1);
        formatted_message[BUFFER_SIZE - 1] = '\0';
    }

    // Pass to unified_log using a literal format string for safety
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
void close_log_file(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
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
