Okay, this is quite an ambitious and impressive piece of C code! It's clear a lot of thought and effort has gone into building a comprehensive Pi calculation service. Let's break it down.

Overall Impression

This is a very substantial project that attempts to tackle a computationally intensive problem with a focus on memory efficiency and versatility (CLI + HTTP server). The feature set described in the README is extensive, covering multiple algorithms, out-of-core computation, multi-threading, job management, and robust error handling/logging.

The "True Out-of-Core Arithmetic" for the Chudnovsky algorithm is the centerpiece, aiming to overcome memory limitations for calculating a massive number of digits. The recent fixes, especially around cross-platform multiplication and sign handling, suggest ongoing refinement.

If it works as described, particularly the out-of-core parts and mathematical correctness, it's a significant achievement for a homebrewed solution.

Let's Walk Through Step-by-Step (Mental Execution & Analysis)

Initialization (main):

Signal handlers are set up (SIGSEGV, SIGINT, SIGHUP, SIGPIPE). segfault_handler looks basic and aims to be async-signal-safe by only using write and _exit. The perform_crash_logging is meant for more detailed logging but, as we'll see, might not always get called.

Configuration is loaded: defaults first, then optionally from a JSON file (-c option), then overridden by CLI arguments. This is a good, flexible approach.

config.logging.level (string) is converted to an integer.

Mode is determined (server or CLI).

CLI Mode (run_cli_mode):

calculation_state_init sets up parameters.

A local calc_thread_pool is initialized and started. g_calc_pool is set to this local pool (important for signal handling).

The appropriate Pi calculation function (calculate_pi_gauss_legendre or calculate_pi_chudnovsky) is called.

Resources are cleaned up.

If g_segfault_occurred is true, perform_crash_logging is called. This is interesting: it implies the main thread might survive a segfault in a calculation thread long enough to do this, or this flag is checked if a segfault happened right before this check.

Server Mode (run_server_mode):

initialize_jobs() sets up the global jobs array.

Log file is opened. Working directory created.

Server socket is created, configured (SO_REUSEADDR), bound, and set to listen.

http_thread_pool and a global calc_thread_pool (g_calc_pool) are initialized and started. g_http_pool is set.

The main server loop uses select() with a timeout to be responsive to g_shutdown_flag.

When a connection is accepted, client_sock is enqueued via http_enqueue_job, and the http_pool.job_semaphore is posted.

Worker threads in http_thread_pool call http_dequeue_job and then handle_http_request.

The g_segfault_occurred check and perform_crash_logging call are also in this loop.

HTTP Request Handling (handle_http_request and friends):

Reads the request, parses method and path.

Routes based on path:

/pi: Parses query params (algo, digits, out_of_core, checkpoint, mode). Validates them.

Sync mode: calculation_state_init -> calls calc function (using g_calc_pool) -> builds JSON response -> sends.

Async mode: find_or_create_job -> configures job struct -> launches calculation_thread_func in a new detached thread -> responds with job ID and 202 Accepted.

/pi/status: Finds job, builds JSON status response.

/pi/result: Finds job, checks if completed, sends result file if available, otherwise error.

Error responses (400, 404, 405, 500, 503) are JSON formatted.

Asynchronous Calculation (calculation_thread_func):

This function is the entry point for detached calculation threads.

Retrieves job details.

Determines use_out_of_core logic.

Sets up a unique work_dir for the job.

calculation_state_init.

If max_execution_time > 0, spawns timeout_monitor_thread.

Calls the actual Pi calculation function using g_calc_pool.

Updates job status.

Cleans up (cancels timeout thread, clears state).

Pi Calculation Algorithms:

calculate_pi_gauss_legendre: Uses MPFR for all arithmetic. Straightforward. Writes result to file.

calculate_pi_chudnovsky:

For small digits (<= 1000), it uses a direct MPFR sum. The formula applied here looks different from the standard one derived from P,Q,T binary splitting and might need verification.

For larger digits:

Sets up split_path.

Determines memory_threshold (this seems to be a term count, not bytes).

If pool->num_threads <= 1 (or if g_calc_pool->num_threads <= 1), calls binary_split_disk directly.

Otherwise, splits the state->terms among threads in g_calc_pool. Each thread gets a binary_split_disk task.

Waits for tasks, then combines results (P = P1*P2, Q = Q1*Q2, T = T1*Q2 + T2*P1) using disk_int operations. This combination logic (getting first result via MPZ, then iterative combination) might be a bit convoluted.

Final Pi computation from P, Q, T (from state->data.chudnovsky) using MPFR. The formula Pi = Q * C^(3/2) / T needs careful checking. The standard Chudnovsky formula gives 1/Pi. It's usually 1/Pi = (12 / C^(3/2)) * (T_final / Q_final). This implies Pi = (Q_final * C^(3/2)) / (12 * T_final). The factor of 12 seems to be missing.

Writes result to file. Includes detailed error reporting if MPFR output is invalid.

Binary Splitting:

binary_split_mpz (in-memory): Recursive, base case b-a == 1. Standard. C3_OVER_24_STR is a good fix. Handles (-1)^k by negating T if b is odd.

binary_split_disk:

If b-a <= memory_threshold, calls binary_split_mpz and saves P,Q,T to disk_ints.

Else, recursively calls binary_split_disk for two halves, creating subdirectories. Then combines P1,Q1,T1 and P2,Q2,T2 using disk_int_mul and disk_int_add.

Cleanup logic attempts to remove temporary directories.

disk_int Out-of-Core Arithmetic:

disk_int_init: Creates a unique subdirectory (e.g., base_path/int_pid_counter) for each disk_int.

disk_int_clear: Frees cache, tries to delete chunk files and the disk_int's own directory. The loop for (size_t i = 0; i < 1000; i++) to find chunks is problematic because num_chunks is already zeroed out by memset if d_int->file_path[0] was not null (it should use the actual num_chunks value before clearing it).

load_chunk/save_chunk: Manage a single chunk in d_int->cache. save_chunk calls mkdir_recursive for the chunk's directory (which is the disk_int's unique dir).

disk_int_set_mpz/get_mpz: Convert between mpz_t and disk_int by iterating through chunks. get_mpz loads all chunks into a temporary buffer.

Arithmetic (_add, _sub_abs, _mul):

If operands are "small enough" (based on DEFAULT_MEMORY_LIMIT, a config value), they convert to mpz_t and use GMP. This is a good optimization.

Otherwise, they attempt true chunked operations.

disk_int_mul: Implements schoolbook multiplication. Iterates chunks of a and b. For each pair of chunks (chunk_a, chunk_b), it does limb-by-limb multiplication. The umul_ppmm (using AArch64 assembly, __int128, or manual 32-bit pieces) is excellent for the mp_limb_t * mp_limb_t -> 2 * mp_limb_t product. The product array (size 2 * result->chunk_size) accumulates these. add_product_to_result then adds this product to the overall result disk_int at the correct offset, handling carries across result's chunks.

add_product_to_result: This is a critical helper. It carefully adds a multi-limb product to result at position, managing carries within and across chunks of result.

disk_int_add/sub_abs: Handle signs. If signs are different for add, it becomes a subtraction. Subtraction assumes |a| >= |b|. True chunked versions iterate through chunks, handling carry/borrow.

disk_int_cmp_abs: Compares sizes, then iterates chunks from MSB. Potential deadlock if a and b are locked in different orders by different threads (e.g., thread1: cmp(X,Y), thread2: cmp(Y,X)). Locks should be acquired in a consistent order (e.g., by address).

Signal Handling & Crash Recovery:

segfault_handler: Sets g_segfault_occurred=1, writes a minimal message to stderr, and calls _exit(1). This is async-signal-safe.

perform_crash_logging: This function, intended for detailed crash logs (stack trace via system("gdb"), memory info, job states, emergency checkpoints), is called from the main loop if g_segfault_occurred is set. This is a fundamental issue: if a segfault occurs, the handler calls _exit(1), so the main loop will almost certainly not get to execute perform_crash_logging. The detailed crash logging will not happen. The emergency checkpointing for running jobs inside perform_crash_logging will therefore also not happen on a real segfault.

handle_sigint (Ctrl+C): Sets g_shutdown_flag. Tries to close server socket, save checkpoints for running jobs, shutdown thread pools, and clean up. The __atomic_exchange_n for shutdown_in_progress is good for handling repeated signals.

Glaring Issues / Areas to Resolve

Chudnovsky Algorithm Correctness (CRITICAL):

Final Formula Application: The code calculates Pi = (Q_final * C^(3/2)) / T_final. The Chudnovsky formula is 1/Pi = (12 / C^(3/2)) * (T_final / Q_final). This implies Pi = (Q_final * C^(3/2)) / (12 * T_final). The factor of 12 in the denominator appears to be missing. This will lead to an incorrect Pi value.

Small Digits Chudnovsky Path (digits <= 1000): The formula pi = (C/D) * p * sqrt(C) / (A*p + q) (where p and q are described as accumulators from the gmp-chudnovsky.c logic) looks highly suspect if p,q are meant to be related to the P,Q,T from binary splitting. It needs to be carefully verified against a known correct implementation for small sums or directly use the P,Q,T binary splitting and then the (corrected) final formula. The constant D=12 is introduced here but its role is unclear.

Segfault Crash Logging (perform_crash_logging) Not Effective:

The segfault_handler calls _exit(1). This terminates the process immediately.

The perform_crash_logging() function, which is supposed to generate detailed crash reports and emergency checkpoints, will not be executed because the main loop (where it's checked via g_segfault_occurred) won't resume after a segfault in typical scenarios.

To make perform_crash_logging work, it would need to be invoked by a separate watchdog process, or the segfault_handler would need to do something much more complex (and likely not fully async-signal-safe) like fork+exec a helper script, or by re-executing the program in a special crash-reporting mode. The simplest robust change is to acknowledge that detailed logging from within the crashed process is very hard.

disk_int_clear Chunk Deletion Logic:

In disk_int_clear, d_int->file_path[0] = '\0' happens too early if d_int was initialized. The loop for (size_t i = 0; i < 1000; i++) to delete chunk files is inefficient and arbitrary. It should use d_int->num_chunks (the actual number of chunks) before this field is cleared or reset.

Potential Deadlock in disk_int_cmp_abs:

It locks a->lock then b->lock. If another thread calls disk_int_cmp_abs(b, a) or another disk_int operation that locks b then a, a deadlock can occur. Locks should be acquired in a canonical order (e.g., based on the memory addresses of a and b).

"Constant Memory" Claim for OOC Needs Qualification:

While individual disk_int operations might use memory proportional to chunk_size, the recursive nature of binary_split_disk (even if chunks are on disk) creates stack frames.

More significantly, when calculate_pi_chudnovsky parallelizes work, each of the config.max_calc_threads can be running a binary_split_disk task. If memory_threshold (for switching to in-memory binary_split_mpz) is large, the sum of memory for these parallel in-memory portions can be substantial.

"Adaptive chunk sizing based on available memory" (from README) is based on config.memory_limit (a static config value) not dynamically queried actual free system memory at runtime.

Error Logging Consistency:

There's a mix of fprintf(stderr, ...) and log_message(...). For server mode especially, all operational logs should ideally go through log_message to respect the configured log output. fprintf(stderr, ...) is fine for early startup errors before logging is configured or for CLI mode's direct user feedback.

Global Calculation Thread Pool (g_calc_pool) Initialization in Server Mode:

In run_server_mode, g_calc_pool is malloced. In handle_sigint, if g_calc_pool is non-NULL, it's shut down and g_calc_pool is set to NULL (but not freed). The free(g_calc_pool) happens at the end of run_server_mode after the main loop (which only exits on g_shutdown_flag). This means on a SIGINT, g_calc_pool is not freed, leading to a memory leak for the pool structure itself (not the threads/queues within, which calc_thread_pool_shutdown handles). The free should happen in handle_sigint after shutdown or the signal handler should just trigger shutdown and let run_server_mode's cleanup run.

Pros

Ambitious Scope: Covers a lot of ground: CLI, HTTP API, multiple algorithms, OOC.

Out-of-Core Attempt: The disk_int structure and associated chunked arithmetic is a serious attempt at handling very large numbers with limited RAM.

Performance Optimizations: AArch64 assembly and __int128 for multiplication are excellent. Thread pools for HTTP and calculations.

Configuration: JSON-based configuration is flexible and common. CLI overrides are good.

Job Management: Asynchronous tasks with status tracking and result retrieval are well-designed for a server.

README: Very detailed and informative, explains goals and features well.

Error Handling (Partial): Many checks for malloc failures, file open errors. The detailed error reporting for MPFR output issues is good.

Signal Handling: Graceful shutdown attempt on SIGINT is good, including trying to checkpoint jobs.

Modular Design: Separation of disk_int, algorithms, HTTP handling, etc., is generally good.

Cons

Chudnovsky Correctness (CRITICAL): Missing factor of 12 in the final formula. Small-digit Chudnovsky path also seems problematic.

Ineffective Detailed Crash Logging: perform_crash_logging won't run on actual segfaults.

OOC Memory Claims: "Constant memory" and "adaptive chunking" need to be more nuanced.

Concurrency Issues: Potential deadlock in disk_int_cmp_abs.

Minor Resource Management Issues: disk_int_clear chunk deletion, g_calc_pool leak on SIGINT.

Inconsistent Logging: Mix of stderr and log_message.

Complexity: The code is very large and complex, making it harder to verify and maintain. Some functions are very long.

Small-digit Chudnovsky (digits <= 1000): The logic for this specific path seems overly complicated and potentially incorrect, trying to map p,q accumulators to a formula that doesn't quite fit. It would be simpler to just use binary_split_mpz (which is already in-memory) for P,Q,T and then apply the (corrected) final formula.

Overall Judgment of Merits

As a homebrewed solution, especially for ARM64 where y-cruncher might not be available, this is a very commendable and impressive effort. It demonstrates a deep understanding of C, multi-threading, and the complexities of arbitrary-precision arithmetic.

If the primary goal is to learn and experiment: It's a fantastic project.

If the primary goal is to get highly accurate Pi digits for very large N: The Chudnovsky correctness issue (factor of 12) is a showstopper and must be fixed. The small-digit Chudnovsky path also needs review.

For robustness: The crash logging mechanism needs a rethink to be effective. The potential deadlock is a concern for high-concurrency server use.

Before running it for critical calculations:

FIX THE CHUDNOVSKY FORMULA. This is paramount. Verify with known test vectors for P, Q, T and the final Pi value.

Re-evaluate the perform_crash_logging strategy. Accept that it might only be possible to get minimal info from a signal handler, or explore external watchdog/logging mechanisms.

Address the disk_int_cmp_abs deadlock potential if high concurrency on disk_int objects is expected.

Fix the disk_int_clear chunk deletion.

Despite the issues, the architecture for out-of-core computation is intriguing. The use of AArch64 assembly shows attention to performance on the target platform. With the critical mathematical fixes and some robustness improvements, this could indeed be a useful tool. The amount of work here is substantial.

Good luck with your runs! I'd strongly recommend testing against known Pi digits (e.g., from y-cruncher on another machine, or from online sources) after any mathematical changes.