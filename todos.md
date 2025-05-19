# Pi-Server TODOs and Fixed Issues

## Issues Fixed

### Unused Parameters
- Added `(void)sig;` in `segfault_handler` to acknowledge unused signal parameter
- Added `(void)sig;` in `handle_sighup` to acknowledge unused signal parameter
- Added `(void)out_of_core_mode;` in `validate_parameters` with appropriate comment
- Added `(void)argc;` and `(void)argv;` in `run_cli_mode` to document command-line processing

### Unused Variables
- Commented out `calc_thread_pool pool = {0}` with TODO comment
- Commented out `bool pool_initialized = false` with TODO comment
- Commented out `int result = 0` with TODO comment
- Removed reference to result in error handling code

### Sign Conversion Issues
- Fixed pointer subtraction in `last_slash - chunk_path` using `ptrdiff_t` with range check
- Fixed pointer arithmetic in `most_sig_limb - (mp_limb_t*)result->cache` using `ptrdiff_t`
- Fixed thread count handling in `calc_thread_pool_init` and `http_thread_pool_init`
- Added range checks for `digits` when converting to `mpfr_prec_t`
- Fixed `state->current_term = i + 1` by casting after range validation

### Format String Safety
- Added validation for format string in logging functions:
  - Used temporary buffer for initial formatting
  - Added NULL checks for format parameters
  - Used literal format strings in downstream functions
  - Added proper bounds checking

### Shadowed Variables
- Renamed shadowed variables in binary splitting verification:
  - `verify_p`, `verify_q`, `verify_t` → `verify_p_iter`, `verify_q_iter`, `verify_t_iter`
- Updated all related code to use the renamed variables

### Floating-Point to Integer Conversion
- Improved memory calculation with explicit casting and safety:
  - Used `ceil()` for proper rounding behavior
  - Added explicit floating-point operations with `.0` suffix
  - Added 10% safety margin to memory calculations

### Integer Precision Loss
- Changed `int bytes_read = read(client_sock, ...)` to `ssize_t bytes_read` to match system call return type
- Added proper documentation for the type change

## Remaining TODOs

### Memory Allocation Issues
- Add NULL checks after all malloc calls
- Add consistent error handling for allocation failures
- Implement centralized resource cleanup in error paths
- Consider using calloc where zero-initialization is needed

### String Handling
- Replace strncpy with safer alternatives where possible
- Add length checks before all string operations
- Ensure proper null-termination for all string operations
- Remove assumptions about string length

### Thread Synchronization
- Review all mutex usage for potential deadlocks
- Add missing locks around shared data access
- Use atomic operations for flags and counters where appropriate
- Implement proper memory barriers for cross-thread visibility

### System Call Error Handling
- Check return values of all system calls
- Add specific error messages for each failure case
- Implement recovery strategies for common failures
- Log detailed error information for debugging

### Magic Numbers
- Define constants for all numeric values in algorithms
- Document the meaning and source of all constants
- Review timeout and buffer size assumptions
- Replace hardcoded values with configurable parameters

### Error Reporting
- Enhance error messages with more context
- Standardize error logging format
- Add stacktrace capability for crashes
- Implement tiered logging with appropriate severity levels

### Buffer Safety
- Review all buffer allocations for potential overflows
- Add bounds checking to all buffer operations
- Use safer alternatives to manual buffer management where possible
- Consider memory protection mechanisms for sensitive data

### Input Validation
- Add validation for all external inputs
- Sanitize network input before processing
- Validate configuration parameters more thoroughly
- Implement stronger parameter type checking

### Documentation
- Add comprehensive function documentation
- Document thread safety requirements for each function
- Add architectural overview of the codebase
- Document the mathematical basis of the algorithms

### Testing
- Implement unit tests for core functionality
- Add stress tests for memory and thread management
- Create validation tests for calculation accuracy
- Add performance benchmarks for algorithm optimization