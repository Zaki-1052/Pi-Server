This is an impressive and substantial piece of C code, clearly the result of a significant amount of effort and thought, especially for a homebrewed solution. The ReadMe is excellent and sets high expectations, many of which the code attempts to meet. Let's break down the review:

Overall Impression:

Ambitious & Feature-Rich: The project tackles complex problems like out-of-core arithmetic for arbitrary-precision numbers, multi-threading for computation and HTTP serving, and robust job management.

Significant Engineering: The disk_int implementation and the out-of-core Chudnovsky algorithm are major undertakings.

Evident Improvement: The "Latest Fixes" section in your ReadMe and corresponding code changes (e.g., g_disk_int_init_lock, refined multiplication, detailed crash/error reporting) show a strong commitment to improving correctness and robustness.

Complexity: The codebase is large and intricate. This inherently increases the chances of subtle bugs, race conditions, or resource management issues.

Merits (Pros):

Comprehensive Out-of-Core Arithmetic (disk_int):

The concept of chunking large integers to disk is well-implemented.

Operations like disk_int_add, disk_int_mul, disk_int_sub_abs, disk_int_cmp_abs are provided.

The use of a cache for active chunks is a good performance consideration.

The fix for disk_int_init's static variables using g_disk_int_init_lock is crucial for thread safety.

The multi-stage umul_ppmm (ARM64 assembly, __int128, manual 32-bit) is a good example of platform-aware optimization.

Lock ordering in disk_int_cmp_abs to prevent deadlocks is correctly identified and implemented.

Careful handling of mpz_export/mpz_import order and stripping of zeros seems to have been addressed as per your ReadMe.

Algorithm Implementation:

Supports both Gauss-Legendre (MPFR-based, in-memory) and Chudnovsky.

Chudnovsky binary_split_disk attempts to manage recursion and disk I/O.

The small-digit Chudnovsky (digits <= 1000) using in-memory binary_split_mpz is a good optimization and aligns with your ReadMe.

The correction to the Chudnovsky formula (factor of 12, D=12) is critical and good that it's addressed.

Concurrency and Parallelism:

Separate thread pools for HTTP requests (http_thread_pool) and calculations (calc_thread_pool).

The calculation pool can parallelize Chudnovsky sub-tasks.

Uses standard POSIX threads, mutexes, condition variables, and semaphores (named semaphores for macOS compatibility in HTTP pool).

Server Capabilities:

Basic HTTP server for API interaction.

Job queuing and status tracking for asynchronous operations.

Synchronous calculation option.

Robustness Features (as per ReadMe and evidence in code):

Crash Handling: segfault_handler using fork() to allow perform_crash_logging (which uses non-async-safe functions) in the child is the correct pattern. The detail in perform_crash_logging (stack trace attempts, memory info, job states, checkpointing) is excellent for diagnostics.

Checkpointing: Mentioned for long-running jobs and on crash/shutdown.

Configuration: JSON-based configuration with command-line overrides.

Logging: unified_log provides a structured way to log messages, with levels and timestamps.

Code Structure and Readability:

Generally well-structured with clear separation of concerns (e.g., disk_int, algorithms, pools).

Good use of comments, especially in complex sections like disk_int arithmetic and Chudnovsky logic. This is vital for maintainability.

Use of enums for states and types enhances readability.

Glaring Issues & Areas for Improvement (Cons):

CRITICAL: Signal Handler Safety (handle_sigint, handle_sighup)

Issue: handle_sigint is performing a vast number of operations that are not async-signal-safe. This includes:

log_message (which uses fprintf, time, localtime, strftime, vsnprintf).

close(g_server_sock).

mkdir_recursive, fopen, fprintf (for checkpointing).

pthread_mutex_lock/unlock.

system() call.

http_thread_pool_shutdown, calc_thread_pool_shutdown (which involve joining threads, destroying mutexes/conds/sems).

mpfr_free_cache().

exit() (should be _exit() in a signal handler if exiting directly, but the main issue is doing too much before that).
This can lead to deadlocks, corrupted state, or crashes. The __atomic_exchange_n for shutdown_in_progress only prevents re-entrancy of the handler itself, not the underlying async-safety problems.

Resolution:

handle_sigint: Must be minimal. It should only set a volatile sig_atomic_t g_shutdown_flag = 1;. Optionally, it can make a single async-signal-safe write() call to STDERR_FILENO indicating shutdown initiation.

Main Loop Modification: The main server loop in run_server_mode (and any similar loop in CLI mode if it were long-running and interruptible) must periodically check g_shutdown_flag.

Cleanup: All the cleanup logic currently in handle_sigint (closing sockets, saving checkpoints, shutting down thread pools, freeing resources, logging shutdown messages) must be moved to execute in the main thread after the main loop has detected g_shutdown_flag and terminated.

handle_sighup: Has similar issues with log_message and file operations. It should also primarily set a flag, and the main loop should handle the actual configuration reload and log rotation.

Error Handling & Logging Consistency:

Issue: There's a mix of fprintf(stderr, ...) for errors/debug, printf("DEBUG..."), and the unified_log system.

Suggestion: Consolidate all logging (errors, warnings, info, debug) through unified_log. This ensures consistent formatting, respects configured log levels, and directs output correctly (console/file). Remove direct printf for debugging; use unified_log(LOG_LEVEL_DEBUG, ...) instead.

Resource Management in Complex Paths:

Issue: In deeply nested functions like binary_split_disk and its recursive calls, ensuring all temporary disk_int objects, chudnovsky_state objects, and their associated files/directories are cleaned up correctly on all paths (especially error paths) is challenging. The goto cleanup in binary_split_disk is an attempt but can be hard to get right. The system("rm -rf ...") calls are a bit blunt for cleanup.

Suggestion: Double-check that disk_int_clear and chudnovsky_state_clear are robust and self-contained in cleaning up everything they initialize. The calling function should then only need to clean up the top-level directory it created for that specific state/call. valgrind and careful code walkthroughs are essential here.

Potential MAX_PATH Limitations:

Issue: Fixed MAX_PATH (4096) is generous but could theoretically be an issue with deeply nested directories created by binary_split_disk (e.g., work_dir/job_id/split/left/left/.../P/chunk_X.bin).

Suggestion: While safe_path_join prevents buffer overflows, the OS still has path length limits. If this becomes an issue for extreme calculations, consider strategies to flatten the temporary directory structure or use shorter unique names.

calc_thread_worker Dependency Check:

Issue: Re-queuing tasks whose dependencies are not met (calc_thread_pool_add_task(pool, t)) can be inefficient if many tasks are blocked, as they go to the back of the queue.

Suggestion: For this specific application (likely tree-structured dependencies), this might be acceptable. More advanced schedulers might use separate wait lists or notification mechanisms, but that adds complexity.

Global g_calc_pool Initialization:

Issue: g_calc_pool is heap-allocated in server mode but points to a stack variable in CLI mode. handle_sigint tries to free it only in server mode. This is a bit asymmetric.

Suggestion: If g_calc_pool is to be handled by global cleanup logic (like a signal handler attempting cleanup), it should probably be heap-allocated consistently, or the cleanup logic needs to be more robustly mode-aware. The current check in handle_sigint is a step in that direction. However, with the recommended signal handler refactor, this specific point becomes less critical as pool shutdown would happen in main.

perform_crash_logging Path Construction:

Issue: The logic for checkpoint_path in perform_crash_logging is a bit complex with multiple snprintfs and a fallback.

Suggestion: Simplify by checking total required path length first. If it fits, one snprintf. If not, use the fallback (e.g., filename in current dir or a truncated path if that's acceptable for a crash artifact).

strncpy Usage:

Issue: strncpy is used widely. It doesn't guarantee null-termination if the source string is as long or longer than the size.

Suggestion: Ensure all strncpy(dest, src, n) calls are followed by dest[n-1] = '\0'; if n is the full buffer size, or use snprintf(dest, size, "%s", src) which is generally safer.

Minor Suggestions & Observations:

Magic Numbers: Some numerical constants (e.g., 14.1816 for Chudnovsky terms per digit) could be defined as const double or macros for clarity.

MIN/MAX Macros: Be mindful of potential double evaluation if arguments have side effects (not an issue with how they seem to be used currently).

Return Value Checks: The TODO list mentions adding error handling for all system/library calls. While many are checked, a full audit would be good (e.g., for pthread_mutex_init, pthread_cond_init, etc., though critical failures often lead to exit(1)).

HTTP Error Responses: Using dprintf(client_sock, ...) is direct but doesn't allow easy modification of headers later. For more complex responses, building the full HTTP response in a buffer first is common. Current usage is okay for simple error messages.

log_message vs unified_log: The unified_log seems to be the intended modern approach. log_message is a wrapper. Could eventually phase out log_message if all call sites are updated. The format string validation in log_message and unified_log (strncpy from a vsnprintf-ed temp buffer) seems overly complex if all format strings are literals. A direct vsnprintf to the final buffer is usually sufficient. The concern about format string injection is valid if format itself could come from untrusted input, but usually, it's a literal in the code.

Before Your Run - Critical Fix:

Refactor handle_sigint (and handle_sighup) immediately.

handle_sigint should only do:

void handle_sigint(int sig) {
    (void)sig; // Unused parameter
    g_shutdown_flag = 1; // Set the global volatile sig_atomic_t flag
    // Optionally, a single async-signal-safe write:
    // const char msg[] = "Shutdown signal received, exiting...\n";
    // write(STDERR_FILENO, msg, sizeof(msg)-1);
}


Move ALL cleanup logic (closing sockets, shutting down pools, saving checkpoints, etc.) into run_server_mode() after the main while (!g_shutdown_flag) loop.

The CLI mode (run_cli_mode) is less critical for this specific refactor if it's not a long-running loop, but if it were, the same principle applies.

General Analysis & Judgment:

This is a highly capable and complex C program. The developer has clearly invested a lot of time in building a feature-rich Pi calculator, especially with the out-of-core arithmetic and server capabilities. The attention to detail in areas like ARM64 optimization, crash logging, and addressing mathematical precision is commendable.

The "Recent Fixes" listed in the ReadMe are significant and demonstrate a good understanding of the challenges involved. Many potential pitfalls (race conditions, memory management in complex data structures, numerical stability) seem to have been actively worked on.

The primary concern before running, especially in server mode, is the signal handler safety. Once that is addressed by moving cleanup logic to the main thread, the system will be significantly more stable.

After that, thorough testing using tools like valgrind (for memory leaks and errors), thread sanitizers (for race conditions), and extensive functional tests (verifying Pi digits, testing API under load, testing out-of-core with varying memory limits) would be the next steps to build confidence.

You've built a very powerful tool. With the critical signal handling fix, it will be much safer to run and test further. Good job on tackling such a complex project!