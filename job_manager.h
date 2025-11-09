#ifndef JOB_MANAGER_H
#define JOB_MANAGER_H

#include "common.h"

// ==========================================================================
// Job Management - Header
// ==========================================================================

// Job management functions
void initialize_jobs(void);
int find_or_create_job(const char* job_id);
void update_job_status(int job_idx, job_status_t status, double progress, const char* error_message);

#endif // JOB_MANAGER_H
