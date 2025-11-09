#ifndef LOGGING_H
#define LOGGING_H

#include "common.h"

// ==========================================================================
// Logging System
// ==========================================================================

// Unified logging function
void unified_log(int level, const char* format, ...);

// Legacy log message function (for backward compatibility)
void log_message(const char* level, const char* format, ...);

// Open log file based on configuration
void open_log_file(const char* log_path);

// Close log file if open
void close_log_file(void);

// Convert string log level to integer log level
int convert_log_level_string_to_int(const char* level);

#endif // LOGGING_H
