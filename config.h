#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

// ==========================================================================
// Configuration Management
// ==========================================================================

// Set default configuration values
void set_default_config(void);

// Load configuration from JSON file
bool load_config_from_file(const char* config_file);

// Override configuration with command line arguments
void override_config_with_args(int argc, char** argv);

#endif // CONFIG_H
