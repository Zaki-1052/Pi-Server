#ifndef ALGORITHMS_H
#define ALGORITHMS_H

#include "common.h"
#include "disk_int.h"

// ==========================================================================
// Algorithm Implementations - Header
// ==========================================================================

// Structure to hold Chudnovsky binary splitting state
struct chudnovsky_state {
    disk_int P;  // Numerator term
    disk_int Q;  // Denominator term
    disk_int T;  // Intermediate term
};

// Chudnovsky state management
void chudnovsky_state_init(chudnovsky_state* state, const char* base_path);
void chudnovsky_state_clear(chudnovsky_state* state);

// Binary splitting algorithms
void binary_split_mpz(mpz_t P, mpz_t Q, mpz_t T, unsigned long a, unsigned long b);
void binary_split_disk(chudnovsky_state* result, unsigned long a, unsigned long b, 
                       const char* base_path, size_t memory_threshold);

// Forward declaration of calc_thread_pool (defined in thread_pool.h)
struct calc_thread_pool;

// Pi calculation algorithms
void calculate_pi_gauss_legendre(struct calculation_state* state);
void calculate_pi_chudnovsky(struct calculation_state* state, struct calc_thread_pool* pool);

#endif // ALGORITHMS_H
