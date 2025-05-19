# Pi-Server Known Issues

## Compiler Warnings

The following compiler warnings appear when building on Linux environments with strict warning flags. These should be addressed in future updates:

### Unused Parameters

These function parameters are declared but not used in their respective functions:

- `void segfault_handler(int sig)` - Unused `sig` parameter
- `bool validate_parameters(const char* algo, const char* digits_str, out_of_core_mode_t out_of_core_mode)` - Unused `out_of_core_mode` parameter
- `void handle_sighup(int sig)` - Unused `sig` parameter
- `void run_cli_mode(int argc, char** argv)` - Unused `argc` and `argv` parameters

### Unused/Set-But-Not-Used Variables

These variables are either declared but never used, or set but their values are never read:

- `calc_thread_pool pool = {0}` in the main algorithm function - Unused variable
- `bool pool_initialized = false` - Set but not used
- `int result = 0` - Set but not used

### Sign Conversion Issues

These warnings indicate implicit conversions between signed and unsigned types that could cause unexpected behavior:

- Line 656: `size_t dir_len = last_slash - chunk_path` - `long` to `size_t` conversion
- Line 1641: `size_t offset = most_sig_limb - (mp_limb_t*)result->cache` - `long` to `size_t` conversion
- Line 2182: `potential_new_size += highest_limb + 1` - `int` to `size_t` conversion
- Line 3240: `pool->threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t))` - `int` to `unsigned long` conversion
- Line 3634: `pool->threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t))` - `int` to `unsigned long` conversion
- Line 3953: `mpfr_prec_t precision = digits * 4 + GL_PRECISION_BITS` - `unsigned long` to `mpfr_prec_t` (which is `long`) conversion
- Line 3986: `mpfr_prec_t precision = state->digits * 4 + GL_PRECISION_BITS` - `unsigned long` to `mpfr_prec_t` conversion
- Line 4009: `state->current_term = i + 1` - `int` to `unsigned long` conversion
- Line 4294: `unsigned long chunk_size = state->terms / pool->num_threads` - `int` to `unsigned long` conversion
- Line 4297: `calc_task** tasks = (calc_task**)malloc(pool->num_threads * sizeof(calc_task*))` - `int` to `unsigned long` conversion
- Line 4299: `pool->num_threads * sizeof(chudnovsky_state)` - `int` to `unsigned long` conversion
- Line 4302: `unsigned long start = i * chunk_size` - `int` to `unsigned long` conversion
- Line 4303: `unsigned long end = (i == pool->num_threads - 1) ? state->terms : (i + 1) * chunk_size` - `int` to `unsigned long` conversion
- Line 5173: `config.max_digits = json_object_get_int64(j_max_digits)` - `int64_t` to `unsigned long` conversion
- Line 5232: `config.checkpoint_interval = json_object_get_int(j_checkpoint_interval)` - `int32_t` to `unsigned long` conversion
- Line 5820: `json_object_object_add(response, "digits", json_object_new_int64(digits))` - `unsigned long` to `int64_t` conversion
- Line 6010: `json_object_object_add(response, "digits", json_object_new_int64(digits))` - `unsigned long` to `int64_t` conversion
- Line 6083: `json_object_object_add(response, "digits", json_object_new_int64(jobs[job_idx].digits))` - `unsigned long` to `int64_t` conversion

### Format String Safety Issues

These warnings indicate potential security risks due to using non-literal format strings:

- Line 5007: `vsnprintf(formatted_message, BUFFER_SIZE, format, args)` - Format string is not a string literal
- Line 5056: `vsnprintf(formatted_message, BUFFER_SIZE, format, args)` - Format string is not a string literal

### Shadowed Variables

These warnings indicate that variables with the same name are declared in nested scopes, potentially leading to confusion and bugs:

- Line 4449: `mpz_t verify_p, verify_q, verify_t` - Shadows previous declaration at line 4377

### Floating-Point to Integer Conversion

These warnings indicate potential precision loss when converting from floating-point to integer types:

- Line 5578: `size_t memory_needed = (digits / 14.1816) * 8 * 3` - Conversion from `double` to `size_t`
- Line 5732: `size_t memory_needed = (digits / 14.1816) * 8 * 3` - Conversion from `double` to `size_t`

### Integer Precision Loss

These warnings indicate potential data truncation when converting from larger to smaller integer types:

- Line 6292: `int bytes_read = read(client_sock, buffer, BUFFER_SIZE - 1)` - `ssize_t` to `int` conversion

## Proper Fixes (Best Practices)

### Unused Parameters
- **Better than suppression:** Refactor function signatures to match their true requirements
- For signal handlers (`segfault_handler`, `handle_sighup`): Document why parameter is unused with a comment, then use `(void)sig;` at start of function
- For `validate_parameters`: Remove the parameter if not needed, or implement proper validation if it should be used
- For `run_cli_mode`: If CLI arguments aren't used, consider if the function even needs these parameters

### Unused Variables
- **Do not suppress:** Remove `pool`, `pool_initialized`, and `result` variables entirely if they're not needed
- If they were intended for future functionality, properly implement that functionality or add TODO comments

### Sign Conversion Issues
- **Not just casting:** Review each conversion for logical correctness
- Use appropriate types throughout (e.g., make `num_threads` an `unsigned` if appropriate)
- Add range checks before conversions to ensure values won't overflow or cause unexpected behavior
- For memory allocation, ensure checks against negative values and integer overflows
- For JSON field conversions, add explicit bounds checking

### Format String Safety
- **Security critical:** For `vsnprintf` calls in logging functions:
  - Validate format strings from untrusted sources
  - Use format string literals where possible
  - Consider using a safer logging library that handles format string validation

### Shadowed Variables
- Rename nested `verify_p`, `verify_q`, `verify_t` variables to unique names
- Consider using a naming convention that distinguishes variables at different scopes
- Refactor code to avoid the need for nested variables with similar purposes

### Floating-Point to Integer Conversion
- Add explicit conversion with rounding, ceiling, or floor based on the mathematical requirements:
  ```c
  size_t memory_needed = (size_t)ceil((digits / 14.1816) * 8 * 3);
  ```
- Validate the formula's correctness - is this a heuristic or an exact calculation?
- Add additional buffer to prevent memory underallocation

### Integer Precision Loss
- Use `ssize_t` for system call return values consistently:
  ```c
  ssize_t bytes_read = read(client_sock, buffer, BUFFER_SIZE - 1);
  ```
- Add explicit checks for large return values that might be truncated
- Consider impacts on 32-bit vs 64-bit platforms

## Build Commands

For Mac/Homebrew:
```
gcc -o pi_calculator pi_calculator.c -I/opt/homebrew/include -L/opt/homebrew/lib -lgmp -lmpfr -ljson-c -lm -pthread -O3
```

For Linux:
```
gcc -o pi_calculator pi_calculator.c -lm -lgmp -lmpfr -ljson-c -lpthread -O3
```

For checking warnings (recommended):
```
gcc -Wall -Wextra -o pi_calculator pi_calculator.c -lm -lgmp -lmpfr -ljson-c -lpthread -O3
```