#ifndef SIGNAL_HANDLERS_H
#define SIGNAL_HANDLERS_H

#include "common.h"

// ==========================================================================
// Signal Handlers - Header
// ==========================================================================

// Signal handler functions
void segfault_handler(int signal);
void perform_crash_logging(const char *crash_log_path);
void handle_sigint(int sig);
void handle_sighup(int sig);

// Cleanup functions to be called from main thread
void perform_graceful_shutdown(void);
void handle_sighup_reload(void);

// Function to get sighup flag status
extern volatile sig_atomic_t sighup_received;

#endif // SIGNAL_HANDLERS_H
