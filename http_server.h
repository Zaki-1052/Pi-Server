#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "common.h"

// ==========================================================================
// HTTP Server - Header
// ==========================================================================

// Forward declaration for client socket argument
void* handle_http_request(void* arg);

// HTTP request handlers
void parse_query_string(const char* query, char* algo, size_t algo_size, 
                      char* digits_str, size_t digits_size,
                      out_of_core_mode_t* out_of_core_mode,
                      bool* checkpointing_enabled,
                      int* calc_mode);

void handle_calculation_request(int client_sock, algorithm_t algorithm, unsigned long digits,
                              out_of_core_mode_t out_of_core_mode, bool checkpointing_enabled,
                              int calc_mode);

void handle_job_status_request(int client_sock, const char* job_id);
void handle_job_result_request(int client_sock, const char* job_id);

#endif // HTTP_SERVER_H
