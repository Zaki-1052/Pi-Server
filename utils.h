#ifndef UTILS_H
#define UTILS_H

#include "common.h"

// ==========================================================================
// Utility Functions
// ==========================================================================

// Safe path joining that prevents buffer overflow
int safe_path_join(char* dest, size_t dest_size, const char* path1, const char* path2);

// Recursively create directories
int mkdir_recursive(const char* path, mode_t mode);

// Generate a UUID for job IDs
void generate_uuid(char* uuid, size_t size);

#endif // UTILS_H
