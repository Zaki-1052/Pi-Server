#include "common.h"
#include "job_manager.h"
#include "utils.h"
#include "logging.h"

// ==========================================================================
// Job Management for Asynchronous Calculations
// ==========================================================================

//
// Below, we use the previously defined calculation_job structure
//

// Jobs array and management
calculation_job jobs[MAX_JOBS];
pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
bool jobs_initialized = false;

// Initialize jobs array
void initialize_jobs() {
    pthread_mutex_lock(&jobs_lock);
    
    if (!jobs_initialized) {
        for (int i = 0; i < MAX_JOBS; i++) {
            jobs[i].job_id[0] = '\0';  // Empty job ID indicates unused slot
            if (pthread_mutex_init(&jobs[i].lock, NULL) != 0) {
                log_message("critical", "pthread_mutex_init failed for jobs[%d].lock in initialize_jobs: %s. Exiting.", i, strerror(errno));
                // This is a critical failure for job management.
                exit(EXIT_FAILURE);
            }
        }
        jobs_initialized = true;
    }
    
    pthread_mutex_unlock(&jobs_lock);
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

// Find or create a job
int find_or_create_job(const char* job_id) {
    pthread_mutex_lock(&jobs_lock);
    
    // If job_id is provided, try to find it
    if (job_id && job_id[0] != '\0') {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (strcmp(jobs[i].job_id, job_id) == 0) {
                pthread_mutex_unlock(&jobs_lock);
                return i;
            }
        }
        // Job not found
        pthread_mutex_unlock(&jobs_lock);
        return -1;
    }
    
    // Find an empty slot for a new job
    int oldest_completed_idx = -1;
    time_t oldest_completed_time = time(NULL);
    
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].job_id[0] == '\0') {
            // Found an empty slot
            generate_uuid(jobs[i].job_id, sizeof(jobs[i].job_id));
            jobs[i].creation_time = time(NULL);
            pthread_mutex_unlock(&jobs_lock);
            return i;
        }
        
        // Track the oldest completed job in case we need to overwrite one
        if (jobs[i].status == JOB_STATUS_COMPLETED || jobs[i].status == JOB_STATUS_FAILED) {
            if (jobs[i].end_time < oldest_completed_time) {
                oldest_completed_time = jobs[i].end_time;
                oldest_completed_idx = i;
            }
        }
    }
    
    // No empty slots, check if we can reuse an old completed job
    if (oldest_completed_idx >= 0 && 
        difftime(time(NULL), oldest_completed_time) > MAX_JOB_AGE_SECONDS) {
        
        // Clean up the old job
        if (jobs[oldest_completed_idx].result_file[0] != '\0') {
            if (unlink(jobs[oldest_completed_idx].result_file) != 0) {
                log_message("warning", "unlink failed for old job result file %s in find_or_create_job: %s", 
                            jobs[oldest_completed_idx].result_file, strerror(errno));
            }
        }
        
        // Reinitialize the job
        generate_uuid(jobs[oldest_completed_idx].job_id, sizeof(jobs[oldest_completed_idx].job_id));
        jobs[oldest_completed_idx].creation_time = time(NULL);
        jobs[oldest_completed_idx].status = JOB_STATUS_QUEUED;
        jobs[oldest_completed_idx].result_file[0] = '\0';
        jobs[oldest_completed_idx].error_message[0] = '\0';
        
        pthread_mutex_unlock(&jobs_lock);
        return oldest_completed_idx;
    }
    
    // No slots available
    pthread_mutex_unlock(&jobs_lock);
    return -1;
}

// Update job status
void update_job_status(int job_idx, job_status_t status, double progress, const char* error_message) {
    if (job_idx < 0 || job_idx >= MAX_JOBS) return;
    
    pthread_mutex_lock(&jobs[job_idx].lock);
    
    jobs[job_idx].status = status;
    jobs[job_idx].progress = progress;
    
    if (status == JOB_STATUS_RUNNING && jobs[job_idx].start_time == 0) {
        jobs[job_idx].start_time = time(NULL);
    } else if ((status == JOB_STATUS_COMPLETED || status == JOB_STATUS_FAILED) && 
               jobs[job_idx].end_time == 0) {
        jobs[job_idx].end_time = time(NULL);
    }
    
    if (error_message) {
        snprintf(jobs[job_idx].error_message, sizeof(jobs[job_idx].error_message), "%s", error_message);
        // sizeof(jobs[job_idx].error_message) ensures snprintf does not write past the buffer
        // and null-terminates it.
    }
    
    pthread_mutex_unlock(&jobs[job_idx].lock);
}
