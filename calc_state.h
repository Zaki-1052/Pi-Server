#ifndef CALC_STATE_H
#define CALC_STATE_H

#include "common.h"
#include "algorithms.h"

// ==========================================================================
// Calculation State - Header
// ==========================================================================

// Calculation state functions
void calculation_state_init(calculation_state* state, unsigned long digits, algorithm_t algorithm, 
                            bool out_of_core, const char* work_dir, int job_idx);
void calculation_state_clear(calculation_state* state);

// Thread functions for calculations
void* timeout_monitor_thread(void* arg);
void* calculation_thread_func(void* arg);

#endif // CALC_STATE_H
