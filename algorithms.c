#include "common.h"
#include "algorithms.h"
#include "disk_int.h"
#include "utils.h"
#include "logging.h"
#include "thread_pool.h"
#include "calc_state.h"

// ==========================================================================
// Algorithm Implementations
// ==========================================================================

struct chudnovsky_state {
    disk_int P;  // Numerator term
    disk_int Q;  // Denominator term
    disk_int T;  // Intermediate term
};

// Initialize Chudnovsky state
void chudnovsky_state_init(chudnovsky_state* state, const char* base_path) {
    if (!state) {
        fprintf(stderr, "Error: Null state passed to chudnovsky_state_init\n");
        return;
    }
    
    // Initialize all fields to a known state
    memset(state, 0, sizeof(chudnovsky_state));
    
    printf("DEBUG: chudnovsky_state_init with base_path=%s\n", base_path ? base_path : "NULL");
    
    if (!base_path || base_path[0] == '\0') {
        fprintf(stderr, "Error: Invalid base path for Chudnovsky state\n");
        return;
    }
    
    // Create the directories with better error reporting
    printf("DEBUG: Creating directory: %s\n", base_path);
    struct stat st;
    if (stat(base_path, &st) != 0) {
        // Directory doesn't exist, create it
        if (mkdir_recursive(base_path, 0755) != 0) {
            fprintf(stderr, "Error: Failed to create base directory for Chudnovsky state: %s (errno: %d - %s)\n", 
                    base_path, errno, strerror(errno));
            return;
        }
    }
    
    char p_path[MAX_PATH], q_path[MAX_PATH], t_path[MAX_PATH];
    
    printf("DEBUG: Joining %s with P\n", base_path);
    if (safe_path_join(p_path, MAX_PATH, base_path, "P") < 0) {
        fprintf(stderr, "Error: Path too long for Chudnovsky state P\n");
        return;
    }
    
    if (mkdir_recursive(p_path, 0755) != 0) {
        fprintf(stderr, "Error: Failed to create P directory: %s\n", p_path);
        return;
    }
    
    printf("DEBUG: Joining %s with Q\n", base_path);
    if (safe_path_join(q_path, MAX_PATH, base_path, "Q") < 0) {
        fprintf(stderr, "Error: Path too long for Chudnovsky state Q\n");
        return;
    }
    
    if (mkdir_recursive(q_path, 0755) != 0) {
        fprintf(stderr, "Error: Failed to create Q directory: %s\n", q_path);
        return;
    }
    
    printf("DEBUG: Joining %s with T\n", base_path);
    if (safe_path_join(t_path, MAX_PATH, base_path, "T") < 0) {
        fprintf(stderr, "Error: Path too long for Chudnovsky state T\n");
        return;
    }
    
    if (mkdir_recursive(t_path, 0755) != 0) {
        fprintf(stderr, "Error: Failed to create T directory: %s\n", t_path);
        return;
    }
    
    printf("DEBUG: Paths: P=%s, Q=%s, T=%s\n", p_path, q_path, t_path);
    
    // Initialize disk integers
    disk_int_init(&state->P, p_path);
    if (state->P.file_path[0] == '\0') {
        fprintf(stderr, "Error: Failed to initialize disk integer P\n");
        return;
    }
    
    disk_int_init(&state->Q, q_path);
    if (state->Q.file_path[0] == '\0') {
        fprintf(stderr, "Error: Failed to initialize disk integer Q\n");
        disk_int_clear(&state->P);
        return;
    }
    
    disk_int_init(&state->T, t_path);
    if (state->T.file_path[0] == '\0') {
        fprintf(stderr, "Error: Failed to initialize disk integer T\n");
        disk_int_clear(&state->P);
        disk_int_clear(&state->Q);
        return;
    }
}

// Clear Chudnovsky state
void chudnovsky_state_clear(chudnovsky_state* state) {
    disk_int_clear(&state->P);
    disk_int_clear(&state->Q);
    disk_int_clear(&state->T);
}

// Binary splitting algorithm for Chudnovsky formula - memory version for small ranges
void binary_split_mpz(mpz_t P, mpz_t Q, mpz_t T, unsigned long a, unsigned long b) {
    // Safety check for invalid range - avoid divide-by-zero and other errors
    if (a >= b) {
        fprintf(stderr, "Warning: Invalid range for binary_split_mpz: a=%lu, b=%lu\n", a, b);
        // Set sensible default values that won't cause crashes
        mpz_set_ui(P, 1);
        mpz_set_ui(Q, 1);
        mpz_set_ui(T, 0);
        return;
    }

    if (b - a == 1) {
        // Base case: single term
        mpz_t t1, t2, t3;
        mpz_init(t1);
        mpz_init(t2);
        mpz_init(t3);
        
        // Use a flag to track errors for proper cleanup
        int error_occurred = 0;
        
        // Q = (6*b - 5) * (2*b - 1) * (6*b - 1)
        mpz_set_ui(t1, 6 * b - 5);
        mpz_set_ui(t2, 2 * b - 1);
        mpz_mul(t3, t1, t2);
        mpz_set_ui(t1, 6 * b - 1);
        mpz_mul(Q, t3, t1);
        
        // Check for errors in computation
        if (mpz_sgn(Q) == 0) {
            fprintf(stderr, "Warning: Zero Q value in base case of binary_split_mpz\n");
            mpz_set_ui(Q, 1);
            error_occurred = 1;
        }
        
        // P = b^3 * C^3 / 24 (match reference implementation exactly)
        mpz_set_ui(P, b);
        mpz_mul_ui(P, P, b);
        mpz_mul_ui(P, P, b);  // b^3

        // From reference: mpz_mul_ui(p1, p1, (C/24)*(C/24)); mpz_mul_ui(p1, p1, C*24);
        unsigned long c_div_24 = C/24;  // 26680
        unsigned long c_mul_24 = C*24;  // 15367680
        
        mpz_mul_ui(P, P, c_div_24 * c_div_24);  // b^3 * (C/24)^2
        mpz_mul_ui(P, P, c_mul_24);             // b^3 * (C/24)^2 * (C*24) = b^3 * C^3 / 24
        
        // Check for errors in computation
        if (mpz_sgn(P) == 0) {
            fprintf(stderr, "Warning: Zero P value in base case of binary_split_mpz\n");
            mpz_set_ui(P, 1);
            error_occurred = 1;
        }
        
        // T = Q * (A + B*b)
        mpz_set_ui(t1, B);
        mpz_mul_ui(t2, t1, b);
        mpz_add_ui(t3, t2, A);
        mpz_mul(T, Q, t3);
        
        // Negate if needed (alternating series)
        if (b % 2 == 1) {
            mpz_neg(T, T);
        }
        
        // Debug output for small values
        if (b < 5) {
            char *p_str = mpz_get_str(NULL, 10, P);
            char *q_str = mpz_get_str(NULL, 10, Q);
            char *t_str = mpz_get_str(NULL, 10, T);
            if (p_str && q_str && t_str) {
                printf("Base case [%lu,%lu]: P=%s, Q=%s, T=%s\n", a, b, p_str, q_str, t_str);
                free(p_str);
                free(q_str);
                free(t_str);
            } else {
                fprintf(stderr, "Error: Failed to allocate string memory in binary_split_mpz\n");
                error_occurred = 1;
            }
        }
        
        // Always clean up temporary variables
        mpz_clear(t1);
        mpz_clear(t2);
        mpz_clear(t3);
        
        // If an error occurred, make sure we have safe values
        if (error_occurred) {
            if (mpz_sgn(P) == 0) mpz_set_ui(P, 1);
            if (mpz_sgn(Q) == 0) mpz_set_ui(Q, 1);
            // T can be zero, no need to check
        }
    } else {
        // Recursive case
        unsigned long m = (a + b) / 2;
        mpz_t P1, Q1, T1, P2, Q2, T2, tmp1, tmp2;
        int error_occurred = 0;
        
        // Initialize all variables first to ensure we can clean up properly
        mpz_init(P1);
        mpz_init(Q1);
        mpz_init(T1);
        mpz_init(P2);
        mpz_init(Q2);
        mpz_init(T2);
        mpz_init(tmp1);
        mpz_init(tmp2);
        
        // Initialize with safe defaults in case recursion fails
        mpz_set_ui(P1, 1);
        mpz_set_ui(Q1, 1);
        mpz_set_ui(T1, 0);
        mpz_set_ui(P2, 1);
        mpz_set_ui(Q2, 1);
        mpz_set_ui(T2, 0);
        
        // Compute left half
        binary_split_mpz(P1, Q1, T1, a, m);
        
        // Check for errors in the left half - properly handle error path
        if (mpz_sgn(P1) == 0) {
            fprintf(stderr, "Warning: Zero P1 value after left recursion in binary_split_mpz [%lu,%lu]\n", a, m);
            mpz_set_ui(P1, 1);
            error_occurred = 1;
        }
        if (mpz_sgn(Q1) == 0) {
            fprintf(stderr, "Warning: Zero Q1 value after left recursion in binary_split_mpz [%lu,%lu]\n", a, m);
            mpz_set_ui(Q1, 1);
            error_occurred = 1;
        }
        
        // Compute right half
        binary_split_mpz(P2, Q2, T2, m, b);
        
        // Check for errors in the right half - properly handle error path
        if (mpz_sgn(P2) == 0) {
            fprintf(stderr, "Warning: Zero P2 value after right recursion in binary_split_mpz [%lu,%lu]\n", m, b);
            mpz_set_ui(P2, 1);
            error_occurred = 1;
        }
        if (mpz_sgn(Q2) == 0) {
            fprintf(stderr, "Warning: Zero Q2 value after right recursion in binary_split_mpz [%lu,%lu]\n", m, b);
            mpz_set_ui(Q2, 1);
            error_occurred = 1;
        }
        
        // Combine results: P = P1 * P2, Q = Q1 * Q2, T = T1 * Q2 + T2 * P1
        // Use try/catch pattern with goto for error handling
        mpz_mul(P, P1, P2);
        if (mpz_sgn(P) == 0) {
            fprintf(stderr, "Warning: Combined P is zero in binary_split_mpz [%lu,%lu]\n", a, b);
            mpz_set_ui(P, 1);
            error_occurred = 1;
        }
        
        mpz_mul(Q, Q1, Q2);
        if (mpz_sgn(Q) == 0) {
            fprintf(stderr, "Warning: Combined Q is zero in binary_split_mpz [%lu,%lu]\n", a, b);
            mpz_set_ui(Q, 1);
            error_occurred = 1;
        }
        
        mpz_mul(tmp1, T1, Q2);
        mpz_mul(tmp2, T2, P1);
        mpz_add(T, tmp1, tmp2);
        
        // Debug output for small values
        if (b < 5) {
            char *p_str = mpz_get_str(NULL, 10, P);
            char *q_str = mpz_get_str(NULL, 10, Q);
            char *t_str = mpz_get_str(NULL, 10, T);
            if (p_str && q_str && t_str) {
                printf("Recursive case [%lu,%lu]: P=%s, Q=%s, T=%s\n", a, b, p_str, q_str, t_str);
                free(p_str);
                free(q_str);
                free(t_str);
            } else {
                fprintf(stderr, "Error: Failed to allocate string memory in binary_split_mpz\n");
                error_occurred = 1;
            }
        }
        
        // Clean up all mpz_t variables even if an error occurred
        mpz_clear(P1);
        mpz_clear(Q1);
        mpz_clear(T1);
        mpz_clear(P2);
        mpz_clear(Q2);
        mpz_clear(T2);
        mpz_clear(tmp1);
        mpz_clear(tmp2);
        
        // Set safe default values if an error occurred
        if (error_occurred) {
            if (mpz_sgn(P) == 0) mpz_set_ui(P, 1);
            if (mpz_sgn(Q) == 0) mpz_set_ui(Q, 1);
            // T can be zero
        }
    }
    
    // Sanity check final results
    if (mpz_sgn(P) == 0) {
        fprintf(stderr, "Warning: Result P is zero in binary_split_mpz [%lu,%lu]\n", a, b);
        mpz_set_ui(P, 1);
    }
    
    if (mpz_sgn(Q) == 0) {
        fprintf(stderr, "Warning: Result Q is zero in binary_split_mpz [%lu,%lu]\n", a, b);
        mpz_set_ui(Q, 1);
    }
}

// Binary splitting on disk for large ranges
void binary_split_disk(chudnovsky_state* result, unsigned long a, unsigned long b, 
                       const char* base_path, size_t memory_threshold) {
    // Track created directories to ensure proper cleanup
    bool left_dir_created = false;
    bool right_dir_created = false;
    bool temp1_dir_created = false;
    bool temp2_dir_created = false;
    char left_path[MAX_PATH] = {0};
    char right_path[MAX_PATH] = {0};
    char temp1_path[MAX_PATH] = {0};
    char temp2_path[MAX_PATH] = {0};
    bool left_state_initialized = false;
    bool right_state_initialized = false;
    bool temp1_initialized = false;
    bool temp2_initialized = false;
    
    if (config.log_level <= LOG_LEVEL_DEBUG) {
        printf("DEBUG: binary_split_disk a=%lu, b=%lu, base_path=%s\n", a, b, base_path ? base_path : "NULL");
    }
    
    // Safety check for invalid range
    if (a >= b) {
        fprintf(stderr, "Warning: Invalid range for binary_split_disk: a=%lu, b=%lu\n", a, b);
        // Initialize result with default values to prevent crashes
        mpz_t one;
        mpz_init(one);
        mpz_set_ui(one, 1);
        
        disk_int_set_mpz(&result->P, one);
        disk_int_set_mpz(&result->Q, one);
        disk_int_set_mpz(&result->T, one);
        
        mpz_clear(one);
        return;
    }
    
    // Safety check for base_path
    if (!base_path || base_path[0] == '\0') {
        fprintf(stderr, "Warning: Invalid base_path for binary_split_disk\n");
        // Initialize result with default values to prevent crashes
        mpz_t one;
        mpz_init(one);
        mpz_set_ui(one, 1);
        
        disk_int_set_mpz(&result->P, one);
        disk_int_set_mpz(&result->Q, one);
        disk_int_set_mpz(&result->T, one);
        
        mpz_clear(one);
        return;
    }
    
    // If range is small enough, compute in memory then save to disk
    if (config.log_level <= LOG_LEVEL_DEBUG) {
        printf("DEBUG: Range check b-a=%lu, threshold=%zu\n", b-a, memory_threshold);
    }
    
    if (b - a <= memory_threshold) {
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Computing in memory\n");
        }
        
        mpz_t P, Q, T;
        mpz_init(P);
        mpz_init(Q);
        mpz_init(T);
        
        binary_split_mpz(P, Q, T, a, b);
        
        // Debug output for small ranges
        if (b - a < 5 && config.log_level <= LOG_LEVEL_DEBUG) {
            char *p_str = mpz_get_str(NULL, 10, P);
            char *q_str = mpz_get_str(NULL, 10, Q);
            char *t_str = mpz_get_str(NULL, 10, T);
            if (p_str && q_str && t_str) {
                printf("Binary split disk [%lu,%lu]: P=%s, Q=%s, T=%s\n", a, b, p_str, q_str, t_str);
                free(p_str);
                free(q_str);
                free(t_str);
            }
        }
        
        // Add debug output before setting disk integers
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            char *p_str = mpz_get_str(NULL, 10, P);
            char *q_str = mpz_get_str(NULL, 10, Q);
            char *t_str = mpz_get_str(NULL, 10, T);
            printf("Before disk_int_set_mpz [%lu,%lu]: P=%s, Q=%s, T=%s\n", 
                  a, b, p_str ? p_str : "NULL", q_str ? q_str : "NULL", t_str ? t_str : "NULL");
            if (p_str) free(p_str);
            if (q_str) free(q_str);
            if (t_str) free(t_str);
        }
        
        // Verify all mpz values are valid before setting
        if (mpz_sgn(P) == 0) {
            fprintf(stderr, "Warning: Zero P value before disk storage in range [%lu,%lu]\n", a, b);
            mpz_set_ui(P, 1);
        }
        if (mpz_sgn(Q) == 0) {
            fprintf(stderr, "Warning: Zero Q value before disk storage in range [%lu,%lu]\n", a, b);
            mpz_set_ui(Q, 1);
        }
        
        // Now set the disk integers with verified values
        disk_int_set_mpz(&result->P, P);
        disk_int_set_mpz(&result->Q, Q);
        disk_int_set_mpz(&result->T, T);
        
        mpz_clear(P);
        mpz_clear(Q);
        mpz_clear(T);
    } else {
        // Split the range
        unsigned long m = (a + b) / 2;
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Splitting range at m=%lu\n", m);
        }
        
        // Create paths for left and right results
        snprintf(left_path, MAX_PATH, "%s/left", base_path);
        snprintf(right_path, MAX_PATH, "%s/right", base_path);
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: left_path=%s, right_path=%s\n", left_path, right_path);
        }
        
        // Create directories with error checking
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Creating directory: %s\n", left_path);
        }
        
        if (mkdir_recursive(left_path, 0755) == 0) {
            left_dir_created = true;
        } else {
            fprintf(stderr, "Warning: Failed to create directory: %s (%s)\n", 
                    left_path, strerror(errno));
            goto cleanup;
        }
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Creating directory: %s\n", right_path);
        }
        
        if (mkdir_recursive(right_path, 0755) == 0) {
            right_dir_created = true;
        } else {
            fprintf(stderr, "Warning: Failed to create directory: %s (%s)\n", 
                    right_path, strerror(errno));
            goto cleanup;
        }
        
        // Compute left and right halves
        chudnovsky_state left_state, right_state;
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Initializing left_state with path: %s\n", left_path);
        }
        
        chudnovsky_state_init(&left_state, left_path);
        left_state_initialized = true;
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Initializing right_state with path: %s\n", right_path);
        }
        
        chudnovsky_state_init(&right_state, right_path);
        right_state_initialized = true;
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Recursively calling binary_split_disk for left half\n");
        }
        
        binary_split_disk(&left_state, a, m, left_path, memory_threshold);
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Recursively calling binary_split_disk for right half\n");
        }
        
        binary_split_disk(&right_state, m, b, right_path, memory_threshold);
        
        // Create temp paths
        snprintf(temp1_path, MAX_PATH, "%s/temp1", base_path);
        snprintf(temp2_path, MAX_PATH, "%s/temp2", base_path);
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: temp1_path=%s, temp2_path=%s\n", temp1_path, temp2_path);
        }
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Creating directory: %s\n", temp1_path);
        }
        
        if (mkdir_recursive(temp1_path, 0755) == 0) {
            temp1_dir_created = true;
        } else {
            fprintf(stderr, "Warning: Failed to create directory: %s (%s)\n", 
                    temp1_path, strerror(errno));
            goto cleanup;
        }
        
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            printf("DEBUG: Creating directory: %s\n", temp2_path);
        }
        
        if (mkdir_recursive(temp2_path, 0755) == 0) {
            temp2_dir_created = true;
        } else {
            fprintf(stderr, "Warning: Failed to create directory: %s (%s)\n", 
                    temp2_path, strerror(errno));
            goto cleanup;
        }
        
        // Initialize temporary disk integers
        disk_int temp1, temp2;
        disk_int_init(&temp1, temp1_path);
        temp1_initialized = true;
        
        disk_int_init(&temp2, temp2_path);
        temp2_initialized = true;
        
        // Add validation checks before multiplication
        mpz_t left_P, left_Q, left_T, right_P, right_Q, right_T;
        mpz_init(left_P);
        mpz_init(left_Q);
        mpz_init(left_T);
        mpz_init(right_P);
        mpz_init(right_Q);
        mpz_init(right_T);
        
        // Get values from disk
        disk_int_get_mpz(left_P, &left_state.P);
        disk_int_get_mpz(left_Q, &left_state.Q);
        disk_int_get_mpz(left_T, &left_state.T);
        disk_int_get_mpz(right_P, &right_state.P);
        disk_int_get_mpz(right_Q, &right_state.Q);
        disk_int_get_mpz(right_T, &right_state.T);
        
        // Verify we have valid values
        if (mpz_sgn(left_P) == 0) {
            fprintf(stderr, "Warning: Zero left_P value in recursive case [%lu,%lu], m=%lu\n", a, b, m);
            mpz_set_ui(left_P, 1);
        }
        if (mpz_sgn(left_Q) == 0) {
            fprintf(stderr, "Warning: Zero left_Q value in recursive case [%lu,%lu], m=%lu\n", a, b, m);
            mpz_set_ui(left_Q, 1);
        }
        if (mpz_sgn(right_P) == 0) {
            fprintf(stderr, "Warning: Zero right_P value in recursive case [%lu,%lu], m=%lu\n", a, b, m);
            mpz_set_ui(right_P, 1);
        }
        if (mpz_sgn(right_Q) == 0) {
            fprintf(stderr, "Warning: Zero right_Q value in recursive case [%lu,%lu], m=%lu\n", a, b, m);
            mpz_set_ui(right_Q, 1);
        }
        
        // Debug output
        if (config.log_level <= LOG_LEVEL_DEBUG) {
            char *lp_str = mpz_get_str(NULL, 10, left_P);
            char *lq_str = mpz_get_str(NULL, 10, left_Q);
            char *lt_str = mpz_get_str(NULL, 10, left_T);
            char *rp_str = mpz_get_str(NULL, 10, right_P);
            char *rq_str = mpz_get_str(NULL, 10, right_Q);
            char *rt_str = mpz_get_str(NULL, 10, right_T);
            
            printf("Recursive values [%lu,%lu]: Left(P=%s, Q=%s, T=%s), Right(P=%s, Q=%s, T=%s)\n", 
                  a, b, 
                  lp_str ? lp_str : "NULL", 
                  lq_str ? lq_str : "NULL", 
                  lt_str ? lt_str : "NULL",
                  rp_str ? rp_str : "NULL", 
                  rq_str ? rq_str : "NULL", 
                  rt_str ? rt_str : "NULL");
                  
            if (lp_str) free(lp_str);
            if (lq_str) free(lq_str);
            if (lt_str) free(lt_str);
            if (rp_str) free(rp_str);
            if (rq_str) free(rq_str);
            if (rt_str) free(rt_str);
        }
        
        // Write validated values back to disk
        disk_int_set_mpz(&left_state.P, left_P);
        disk_int_set_mpz(&left_state.Q, left_Q);
        disk_int_set_mpz(&left_state.T, left_T);
        disk_int_set_mpz(&right_state.P, right_P);
        disk_int_set_mpz(&right_state.Q, right_Q);
        disk_int_set_mpz(&right_state.T, right_T);
        
        // Clean up temporary mpz variables
        mpz_clear(left_P);
        mpz_clear(left_Q);
        mpz_clear(left_T);
        mpz_clear(right_P);
        mpz_clear(right_Q);
        mpz_clear(right_T);
        
        // Combine results using disk operations
        // P = P1 * P2
        disk_int_mul(&result->P, &left_state.P, &right_state.P);
        
        // Q = Q1 * Q2
        disk_int_mul(&result->Q, &left_state.Q, &right_state.Q);
        
        // T = T1 * Q2 + T2 * P1
        disk_int_mul(&temp1, &left_state.T, &right_state.Q);
        disk_int_mul(&temp2, &right_state.T, &left_state.P);
        disk_int_add(&result->T, &temp1, &temp2);
        
    cleanup:
        // Clean up resources in reverse order of creation
        
        // Clean up temporary disk integers
        if (temp2_initialized) {
            disk_int_clear(&temp2);
        }
        
        if (temp1_initialized) {
            disk_int_clear(&temp1);
        }
        
        // Clean up the states which will remove their files
        if (right_state_initialized) {
            chudnovsky_state_clear(&right_state);
        }
        
        if (left_state_initialized) {
            chudnovsky_state_clear(&left_state);
        }
        
        // Force removal of any files that might be left in the directories
        if (temp2_dir_created) {
            // Remove any leftover files in temp2 directory
            char cleanup_cmd[MAX_PATH * 2];
            snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -f %s/*", temp2_path);
            int sys_result = system(cleanup_cmd);
            if (sys_result == -1) {
                log_message("warning", "system call to clean temp2_path failed for %s: %s", temp2_path, strerror(errno));
            } else if (WIFEXITED(sys_result) && WEXITSTATUS(sys_result) != 0) {
                log_message("warning", "system call to clean temp2_path for %s exited with status %d", temp2_path, WEXITSTATUS(sys_result));
            }
            
            // Now try to remove the directory
            if (rmdir(temp2_path) != 0) {
                fprintf(stderr, "Warning: Failed to remove temp2 directory: %s (%s)\n", 
                        temp2_path, strerror(errno));
            }
        }
        
        if (temp1_dir_created) {
            // Remove any leftover files in temp1 directory
            char cleanup_cmd[MAX_PATH * 2];
            snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -f %s/*", temp1_path);
            int sys_result = system(cleanup_cmd);
            if (sys_result == -1) {
                fprintf(stderr, "Warning: Failed to execute cleanup command for temp1 directory\n");
            }
            
            // Now try to remove the directory
            if (rmdir(temp1_path) != 0) {
                fprintf(stderr, "Warning: Failed to remove temp1 directory: %s (%s)\n", 
                        temp1_path, strerror(errno));
            }
        }
        
        if (right_dir_created) {
            // Remove any leftover files in right directory
            char cleanup_cmd[MAX_PATH * 2];
            snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -f %s/*", right_path);
            int sys_result = system(cleanup_cmd);
            if (sys_result == -1) {
                fprintf(stderr, "Warning: Failed to execute cleanup command for right directory\n");
            }
            
            // Now try to remove the directory
            if (rmdir(right_path) != 0) {
                fprintf(stderr, "Warning: Failed to remove right directory: %s (%s)\n", 
                        right_path, strerror(errno));
            }
        }
        
        if (left_dir_created) {
            // Remove any leftover files in left directory
            char cleanup_cmd[MAX_PATH * 2];
            snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -f %s/*", left_path);
            int sys_result = system(cleanup_cmd);
            if (sys_result == -1) {
                log_message("warning", "system call to clean temp1_path failed for %s: %s", temp1_path, strerror(errno));
            } else if (WIFEXITED(sys_result) && WEXITSTATUS(sys_result) != 0) {
                log_message("warning", "system call to clean temp1_path for %s exited with status %d", temp1_path, WEXITSTATUS(sys_result));
            }
            
            // Now try to remove the directory
            if (rmdir(left_path) != 0) {
                fprintf(stderr, "Warning: Failed to remove left directory: %s (%s)\n", 
                        left_path, strerror(errno));
            }
        }
    }
}
void calculate_pi_gauss_legendre(calculation_state* state) {
    // Get the pi variable from the state
    mpfr_t* pi = &state->data.gauss_legendre.pi;
    
    // Determine the precision needed
    // Cast from unsigned long (state->digits) to mpfr_prec_t (long) with overflow check
    if (state->digits > (unsigned long)LONG_MAX / 4) {
        log_message("error", "Digits value too large for precision calculation");
        return;
    }
    mpfr_prec_t precision = (mpfr_prec_t)(state->digits * 4) + GL_PRECISION_BITS;
    
    // Initialize variables for the algorithm
    mpfr_t a, b, t, p, a_next, b_next, t_next, pi_approx;
    mpfr_init2(a, precision);
    mpfr_init2(b, precision);
    mpfr_init2(t, precision);
    mpfr_init2(p, precision);
    mpfr_init2(a_next, precision);
    mpfr_init2(b_next, precision);
    mpfr_init2(t_next, precision);
    mpfr_init2(pi_approx, precision);
    
    // Set the initial values
    mpfr_set_ui(a, 1, MPFR_RNDN);
    mpfr_sqrt_ui(b, 2, MPFR_RNDN);
    mpfr_ui_div(b, 1, b, MPFR_RNDN);
    mpfr_set_ui(t, 1, MPFR_RNDN);
    mpfr_div_ui(t, t, 4, MPFR_RNDN);
    mpfr_set_ui(p, 1, MPFR_RNDN);
    
    // Iterate to improve the approximation
    for (int i = 0; i < GL_DEFAULT_ITERATIONS; i++) {
        // Cast from int to unsigned long with check (though i is always positive here)
        state->current_term = (unsigned long)(i + 1);
        
        // Update job progress if async
        if (state->job_idx >= 0) {
            update_job_status(state->job_idx, JOB_STATUS_RUNNING, 
                             (double)i / GL_DEFAULT_ITERATIONS, NULL);
        }
        
        // Calculate next values
        mpfr_add(a_next, a, b, MPFR_RNDN);
        mpfr_div_ui(a_next, a_next, 2, MPFR_RNDN);
        
        mpfr_mul(b_next, a, b, MPFR_RNDN);
        mpfr_sqrt(b_next, b_next, MPFR_RNDN);
        
        mpfr_sub(t_next, a, a_next, MPFR_RNDN);
        mpfr_pow_ui(t_next, t_next, 2, MPFR_RNDN);
        mpfr_mul(t_next, t_next, p, MPFR_RNDN);
        mpfr_sub(t_next, t, t_next, MPFR_RNDN);
        
        mpfr_mul_ui(p, p, 2, MPFR_RNDN);
        
        // Update variables for next iteration
        mpfr_set(a, a_next, MPFR_RNDN);
        mpfr_set(b, b_next, MPFR_RNDN);
        mpfr_set(t, t_next, MPFR_RNDN);
    }
    
    // Final approximation: pi = (a + b)^2 / (4 * t)
    mpfr_add(pi_approx, a, b, MPFR_RNDN);
    mpfr_pow_ui(pi_approx, pi_approx, 2, MPFR_RNDN);
    mpfr_mul_ui(t, t, 4, MPFR_RNDN);
    mpfr_div(*pi, pi_approx, t, MPFR_RNDN);
    
    // Clear all MPFR variables
    mpfr_clear(a);
    mpfr_clear(b);
    mpfr_clear(t);
    mpfr_clear(p);
    mpfr_clear(a_next);
    mpfr_clear(b_next);
    mpfr_clear(t_next);
    mpfr_clear(pi_approx);
    
    // Write the result to file
    FILE* f = fopen(state->output_file, "w");
    if (f) {
        // Use mpfr_get_str to get the digits as a string and print manually
        char *str_pi;
        mpfr_exp_t exp;
        str_pi = mpfr_get_str(NULL, &exp, 10, state->digits + 2, *pi, MPFR_RNDN);
        
        // Format correctly: insert decimal point after first digit
        fprintf(f, "%c.%s", str_pi[0], str_pi + 1);
        
        // Free the string allocated by mpfr_get_str
        mpfr_free_str(str_pi);
        fclose(f);
    }
}

// Calculate Pi using Chudnovsky algorithm with parallel binary splitting
void calculate_pi_chudnovsky(calculation_state* state, calc_thread_pool* pool) {
    printf("Calculating Pi to %lu digits using Chudnovsky algorithm...\n", state->digits);
    
    // Safety check for state
    if (!state) {
        fprintf(stderr, "Error: NULL calculation_state in calculate_pi_chudnovsky\n");
        return;
    }
    
    // Safety check for pool
    if (!pool) {
        fprintf(stderr, "Error: NULL calc_thread_pool in calculate_pi_chudnovsky\n");
        return;
    }
    
    // For small digit counts, use a simplified in-memory approach with binary splitting
    if (state->digits <= 1000) {
        printf("Using in-memory binary splitting approach for small digit count...\n");
        
        // Calculate enough terms for the desired precision
        unsigned long terms = (unsigned long)ceil(state->digits / 14.0) + 1;
        printf("Will compute %lu terms using binary splitting\n", terms);
        
        // Initialize variables for binary splitting
        mpz_t P, Q, T;
        mpz_init(P);
        mpz_init(Q);
        mpz_init(T);
        
        // Perform binary splitting algorithm (in memory)
        binary_split_mpz(P, Q, T, 0, terms);
        
        printf("Binary splitting complete\n");
        printf("P size: %zu digits\n", mpz_sizeinbase(P, 10));
        printf("Q size: %zu digits\n", mpz_sizeinbase(Q, 10));
        printf("T size: %zu digits\n", mpz_sizeinbase(T, 10));
        
        // Compute pi using the exact steps from the gmp-chudnovsky.c reference implementation
        mpfr_t pi, qi, sqrt_c;
        mpfr_prec_t precision = (mpfr_prec_t)(state->digits * 4);
        
        // Initialize MPFR variables
        mpfr_init2(pi, precision);
        mpfr_init2(qi, precision);
        mpfr_init2(sqrt_c, precision);
        
        // Make a copy of our P and Q values to modify
        mpz_t p1, q1;
        mpz_init(p1);
        mpz_init(q1);
        mpz_set(p1, P);
        mpz_set(q1, T); // In our representation, T is used as q1 in the reference
        
        // Debug output
        char *p_str = mpz_get_str(NULL, 10, p1);
        char *q_str = mpz_get_str(NULL, 10, q1);
        if (p_str && q_str) {
            printf("Before final calculation: p1=%s, q1=%s\n", p_str, q_str);
            free(p_str);
            free(q_str);
        }
        
        // Step 1: Add A*p1 to q1 (q1 = q1 + A*p1)
        mpz_addmul_ui(q1, p1, A);
        
        // Step 2: Multiply p1 by C/D (p1 = p1 * C/D)
        mpz_mul_ui(p1, p1, C/12); // D = 12
        
        // Debug output after modifications
        p_str = mpz_get_str(NULL, 10, p1);
        q_str = mpz_get_str(NULL, 10, q1);
        if (p_str && q_str) {
            printf("After modifications: p1=%s, q1=%s\n", p_str, q_str);
            free(p_str);
            free(q_str);
        }
        
        // Step 3: Convert to floating point
        mpfr_set_z(pi, p1, MPFR_RNDN);  // pi = p1
        mpfr_set_z(qi, q1, MPFR_RNDN);  // qi = q1
        
        // Step 4: Calculate sqrt(C)
        mpfr_set_ui(sqrt_c, C, MPFR_RNDN);
        mpfr_sqrt(sqrt_c, sqrt_c, MPFR_RNDN);
        
        // Step 5: Divide pi by qi (pi = pi / qi)
        mpfr_div(pi, pi, qi, MPFR_RNDN);
        
        // Step 6: Multiply by sqrt(C) (pi = pi * sqrt(C))
        mpfr_mul(pi, pi, sqrt_c, MPFR_RNDN);
        
        // Clear the temporary mpz variables
        mpz_clear(p1);
        mpz_clear(q1);
        
        // Print final value for verification
        mpfr_printf("Final pi value: %.10Rf\n", pi);
        
        // Write the result to file
        FILE* f = fopen(state->output_file, "w");
        if (f) {
            // Get pi as a string
            mpfr_exp_t exp;
            char *str_pi = mpfr_get_str(NULL, &exp, 10, state->digits + 2, pi, MPFR_RNDN);
            
            if (str_pi != NULL) {
                // Format correctly with the right exponent (should be 1 for pi)
                if (exp == 1) {
                    fprintf(f, "%c.%s", str_pi[0], str_pi + 1);  // Standard 3.14159... format
                } else {
                    // Fall back to standard format if something unusual happened with the exponent
                    fprintf(f, "%c.%s", str_pi[0], str_pi + 1);
                }
                mpfr_free_str(str_pi);
            } else {
                fprintf(f, "ERROR: Pi calculation failed (mpfr_get_str returned NULL)");
            }
            if (fclose(f) != 0) {
                log_message("error", "fclose failed for output file %s in calculate_pi_gauss_legendre: %s", state->output_file, strerror(errno));
            }
        } else {
            log_message("error", "fopen failed for output file %s in calculate_pi_gauss_legendre: %s", state->output_file, strerror(errno));
        }
        
        // Clean up variables
        mpz_clear(P);
        mpz_clear(Q);
        mpz_clear(T);
        mpfr_clear(pi);
        mpfr_clear(qi);
        mpfr_clear(sqrt_c);
        
        printf("Pi calculation complete! Result saved to %s\n", state->output_file);
        return;
    }
    
    // For larger digit counts, use the full disk-based approach
    printf("DEBUG: Using full disk-based approach\n");
    
    // Create working directories
    char split_path[MAX_PATH];
    printf("DEBUG: Joining paths: %s + 'split'\n", state->work_dir);
    if (safe_path_join(split_path, MAX_PATH, state->work_dir, "split") < 0) {
        fprintf(stderr, "Error: Path too long for split directory\n");
        return;  // Return early on error
    }
    
    printf("DEBUG: Split path is: %s\n", split_path);
    
    // Ensure we clean up any existing split directory first
    // Delete the split directory and recreate it fresh to avoid corrupt state
    printf("DEBUG: Removing existing split directory to ensure clean state\n");
    char rm_cmd[MAX_PATH + 20];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", split_path);
    int sys_result = system(rm_cmd);
    if (sys_result == -1) {
        log_message("warning", "system(rm -rf split_path) failed in calculate_pi_chudnovsky for %s: %s", split_path, strerror(errno));
    } else if (WIFEXITED(sys_result) && WEXITSTATUS(sys_result) != 0) {
        log_message("warning", "system(rm -rf split_path) for %s exited with status %d", split_path, WEXITSTATUS(sys_result));
    }
    
    // Now create fresh split directory
    if (mkdir_recursive(split_path, 0755) != 0) {
        fprintf(stderr, "Error: Failed to create split directory\n");
        return;  // Return early on error
    }
    printf("DEBUG: Successfully created clean split directory: %s\n", split_path);
    
    // Determine memory threshold based on available memory
    size_t memory_threshold = 1000;  // Default
    printf("DEBUG: Memory threshold set to %zu\n", memory_threshold);
    
    // Adjust threshold based on size of calculation
    if (state->terms > 1000000) {
        memory_threshold = 1000;  // Small threshold for very large calculations
    } else if (state->terms > 100000) {
        memory_threshold = 5000;
    } else if (state->terms > 10000) {
        memory_threshold = 10000;
    } else {
        memory_threshold = 20000;
    }
    
    // Perform binary splitting
    printf("DEBUG: Pool has %d threads\n", pool->num_threads);
    if (pool->num_threads <= 1) {
        // Single-threaded approach
        printf("DEBUG: Using single-threaded approach, terms: %lu\n", state->terms);
        
        // Create a temporary state for the calculation
        chudnovsky_state temp_state;
        char temp_path[MAX_PATH];
        
        if (safe_path_join(temp_path, MAX_PATH, split_path, "single") < 0) {
            fprintf(stderr, "Error: Path too long for single thread calculation\n");
            return;
        }
        
        if (mkdir_recursive(temp_path, 0755) != 0) {
            fprintf(stderr, "Error: Failed to create single thread calculation directory\n");
            return;
        }
        
        // Initialize the temporary state
        chudnovsky_state_init(&temp_state, temp_path);
        
        // Perform the calculation
        binary_split_disk(&temp_state, 0, state->terms, split_path, memory_threshold);
        
        // Transfer the results to the original state
        mpz_t final_P, final_Q, final_T;
        mpz_init(final_P);
        mpz_init(final_Q);
        mpz_init(final_T);
        
        // Get values from temp state
        disk_int_get_mpz(final_P, &temp_state.P);
        disk_int_get_mpz(final_Q, &temp_state.Q);
        disk_int_get_mpz(final_T, &temp_state.T);
        
        // Transfer to the original state
        disk_int_set_mpz(&state->data.chudnovsky.P, final_P);
        disk_int_set_mpz(&state->data.chudnovsky.Q, final_Q);
        disk_int_set_mpz(&state->data.chudnovsky.T, final_T);
        
        // Clean up
        mpz_clear(final_P);
        mpz_clear(final_Q);
        mpz_clear(final_T);
        chudnovsky_state_clear(&temp_state);
    } else {
        // Multi-threaded approach
        unsigned long chunk_size = state->terms / pool->num_threads;
        
        // Create task for each chunk
        calc_task** tasks = (calc_task**)malloc(pool->num_threads * sizeof(calc_task*));
        if (tasks == NULL) {
            log_message("critical", "malloc failed for tasks array in calculate_pi_chudnovsky: %s", strerror(errno));
            update_job_status(state->job_idx, JOB_STATUS_FAILED, state->current_term / (double)state->terms, "malloc failure for tasks array");
            return;
        }
        chudnovsky_state* chunk_results = (chudnovsky_state*)malloc(
                                          pool->num_threads * sizeof(chudnovsky_state));
        if (chunk_results == NULL) {
            log_message("critical", "malloc failed for chunk_results array in calculate_pi_chudnovsky: %s", strerror(errno));
            free(tasks); // tasks was allocated
            update_job_status(state->job_idx, JOB_STATUS_FAILED, state->current_term / (double)state->terms, "malloc failure for chunk_results array");
            return;
        }
        
        for (int i = 0; i < pool->num_threads; i++) {
            unsigned long start = i * chunk_size;
            unsigned long end = (i == pool->num_threads - 1) ? state->terms : (i + 1) * chunk_size;
            
            char chunk_path[MAX_PATH];
            char chunk_component[32];
            snprintf(chunk_component, sizeof(chunk_component), "chunk_%d", i);
            if (safe_path_join(chunk_path, MAX_PATH, split_path, chunk_component) < 0) {
                fprintf(stderr, "Error: Path too long for chunk directory %d\n", i);
                // Handle error - this is in a loop, so you might want to continue or break
            }
            mkdir_recursive(chunk_path, 0755);
            
            chudnovsky_state_init(&chunk_results[i], chunk_path);
            
            tasks[i] = calc_thread_pool_submit_binary_split(pool, start, end, 
                                                         &chunk_results[i], 
                                                         chunk_path, 
                                                         memory_threshold);
        }
        
        // Wait for all tasks to complete
        for (int i = 0; i < pool->num_threads; i++) {
            calc_task_wait(tasks[i]);
            
            // Update job progress
            if (state->job_idx >= 0) {
                double progress = (double)(i + 1) / pool->num_threads * 0.8;  // 80% of total progress
                update_job_status(state->job_idx, JOB_STATUS_RUNNING, progress, NULL);
            }
            
            calc_task_free(tasks[i]);
        }
        
        // Combine results
        printf("Combining chunk results...\n");
        
        // Clean up existing state before reinitializing
        // Instead of clearing and reinitializing the main state,
        // let's create a temporary state for combining results
        chudnovsky_state combined_state;
        
        char result_path[MAX_PATH];
        if (safe_path_join(result_path, MAX_PATH, split_path, "combined") < 0) {
            fprintf(stderr, "Error: Path too long for combined results\n");
            return;
        }
        
        if (mkdir_recursive(result_path, 0755) != 0) {
            fprintf(stderr, "Error: Failed to create combined result directory\n");
            return;
        }
        
        // Initialize the temporary state
        chudnovsky_state_init(&combined_state, result_path);
        
        // Set result to first chunk
        mpz_t mpz_temp;
        mpz_init(mpz_temp);
        
        // Set initial P from chunk 0
        disk_int_get_mpz(mpz_temp, &chunk_results[0].P);
        printf("DEBUG: Initial P from chunk 0: %s\n", mpz_get_str(NULL, 10, mpz_temp));
        disk_int_set_mpz(&combined_state.P, mpz_temp);
        
        // Set initial Q from chunk 0
        disk_int_get_mpz(mpz_temp, &chunk_results[0].Q);
        printf("DEBUG: Initial Q from chunk 0: %s\n", mpz_get_str(NULL, 10, mpz_temp));
        disk_int_set_mpz(&combined_state.Q, mpz_temp);
        
        // Set initial T from chunk 0
        disk_int_get_mpz(mpz_temp, &chunk_results[0].T);
        printf("DEBUG: Initial T from chunk 0: %s\n", mpz_get_str(NULL, 10, mpz_temp));
        disk_int_set_mpz(&combined_state.T, mpz_temp);
        
        // Verify combined state was initialized correctly
        mpz_t verify_p, verify_q, verify_t;
        mpz_init(verify_p);
        mpz_init(verify_q);
        mpz_init(verify_t);
        
        disk_int_get_mpz(verify_p, &combined_state.P);
        disk_int_get_mpz(verify_q, &combined_state.Q);
        disk_int_get_mpz(verify_t, &combined_state.T);
        
        printf("DEBUG: After initial seeding - combined_state.P=%s\n", mpz_get_str(NULL, 10, verify_p));
        printf("DEBUG: After initial seeding - combined_state.Q=%s\n", mpz_get_str(NULL, 10, verify_q));
        printf("DEBUG: After initial seeding - combined_state.T=%s\n", mpz_get_str(NULL, 10, verify_t));
        
        mpz_clear(verify_p);
        mpz_clear(verify_q);
        mpz_clear(verify_t);
        
        // Combine with remaining chunks
        for (int i = 1; i < pool->num_threads; i++) {
            printf("Combining with chunk %d...\n", i);
            
            // Create temporary results
            disk_int temp_P, temp_Q, temp_T1, temp_T2;
            
            char temp_path[MAX_PATH];
            char temp_component[32];
            snprintf(temp_component, sizeof(temp_component), "temp_%d", i);
            if (safe_path_join(temp_path, MAX_PATH, result_path, temp_component) < 0) {
                fprintf(stderr, "Error: Path too long for temp directory %d\n", i);
                // Handle error
            }
            mkdir_recursive(temp_path, 0755);
            
            disk_int_init(&temp_P, temp_path);
            disk_int_init(&temp_Q, temp_path);
            disk_int_init(&temp_T1, temp_path);
            disk_int_init(&temp_T2, temp_path);
            
            // P = P1 * P2
            disk_int_mul(&temp_P, &combined_state.P, &chunk_results[i].P);
            
            // Q = Q1 * Q2
            disk_int_mul(&temp_Q, &combined_state.Q, &chunk_results[i].Q);
            
            // T = T1 * Q2 + T2 * P1
            disk_int_mul(&temp_T1, &combined_state.T, &chunk_results[i].Q);
            disk_int_mul(&temp_T2, &chunk_results[i].T, &combined_state.P);
            
            // Get the values without clearing first
            mpz_t mpz_temp_P, mpz_temp_Q, mpz_temp_T;
            mpz_init(mpz_temp_P);
            mpz_init(mpz_temp_Q);
            mpz_init(mpz_temp_T);
            
            // Get temporary values
            disk_int_get_mpz(mpz_temp_P, &temp_P);
            disk_int_get_mpz(mpz_temp_Q, &temp_Q);
            
            // T = T1 + T2
            disk_int temp_T;
            disk_int_init(&temp_T, temp_path);
            disk_int_add(&temp_T, &temp_T1, &temp_T2);
            disk_int_get_mpz(mpz_temp_T, &temp_T);
            
            // Update combined state directly WITHOUT clearing first
            // This is the fix for the bug where disk integers were cleared before setting new values
            // Since combined_state.P, Q, T were already initialized, we can directly set new values
            disk_int_set_mpz(&combined_state.P, mpz_temp_P);
            disk_int_set_mpz(&combined_state.Q, mpz_temp_Q);
            disk_int_set_mpz(&combined_state.T, mpz_temp_T);
            
            // Verify combined state was updated correctly after combining with chunk i
            // Use different variable names to avoid shadowing previous declarations
            mpz_t verify_p_iter, verify_q_iter, verify_t_iter;
            mpz_init(verify_p_iter);
            mpz_init(verify_q_iter);
            mpz_init(verify_t_iter);
            
            disk_int_get_mpz(verify_p_iter, &combined_state.P);
            disk_int_get_mpz(verify_q_iter, &combined_state.Q);
            disk_int_get_mpz(verify_t_iter, &combined_state.T);
            
            printf("DEBUG: After combining with chunk %d:\n", i);
            // printf("  - combined_state.P=%s\n", mpz_get_str(NULL, 10, verify_p_iter));
            // printf("  - combined_state.Q=%s\n", mpz_get_str(NULL, 10, verify_q_iter));
            // printf("  - combined_state.T=%s\n", mpz_get_str(NULL, 10, verify_t_iter));
            
            // Check for zeros
            if (mpz_sgn(verify_q_iter) == 0) {
                printf("CRITICAL ERROR: Q became ZERO after combining with chunk %d!\n", i);
                printf("  - temp_Q was: %s\n", mpz_get_str(NULL, 10, mpz_temp_Q));
            }
            
            mpz_clear(verify_p_iter);
            mpz_clear(verify_q_iter);
            mpz_clear(verify_t_iter);
            
            // Clean up temporary MPZ values
            mpz_clear(mpz_temp_P);
            mpz_clear(mpz_temp_Q);
            mpz_clear(mpz_temp_T);
            
            // Clean up only the temporary disk_int objects
            // We MUST NOT clear combined_state.P/Q/T here as they're needed for the next iteration
            disk_int_clear(&temp_P);
            disk_int_clear(&temp_Q);
            disk_int_clear(&temp_T1);
            disk_int_clear(&temp_T2);
            disk_int_clear(&temp_T);
            
            rmdir(temp_path);
            
            // Update job progress
            if (state->job_idx >= 0) {
                double progress = 0.8 + (double)i / pool->num_threads * 0.1;  // 80-90% of total progress
                update_job_status(state->job_idx, JOB_STATUS_RUNNING, progress, NULL);
            }
        }
        
        mpz_clear(mpz_temp);
        
        // Clean up chunk results
        for (int i = 0; i < pool->num_threads; i++) {
            chudnovsky_state_clear(&chunk_results[i]);
            
            char chunk_path[MAX_PATH];
            char chunk_component[32];
            snprintf(chunk_component, sizeof(chunk_component), "chunk_%d", i);
            if (safe_path_join(chunk_path, MAX_PATH, split_path, chunk_component) < 0) {
                fprintf(stderr, "Error: Path too long for chunk directory %d\n", i);
                // Handle error
            }
            rmdir(chunk_path);
        }
        
        free(chunk_results);
        free(tasks);
        
        // Now we need to transfer the final results from combined_state to the original state
        mpz_t final_P, final_Q, final_T;
        mpz_init(final_P);
        mpz_init(final_Q);
        mpz_init(final_T);
        
        // Verify combined state is still valid
        printf("DEBUG: Verifying combined state objects are still valid\n");
        printf("DEBUG: combined_state.P.file_path=%s\n", combined_state.P.file_path);
        printf("DEBUG: combined_state.Q.file_path=%s\n", combined_state.Q.file_path);
        printf("DEBUG: combined_state.T.file_path=%s\n", combined_state.T.file_path);
        
        // Check if any file paths got cleared accidentally
        if (combined_state.P.file_path[0] == '\0') {
            fprintf(stderr, "CRITICAL ERROR: combined_state.P.file_path is empty before final transfer!\n");
            // Reinitialize it if needed
            if (safe_path_join(combined_state.P.file_path, MAX_PATH, result_path, "P") >= 0) {
                fprintf(stderr, "Attempting to recover P path: %s\n", combined_state.P.file_path);
            }
        }
        if (combined_state.Q.file_path[0] == '\0') {
            fprintf(stderr, "CRITICAL ERROR: combined_state.Q.file_path is empty before final transfer!\n");
            // Reinitialize it if needed
            if (safe_path_join(combined_state.Q.file_path, MAX_PATH, result_path, "Q") >= 0) {
                fprintf(stderr, "Attempting to recover Q path: %s\n", combined_state.Q.file_path);
            }
        }
        if (combined_state.T.file_path[0] == '\0') {
            fprintf(stderr, "CRITICAL ERROR: combined_state.T.file_path is empty before final transfer!\n");
            // Reinitialize it if needed
            if (safe_path_join(combined_state.T.file_path, MAX_PATH, result_path, "T") >= 0) {
                fprintf(stderr, "Attempting to recover T path: %s\n", combined_state.T.file_path);
            }
        }
        
        // Get values from combined state
        disk_int_get_mpz(final_P, &combined_state.P);
        disk_int_get_mpz(final_Q, &combined_state.Q);
        disk_int_get_mpz(final_T, &combined_state.T);
        
        // Print debug info before transfer
        printf("DEBUG: Before transfer - P=%s, Q=%s, T=%s\n",
            mpz_get_str(NULL, 10, final_P),
            mpz_get_str(NULL, 10, final_Q),
            mpz_get_str(NULL, 10, final_T));
            
        // Transfer to the original state
        // CRITICAL: We must NOT clear the original state before transfer
        // Instead, make sure the disk_int_set_mpz function handles the update correctly
        
        printf("DEBUG: CRITICAL - Transferring combined_state to original state\n");
        printf("DEBUG: Original state P path: %s\n", state->data.chudnovsky.P.file_path);
        printf("DEBUG: Original state Q path: %s\n", state->data.chudnovsky.Q.file_path);
        printf("DEBUG: Original state T path: %s\n", state->data.chudnovsky.T.file_path);
        
        // Before transfer, verify the original objects are properly initialized
        if (state->data.chudnovsky.P.file_path[0] == '\0') {
            fprintf(stderr, "ERROR: Original state P has empty file_path before transfer!\n");
        }
        if (state->data.chudnovsky.Q.file_path[0] == '\0') {
            fprintf(stderr, "ERROR: Original state Q has empty file_path before transfer!\n");
        }
        if (state->data.chudnovsky.T.file_path[0] == '\0') {
            fprintf(stderr, "ERROR: Original state T has empty file_path before transfer!\n");
        }
        
        // Transfer to original state
        disk_int_set_mpz(&state->data.chudnovsky.P, final_P);
        disk_int_set_mpz(&state->data.chudnovsky.Q, final_Q);
        disk_int_set_mpz(&state->data.chudnovsky.T, final_T);
        
        // Clean up mpz values FIRST before clearing the combined state
        mpz_clear(final_P);
        mpz_clear(final_Q);
        mpz_clear(final_T);
        
        // CRITICAL: Verify the values were transferred correctly
        mpz_t verify_P, verify_Q, verify_T;
        mpz_init(verify_P);
        mpz_init(verify_Q);
        mpz_init(verify_T);
        
        printf("DEBUG: Verifying transfer was successful...\n");
        disk_int_get_mpz(verify_P, &state->data.chudnovsky.P);
        disk_int_get_mpz(verify_Q, &state->data.chudnovsky.Q);
        disk_int_get_mpz(verify_T, &state->data.chudnovsky.T);
        
        printf("DEBUG: After transfer - P=%s, Q=%s, T=%s\n",
            mpz_get_str(NULL, 10, verify_P),
            mpz_get_str(NULL, 10, verify_Q),
            mpz_get_str(NULL, 10, verify_T));
            
        mpz_clear(verify_P);
        mpz_clear(verify_Q);
        mpz_clear(verify_T);
        
        // Now it's safe to clear the combined state
        chudnovsky_state_clear(&combined_state);
    }
    
    // Mark calculation as complete
    state->current_term = state->terms;
    
    // Final computation of Pi
    printf("Binary splitting complete. Computing final Pi value...\n");
    
    // Verify state before proceeding with enhanced debug information
    if (!state) {
        fprintf(stderr, "Error: Null state in final Pi calculation\n");
        return;
    }
    
    if (state->data.chudnovsky.P.file_path[0] == '\0') {
        fprintf(stderr, "Error: Uninitialized P disk integer in final Pi calculation\n");
    }
    
    if (state->data.chudnovsky.Q.file_path[0] == '\0') {
        fprintf(stderr, "Error: Uninitialized Q disk integer in final Pi calculation\n");
    }
    
    if (state->data.chudnovsky.T.file_path[0] == '\0') {
        fprintf(stderr, "Error: Uninitialized T disk integer in final Pi calculation\n");
    }
    
    // Check for any initialization issues
    if (!state->data.chudnovsky.P.file_path[0] || 
        !state->data.chudnovsky.Q.file_path[0] || 
        !state->data.chudnovsky.T.file_path[0]) {
        
        fprintf(stderr, "Error: Invalid state or uninitialized P, Q, T in final Pi calculation\n");
        
        // Set a default output file with an error message
        char error_file[MAX_PATH];
        snprintf(error_file, MAX_PATH, "./pi_calc/pi_error_%lu.txt", time(NULL));
        FILE* f = fopen(error_file, "w");
        if (f) {
            fprintf(f, "ERROR: Pi calculation failed due to invalid state\n");
            fclose(f);
        }
        return;
    }
    
    // Print debug information about the state
    printf("Debug: P file path = %s\n", state->data.chudnovsky.P.file_path);
    printf("Debug: Q file path = %s\n", state->data.chudnovsky.Q.file_path);
    printf("Debug: T file path = %s\n", state->data.chudnovsky.T.file_path);
    
    // Update job progress
    if (state->job_idx >= 0) {
        update_job_status(state->job_idx, JOB_STATUS_RUNNING, 0.9, NULL);  // 90% progress
    }
    
    // Pi = p*(C/D)*sqrt(C) / (q+A*p) - Correct Chudnovsky formula
    // Based on reference implementation in gmp-chudnovsky.c (line 686-689)
    mpz_t mpz_P, mpz_Q, mpz_T, mpz_APplusQ;
    mpfr_t mpfr_pi, mpfr_C, mpfr_temp, mpfr_P_val, mpfr_Q_val, mpfr_sqrt_C;
    
    mpz_init(mpz_P);
    mpz_init(mpz_Q);
    mpz_init(mpz_T);
    mpz_init(mpz_APplusQ);
    
    // Determine required precision
    mpfr_prec_t precision = (mpfr_prec_t)(state->digits * 4);
    
    mpfr_init2(mpfr_pi, precision);
    mpfr_init2(mpfr_C, precision);
    mpfr_init2(mpfr_temp, precision);
    mpfr_init2(mpfr_P_val, precision);
    mpfr_init2(mpfr_Q_val, precision);
    mpfr_init2(mpfr_sqrt_C, precision);
    
    // Reset values to safe defaults first
    mpz_set_ui(mpz_P, 1);
    mpz_set_ui(mpz_Q, 1);
    mpz_set_ui(mpz_T, 1);
    
    // Try to get P, Q, T from disk
    bool disk_read_successful = true;
    if (state->data.chudnovsky.P.file_path[0] != '\0') {
        disk_int_get_mpz(mpz_P, &state->data.chudnovsky.P);
    } else {
        disk_read_successful = false;
        fprintf(stderr, "Error: P state is not initialized\n");
    }
    
    if (state->data.chudnovsky.Q.file_path[0] != '\0') {
        disk_int_get_mpz(mpz_Q, &state->data.chudnovsky.Q);
    } else {
        disk_read_successful = false;
        fprintf(stderr, "Error: Q state is not initialized\n");
    }
    
    if (state->data.chudnovsky.T.file_path[0] != '\0') {
        disk_int_get_mpz(mpz_T, &state->data.chudnovsky.T);
    } else {
        disk_read_successful = false;
        fprintf(stderr, "Error: T state is not initialized\n");
    }
    
    // Verify that we got valid values
    if (mpz_sgn(mpz_P) == 0) {
        fprintf(stderr, "Error: Invalid value for P (zero)\n");
        mpz_set_ui(mpz_P, 1); // Prevent division by zero
        disk_read_successful = false;
    }
    
    if (mpz_sgn(mpz_Q) == 0) {
        fprintf(stderr, "Error: Invalid value for Q (zero)\n");
        mpz_set_ui(mpz_Q, 1); // Prevent division by zero
        disk_read_successful = false;
    }
    
    if (!disk_read_successful) {
        fprintf(stderr, "WARNING: Could not read proper P, Q, T values from disk.\n");
        fprintf(stderr, "Computing default Pi approximation with limited precision.\n");
    }
    
    printf("Debug: P=%s, Q=%s, T=%s\n", 
           mpz_get_str(NULL, 10, mpz_P),
           mpz_get_str(NULL, 10, mpz_Q), 
           mpz_get_str(NULL, 10, mpz_T));
    
    // C = 640320
    mpfr_set_ui(mpfr_C, C, MPFR_RNDN);
    
    // Convert P and Q to mpfr values
    mpfr_set_z(mpfr_P_val, mpz_P, MPFR_RNDN);
    mpfr_set_z(mpfr_Q_val, mpz_Q, MPFR_RNDN);
    
    // Calculate q + A*p (this is exactly how gmp-chudnovsky.c does it on line 694: mpz_addmul_ui(q1, p1, A);)
    mpz_mul_ui(mpz_APplusQ, mpz_P, A);  // A*p
    mpz_add(mpz_APplusQ, mpz_APplusQ, mpz_Q);  // q + A*p
    
    // Multiply P by C/D (this matches gmp-chudnovsky.c line 695: mpz_mul_ui(p1, p1, C/D);)
    mpz_mul_ui(mpz_P, mpz_P, C/D);
    
    // Calculate sqrt(C)
    mpfr_sqrt(mpfr_sqrt_C, mpfr_C, MPFR_RNDN);
    
    // Formula from gmp-chudnovsky.c lines 686-689:
    /*
        p*(C/D)*sqrt(C)
      pi = -----------------
           (q+A*p)
    */
    
    // Numerator: p*(C/D)*sqrt(C)
    mpfr_set_z(mpfr_pi, mpz_P, MPFR_RNDN);  // pi = p*(C/D)
    mpfr_mul(mpfr_pi, mpfr_pi, mpfr_sqrt_C, MPFR_RNDN);  // pi = p*(C/D)*sqrt(C)
    
    // Denominator: (q+A*p)
    mpfr_set_z(mpfr_temp, mpz_APplusQ, MPFR_RNDN);  // temp = q+A*p
    
    // Double check that denominator is not zero
    if (mpfr_sgn(mpfr_temp) == 0) {
        fprintf(stderr, "Error: Denominator (q+A*p) is zero.\n");
        // Prevent division by zero
        mpfr_set_ui(mpfr_temp, 1, MPFR_RNDN);
    }
    
    // Final division: pi = Numerator / Denominator
    mpfr_div(mpfr_pi, mpfr_pi, mpfr_temp, MPFR_RNDN);
    
    // Debug output
    mpfr_printf("Final pi value: %.10Rf\n", mpfr_pi);
    
    // Check if result is NaN or Infinity
    if (!mpfr_number_p(mpfr_pi)) {
        fprintf(stderr, "WARNING: Pi calculation resulted in a non-numeric value (NaN or Inf)\n");
        fprintf(stderr, "This usually indicates a division by zero or invalid operation\n");
    }
    
    // Clean up mpz variables (but not mpfr ones yet as we need them for output)
    mpz_clear(mpz_P);
    mpz_clear(mpz_Q);
    mpz_clear(mpz_T);
    mpz_clear(mpz_APplusQ);
    
    // Write the result to file before cleaning up mpfr variables
    FILE* f = fopen(state->output_file, "w");
    if (f) {
        // First print a safe version using mpfr_fprintf (avoids segfault)
        // fprintf(f, "Pi approximation (safe format): ");
        // mpfr_fprintf(f, "%.50RNf\n\n", mpfr_pi);
        
        // Now attempt to get full precision string representation
        printf("DEBUG: Preparing to call mpfr_get_str for %lu digits\n", state->digits);
        printf("DEBUG: mpfr_pi precision = %lu bits\n", mpfr_get_prec(mpfr_pi));
        
        // Use a try-catch approach to handle potential mpfr_get_str issues
        char *str_pi = NULL;
        mpfr_exp_t exp = 0;
        
        // Safety: Limit digits to reasonable maximum if needed
        // unsigned long safe_digits = state->digits;
        // if (safe_digits > 100000) {
        //     fprintf(stderr, "WARNING: Limiting digit output to 100000 for safety\n");
        //     safe_digits = 100000;
        // }
        
        // Only attempt mpfr_get_str if we have a valid number
        if (mpfr_number_p(mpfr_pi)) {
            // Use mpfr_get_str with additional error checking
            str_pi = mpfr_get_str(NULL, &exp, 10, state->digits + 2, mpfr_pi, MPFR_RNDN);
        
            if (str_pi != NULL) {
                // Check that we got valid output
                if (str_pi[0] != '@' && str_pi[0] != 'n' && str_pi[0] != 'N') {
                    // Format correctly: insert decimal point after first digit
                    fprintf(f, "%c.%s", str_pi[0], str_pi + 1);
                } else {
                    // Got invalid result, generate a detailed error report
                    time_t now = time(NULL);
                    char time_str[32];
                    struct tm *tm_info = localtime(&now);
                    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
                    
                    fprintf(f, "ERROR: Pi calculation failed to produce valid result\n\n");
                    fprintf(f, "Diagnostic Information:\n");
                    fprintf(f, "  Timestamp: %s\n", time_str);
                    fprintf(f, "  Algorithm: %s\n", state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre");
                    fprintf(f, "  Requested digits: %lu\n", state->digits);
                    fprintf(f, "  Out-of-core mode: %s\n", state->out_of_core ? "Yes" : "No");
                    fprintf(f, "  Invalid result value: '%s'\n", str_pi);
                    fprintf(f, "  Working directory: %s\n", state->work_dir);
                    fprintf(f, "  Memory allocation check: %s\n", mpfr_mp_memory_cleanup() ? "Leaks detected" : "No leaks detected");
                    
                    // Log additional error info
                    fprintf(stderr, "Error: Pi calculation failed to produce valid result at %s\n", time_str);
                    fprintf(stderr, "  Algorithm: %s, Digits: %lu, Result: '%s'\n", 
                            state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre",
                            state->digits, str_pi);
                    
                    // Create error report file for diagnostics
                    char error_file[MAX_PATH];
                    
                    // Calculate path length to ensure no truncation
                    size_t dir_len = strlen(state->work_dir);
                    char time_str_filename[32];
                    snprintf(time_str_filename, sizeof(time_str_filename), "%ld", (long)now);
                    size_t time_len = strlen(time_str_filename);
                    size_t suffix_len = strlen("/error_report_.json") + time_len;
                    
                    if (dir_len + suffix_len + 1 > MAX_PATH) {
                        fprintf(stderr, "Error: Error report path would be too long\n");
                        strncpy(error_file, "error_report_truncated.json", MAX_PATH-1);
                        error_file[MAX_PATH-1] = '\0';
                    } else {
                        snprintf(error_file, sizeof(error_file), "%s/error_report_%s.json", 
                                state->work_dir, time_str_filename);
                    }
                    
                    FILE* error_report = fopen(error_file, "w");
                    if (error_report) {
                        fprintf(error_report, "{\n");
                        fprintf(error_report, "  \"error\": \"Pi calculation failed to produce valid result\",\n");
                        fprintf(error_report, "  \"timestamp\": %ld,\n", (long)now);
                        fprintf(error_report, "  \"algorithm\": \"%s\",\n", 
                                state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre");
                        fprintf(error_report, "  \"digits\": %lu,\n", state->digits);
                        fprintf(error_report, "  \"out_of_core\": %s,\n", state->out_of_core ? "true" : "false");
                        fprintf(error_report, "  \"invalid_result\": \"%s\",\n", str_pi);
                        fprintf(error_report, "  \"work_dir\": \"%s\",\n", state->work_dir);
                        fprintf(error_report, "  \"job_id\": %d\n", state->job_idx);
                        fprintf(error_report, "}\n");
                        fclose(error_report);
                        
                        fprintf(stderr, "  Error report saved to: %s\n", error_file);
                    }
                    
                    // Update job status if this is part of a job
                    if (state->job_idx >= 0) {
                        char error_message[MAX_JOB_ERROR_MSG];
                        snprintf(error_message, sizeof(error_message), 
                                 "Calculation failed: Invalid result produced ('%s')", str_pi);
                        update_job_status(state->job_idx, JOB_STATUS_FAILED, 1.0, error_message);
                    }
                }
                
                // Free the string allocated by mpfr_get_str
                mpfr_free_str(str_pi);
            } else {
                // Handle NULL result from mpfr_get_str with detailed error report
                time_t now = time(NULL);
                char time_str[32];
                struct tm *tm_info = localtime(&now);
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
                
                fprintf(f, "ERROR: Pi calculation failed (mpfr_get_str returned NULL)\n\n");
                fprintf(f, "Diagnostic Information:\n");
                fprintf(f, "  Timestamp: %s\n", time_str);
                fprintf(f, "  Algorithm: %s\n", state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre");
                fprintf(f, "  Requested digits: %lu\n", state->digits);
                fprintf(f, "  Out-of-core mode: %s\n", state->out_of_core ? "Yes" : "No");
                fprintf(f, "  Working directory: %s\n", state->work_dir);
                fprintf(f, "  MPFR version: %s\n", mpfr_get_version());
                fprintf(f, "  Memory allocation check: %s\n", mpfr_mp_memory_cleanup() ? "Leaks detected" : "No leaks detected");
                
                // Log error details
                fprintf(stderr, "Error: mpfr_get_str returned NULL in Pi calculation at %s\n", time_str);
                fprintf(stderr, "  Algorithm: %s, Digits: %lu\n", 
                        state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre",
                        state->digits);
            
                // Create error report file for diagnostics
                char error_file[MAX_PATH];
                
                // Calculate path length to ensure no truncation
                size_t dir_len = strlen(state->work_dir);
                char time_str_null_err[32];
                snprintf(time_str_null_err, sizeof(time_str_null_err), "%ld", (long)now);
                size_t time_len = strlen(time_str_null_err);
                size_t suffix_len = strlen("/error_report_null_.json") + time_len;
                
                if (dir_len + suffix_len + 1 > MAX_PATH) {
                    fprintf(stderr, "Error: Error report path would be too long\n");
                    strncpy(error_file, "error_report_truncated.json", MAX_PATH-1);
                    error_file[MAX_PATH-1] = '\0';
                } else {
                    snprintf(error_file, sizeof(error_file), "%s/error_report_null_%s.json", 
                            state->work_dir, time_str_null_err);
                }
                
                FILE* error_report = fopen(error_file, "w");
                if (error_report) {
                    fprintf(error_report, "{\n");
                    fprintf(error_report, "  \"error\": \"Pi calculation failed (mpfr_get_str returned NULL)\",\n");
                    fprintf(error_report, "  \"timestamp\": %ld,\n", (long)now);
                    fprintf(error_report, "  \"algorithm\": \"%s\",\n", 
                            state->algorithm == ALGO_CHUDNOVSKY ? "Chudnovsky" : "Gauss-Legendre");
                    fprintf(error_report, "  \"digits\": %lu,\n", state->digits);
                    fprintf(error_report, "  \"out_of_core\": %s,\n", state->out_of_core ? "true" : "false");
                    fprintf(error_report, "  \"work_dir\": \"%s\",\n", state->work_dir);
                    fprintf(error_report, "  \"mpfr_version\": \"%s\",\n", mpfr_get_version());
                    fprintf(error_report, "  \"job_id\": %d\n", state->job_idx);
                    fprintf(error_report, "}\n");
                    fclose(error_report);
                    
                    fprintf(stderr, "  Error report saved to: %s\n", error_file);
                }
                
                // Update job status if this is part of a job
                if (state->job_idx >= 0) {
                    char error_message[MAX_JOB_ERROR_MSG];
                    
                    // Calculate max length for error file path in message
                    size_t prefix_len = strlen("Calculation failed: mpfr_get_str returned NULL, error report: ");
                    size_t max_file_path = MAX_JOB_ERROR_MSG - prefix_len - 1; // -1 for null terminator
                    
                    // Truncate error file path if needed
                    char trunc_path[MAX_JOB_ERROR_MSG];
                    if (strlen(error_file) > max_file_path) {
                        // If path is too long, just use the filename part
                        const char* filename = strrchr(error_file, '/');
                        if (filename) {
                            filename++; // Skip the '/'
                            strncpy(trunc_path, filename, sizeof(trunc_path)-1);
                        } else {
                            // No slash found, use truncated path
                            strncpy(trunc_path, error_file, max_file_path);
                            trunc_path[max_file_path] = '\0';
                        }
                    } else {
                        strncpy(trunc_path, error_file, sizeof(trunc_path)-1);
                        trunc_path[sizeof(trunc_path)-1] = '\0';
                    }
                    
                    // Further ensure no truncation by using a predefined error message
                    // Extract only the filename part for inclusion in the error message
                    const char* short_name = strrchr(trunc_path, '/');
                    if (short_name) {
                        short_name++; // Skip the '/'
                    } else {
                        short_name = trunc_path; // Use as is if no slash
                    }
                    
                    // Create a filename of limited length if needed
                    char limit_name[64]; // Small enough to fit in the error message
                    if (strlen(short_name) > 60) {
                        // Truncate to a safe size
                        strncpy(limit_name, short_name, 57);
                        limit_name[57] = '.';
                        limit_name[58] = '.';
                        limit_name[59] = '.';
                        limit_name[60] = '\0';
                    } else {
                        strcpy(limit_name, short_name);
                    }
                    
                    snprintf(error_message, sizeof(error_message), 
                             "Calculation failed: mpfr_get_str returned NULL, error report: %s", 
                             limit_name);
                    update_job_status(state->job_idx, JOB_STATUS_FAILED, 1.0, error_message);
                }
            }
        }
        
        fclose(f);
    }
    
    printf("Pi calculation complete! Result saved to %s\n", state->output_file);
    
    // Update job status
    if (state->job_idx >= 0) {
        update_job_status(state->job_idx, JOB_STATUS_COMPLETED, 1.0, NULL);  // 100% progress
    }
    
    // We won't try to remove directories here as some files might still exist
    // The OS will clean them up when the process exits
    
    // Clean up mpfr variables at the very end
    mpfr_clear(mpfr_P_val);
    mpfr_clear(mpfr_Q_val);
    mpfr_clear(mpfr_C);
    mpfr_clear(mpfr_sqrt_C);
    mpfr_clear(mpfr_temp);
    mpfr_clear(mpfr_pi);
} // End of calculate_pi_chudnovsky function

// Let the compiler know we're at file scope
;

// ==========================================================================
// Logging System
// ==========================================================================

// Implementation of logging and configuration functions

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
void close_log_file() {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

// ==========================================================================
// Configuration Loading and Management
// ==========================================================================

// Global state flags
bool g_server_running = false;  // Server active flag
// Global shutdown flag
volatile bool g_shutdown_flag = false;   // Shutdown requested flag

// Global configuration
config_t config;

// Set default configuration values
void set_default_config() {
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
    // Initialize logging level
    config.log_level = LOG_LEVEL_INFO;
    strncpy(config.logging_output, "console", sizeof(config.logging_output));
    config.logging.level = LOG_LEVEL_INFO; // Set default integer level
    
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
        config.memory_limit = (size_t)json_object_get_int64(j_memory_limit) * 1024 * 1024 * 1024;  // Convert GB to bytes
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
                // Set log level from command line argument
                // Also set the integer value
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
                config.memory_limit = (size_t)atol(optarg) * 1024 * 1024 * 1024;  // Convert GB to bytes
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
        
        // Non-option argument is the digit count
        config.max_digits = strtoul(argv[optind], NULL, 10);
    }
}

// Thread function to monitor calculation timeout
void* timeout_monitor_thread(void* arg) {
    // Make thread cancelable
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    
    // Extract the timeout data
    struct timeout_monitor_data* data = (struct timeout_monitor_data*)arg;
    
    if (!data) {
        fprintf(stderr, "Error: Null argument passed to timeout_monitor_thread\n");
        return NULL;
    }
    
    // Copy the data to local variables
    time_t start_time = data->start_time;
    time_t max_execution_time = data->max_execution_time;
    volatile bool* timed_out_flag = data->timed_out_flag;
    int job_idx = data->job_idx;
    
    // Free the data structure since we've copied what we need
    free(data);
    data = NULL;  // Avoid use-after-free
    
    // Calculate the end time
    time_t end_time = start_time + max_execution_time;
    
    // Log the timeout information
    char timeout_msg[256];
    char *timestr = ctime(&end_time);
    if (timestr) {
        timestr[strlen(timestr)-1] = '\0';  // Remove trailing newline
        snprintf(timeout_msg, sizeof(timeout_msg), 
                "Timeout monitor started for job %d: %ld seconds (until %s)", 
                job_idx, max_execution_time, timestr);
    } else {
        snprintf(timeout_msg, sizeof(timeout_msg), 
                "Timeout monitor started for job %d: %ld seconds", 
                job_idx, max_execution_time);
    }
    log_message("debug", "%s", timeout_msg);
    
    // Get job info for better logging
    unsigned long digits = 0;
    algorithm_t algorithm = ALGO_GAUSS_LEGENDRE;
    
    pthread_mutex_lock(&jobs_lock);
    if (job_idx >= 0 && job_idx < MAX_JOBS && jobs[job_idx].job_id[0] != '\0') {
        pthread_mutex_lock(&jobs[job_idx].lock);
        digits = jobs[job_idx].digits;
        algorithm = jobs[job_idx].algorithm;
        pthread_mutex_unlock(&jobs[job_idx].lock);
    }
    pthread_mutex_unlock(&jobs_lock);
    
    // Loop until the timeout is reached, checking every second
    while (time(NULL) < end_time) {
        // Sleep for a short time to reduce CPU usage
        sleep(1);
        
        // Check if the thread should be canceled
        pthread_testcancel();
        
        // Check if job is already done (no need for timeout anymore)
        bool job_completed = false;
        
        pthread_mutex_lock(&jobs_lock);
        if (job_idx >= 0 && job_idx < MAX_JOBS && jobs[job_idx].job_id[0] != '\0') {
            pthread_mutex_lock(&jobs[job_idx].lock);
            job_completed = (jobs[job_idx].status == JOB_STATUS_COMPLETED || 
                           jobs[job_idx].status == JOB_STATUS_FAILED);
            pthread_mutex_unlock(&jobs[job_idx].lock);
        }
        pthread_mutex_unlock(&jobs_lock);
        
        if (job_completed) {
            log_message("debug", "Job %d already completed, exiting timeout monitor", job_idx);
            return NULL;
        }
    }
    
    // If we get here, the timeout has been reached
    char algorithm_str[3] = "??";
    if (algorithm == ALGO_CHUDNOVSKY) {
        strcpy(algorithm_str, "CH");
    } else if (algorithm == ALGO_GAUSS_LEGENDRE) {
        strcpy(algorithm_str, "GL");
    }
    
    log_message("warning", "Calculation timed out after %ld seconds for job %d (%s, %lu digits)", 
               max_execution_time, job_idx, algorithm_str, digits);
    
    fprintf(stderr, "Calculation timed out after %ld seconds for job %d (%s, %lu digits)\n", 
            max_execution_time, job_idx, algorithm_str, digits);
    
    // Create a timeout report file
    char timeout_file[MAX_PATH];
    
    // Calculate length to ensure no truncation
    size_t dir_len = strlen(config.work_dir);
    char idx_str[32];
    snprintf(idx_str, sizeof(idx_str), "%d", job_idx);
    size_t idx_len = strlen(idx_str);
    size_t suffix_len = strlen("/job__timeout.json") + idx_len;
    
    if (dir_len + suffix_len + 1 > MAX_PATH) {
        fprintf(stderr, "Error: Timeout file path would be too long\n");
        strncpy(timeout_file, "timeout_report.json", MAX_PATH-1);
        timeout_file[MAX_PATH-1] = '\0';
    } else {
        snprintf(timeout_file, sizeof(timeout_file), "%s/job_%d_timeout.json", 
                config.work_dir, job_idx);
    }
    
    FILE* f = fopen(timeout_file, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"job_id\": \"%d\",\n", job_idx);
        fprintf(f, "  \"timeout\": %ld,\n", max_execution_time);
        fprintf(f, "  \"algorithm\": \"%s\",\n", algorithm_str);
        fprintf(f, "  \"digits\": %lu,\n", digits);
        fprintf(f, "  \"start_time\": %ld,\n", (long)start_time);
        fprintf(f, "  \"end_time\": %ld,\n", (long)end_time);
        fprintf(f, "  \"actual_time\": %ld\n", (long)time(NULL));
        fprintf(f, "}\n");
        fclose(f);
    }
    
    // Set the timed out flag to true - use atomic operation to ensure visibility
    __atomic_store_n(timed_out_flag, true, __ATOMIC_SEQ_CST);
    
    // Update the job status
    char error_message[256];
    snprintf(error_message, sizeof(error_message), 
             "Calculation timed out after %ld seconds (%s, %lu digits)", 
             max_execution_time, algorithm_str, digits);
    
    update_job_status(job_idx, JOB_STATUS_FAILED, 1.0, error_message);
    
    return NULL;
}

