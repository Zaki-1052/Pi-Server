#include "utils.h"
#include "logging.h"

// ==========================================================================
// Utility Functions Implementation
// ==========================================================================

// Safe path joining that prevents buffer overflow
int safe_path_join(char *dest, size_t dest_size, const char *base, const char *component) {
    if (!dest || !base || !component || dest_size == 0) {
        return -1;
    }

    size_t base_len = strlen(base);
    size_t comp_len = strlen(component);
    size_t needed_len;

    // Check if base already ends with a separator
    if (base_len > 0 && base[base_len-1] == '/') {
        needed_len = base_len + comp_len + 1; // +1 for null terminator
    } else {
        needed_len = base_len + 1 + comp_len + 1; // +1 for separator, +1 for null terminator
    }

    // Check if it fits in the destination buffer
    if (needed_len > dest_size) {
        // Would truncate, so return error
        if (dest_size > 0) {
            dest[0] = '\0'; // Ensure dest is still a valid string
        }
        return -1;
    }

    // Safe to copy
    strcpy(dest, base);

    // Add separator if needed
    if (base_len > 0 && base[base_len-1] != '/') {
        strcat(dest, "/");
    }

    // Add component
    strcat(dest, component);

    return 0;
}

// Create directories recursively (similar to mkdir -p)
int mkdir_recursive(const char* path, mode_t mode) {
    char tmp[MAX_PATH];
    char* p = NULL;
    size_t len;

    // Copy path to avoid modifying original
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    // Remove trailing slash
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }

    // Check if path exists already
    struct stat st;
    if (stat(tmp, &st) == 0) {
        return 0;  // Already exists
    }

    // Find parent directory
    p = strrchr(tmp, '/');
    if (p) {
        // Temporarily terminate string at parent
        *p = 0;

        // Create parent directories first
        int ret = mkdir_recursive(tmp, mode);
        if (ret != 0 && errno != EEXIST) {
            return ret;
        }

        // Restore slash
        *p = '/';
    }

    // Create this directory
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create directory: %s (error: %s)\n",
                tmp, strerror(errno));
        return -1;
    }

    return 0;
}

// Generate a simple UUID for job IDs
void generate_uuid(char* uuid, size_t size) {
    static const char hex_chars[] = "0123456789abcdef";

    if (size < UUID_STR_LEN) return;

    // Format: 8-4-4-4-12 (8 chars, dash, 4 chars, dash, etc.)
    unsigned char random_bytes[UUID_NUM_BYTES];

    // Get random bytes
    FILE* urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        size_t bytes_read = fread(random_bytes, 1, UUID_NUM_BYTES, urandom);
        if (bytes_read != UUID_NUM_BYTES) {
            // Fall back to time-based if read fails
            time_t now = time(NULL);
            unsigned int seed = (unsigned int)now;
            for (int i = 0; i < UUID_NUM_BYTES; i++) { // Use UUID_NUM_BYTES
                random_bytes[i] = (unsigned char)(rand_r(&seed) % 256);
            }
        }
        if (fclose(urandom) != 0) {
            log_message("warning", "fclose failed for /dev/urandom in generate_uuid: %s", strerror(errno));
            // Continue, as we might have gotten enough random bytes or will use fallback
        }
    } else {
        log_message("warning", "fopen failed for /dev/urandom in generate_uuid: %s. Using fallback.", strerror(errno));
        // Fallback to time-based if /dev/urandom not available
        time_t now = time(NULL);
        unsigned int seed = (unsigned int)now;
        for (int i = 0; i < 16; i++) {
            random_bytes[i] = (unsigned char)(rand_r(&seed) % 256);
        }
    }

    // Format the UUID
    int pos = 0;
    for (int i = 0; i < UUID_NUM_BYTES; i++) { // Use UUID_NUM_BYTES
        // Add dashes at positions 8, 13, 18, 23 (corresponding to byte indices 4, 6, 8, 10)
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            uuid[pos++] = '-';
        }

        uuid[pos++] = hex_chars[random_bytes[i] >> 4];
        uuid[pos++] = hex_chars[random_bytes[i] & 0x0F];
    }

    uuid[pos] = '\0';
}
