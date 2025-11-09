#ifndef SIGNAL_HANDLERS_H
#define SIGNAL_HANDLERS_H

#include "common.h"

// ==========================================================================
// Signal Handlers - Header
// ==========================================================================

// Signal handler functions
void segfault_handler(int signal);
void perform_crash_logging(void);
void handle_sigint(int sig);
void handle_sighup(int sig);

#endif // SIGNAL_HANDLERS_H
