# Pi Calculator: Remaining Issues & Next Steps (Post-Initial Chunking)

This document outlines the key areas to focus on following the initial implementation of chunked `disk_int` operations.

## 1. Critical: Correctness of Chunked Arithmetic

### a. Implement Correct Limb-Level Multiplication in `disk_int_mul`
*   **Issue:** The current chunked `disk_int_mul` uses a simplified limb multiplication (`prod = a_limb * b_limb + ...`) that will truncate results, leading to incorrect products for the overall numbers.
*   **Action:**
    1.  Research or use GMP's internal functions (if accessible and appropriate, e.g., `mpn_mul_1` or understanding `umul_ppmm`) for multiplying two `mp_limb_t`s to get a two-limb result (low and high parts).
    2.  Modify the inner loop of `disk_int_mul` to correctly perform `(limb_a * limb_b) + existing_product_limb + carry_from_previous_limb_multiplication`. This accumulation must handle a multi-limb carry (potentially two limbs from the product plus one from accumulation).
    3.  Ensure the `product` buffer (currently `mp_limb_t* product`) and the logic in `add_product_to_result` can correctly handle these potentially wider intermediate products and their carries.

### b. Robust Sign Handling & Subtraction in `disk_int_add`
*   **Issue:** The current `disk_int_add` does not fully handle addition when operands have different signs (which is subtraction). The sign of the result is also simplified.
*   **Action:**
    1.  Implement a chunked magnitude comparison function: `int disk_int_cmp_abs(disk_int* a, disk_int* b)`.
    2.  Based on signs and a_abs vs b_abs:
        *   If `a > 0, b > 0`: `result = a + b`, sign is `+`.
        *   If `a < 0, b < 0`: `result = |a| + |b|`, sign is `-`.
        *   If `a > 0, b < 0`:
            *   If `|a| >= |b|`: `result = |a| - |b|`, sign is `+`.
            *   If `|a| < |b|`: `result = |b| - |a|`, sign is `-`.
        *   If `a < 0, b > 0`:
            *   If `|a| >= |b|`: `result = |a| - |b|`, sign is `-`.
            *   If `|a| < |b|`: `result = |b| - |a|`, sign is `+`.
    3.  Implement a `disk_int_sub_abs(result, a_abs, b_abs)` function for chunked absolute subtraction (assuming `a_abs >= b_abs`), handling borrows across chunks.

## 2. Important: Out-of-Core Integrity & Efficiency

### a. Ensure `disk_int` Operations are Truly Out-of-Core
*   **Issue:** While `disk_int_set_mpz` is chunked, `disk_int_get_mpz` still loads the entire number into an in-memory `mpz_t`. If any critical path operation (like the final MPFR conversion or a fallback path *within* a supposedly chunked operation) uses `disk_int_get_mpz` on a huge number, the OOC benefit is lost for that step.
*   **Action:**
    1.  Review all call sites of `disk_int_get_mpz`. Ensure it's only used for:
        *   Fallback paths in `disk_int_add`/`mul` when numbers *are confirmed* to fit in memory.
        *   The final conversion to `mpfr_t` (this step inherently might require significant memory, but the preceding arithmetic should have been OOC).
    2.  The core logic of `disk_int_add` (chunked path) and `disk_int_mul` (chunked path) should *only* use `load_chunk` and `save_chunk` for I/O, manipulating data via their `cache` member, not by converting entire `disk_int` operands to `mpz_t`.

### b. Refine Carry/Borrow Propagation in Chunked Arithmetic
*   **Issue:** Carry/borrow logic across chunks (`disk_int_add`, `disk_int_sub_abs`, `add_product_to_result`) is complex and needs to be flawless.
*   **Action:**
    1.  Rigorously test carry/borrow propagation with edge cases (e.g., carries propagating over multiple empty chunks, carries generating new most significant chunks).
    2.  Use small, known inputs and manually verify intermediate chunk values and carries.

### c. Optimize Chunk Management and Caching
*   **Issue:** The current `disk_int` has a single-chunk cache. This might be suboptimal for operations like multiplication that access multiple input chunks and write to result chunks.
*   **Action (Can be iterative):**
    1.  **Initial:** Ensure the current single-chunk cache per `disk_int` is used effectively (e.g., `load_chunk` only loads if not already cached and different).
    2.  **Consideration for `disk_int_mul`:** The `disk_int_mul` logic might benefit from explicitly managing temporary `mpz_t`s for the two source chunks (`a->cache`, `b->cache`) and the accumulating portion of the `result->cache` rather than relying solely on the `disk_int`'s own single-chunk cache for all three during the `add_product_to_result` step.
    3.  **Future:** Explore more advanced caching (e.g., a small LRU cache for recently used chunks if profiling shows benefits).

## 3. Robustness and Testing

### a. Comprehensive Unit Tests for `disk_int` Operations
*   **Issue:** The new chunked arithmetic is complex and highly prone to off-by-one errors or incorrect carry/borrow handling.
*   **Action:**
    1.  Create a dedicated test suite for `disk_int`.
    2.  Test `disk_int_add`, `disk_int_sub_abs` (once implemented), and `disk_int_mul` with:
        *   Small numbers.
        *   Numbers of different lengths.
        *   Numbers that span multiple chunks.
        *   Numbers involving extensive carry/borrow.
        *   Operations involving zero.
        *   Operations with negative numbers.
    3.  Compare results against GMP's in-memory operations (`mpz_add`, `mpz_mul`).

### b. Memory Leak and Resource Management Testing
*   **Issue:** The new chunking logic involves more dynamic memory for caches and file handling.
*   **Action:** Use Valgrind (or similar tools) regularly to check for memory leaks in the `disk_int` functions and their usage paths. Ensure all file handles are closed.

## 4. Lower Priority / Future Enhancements

### a. Advanced Multiplication Algorithms (Karatsuba/Toom-Cook)
*   **Action:** Once schoolbook chunked multiplication is stable and correct, plan the implementation of Karatsuba (or Toom-Cook) for chunked `disk_int`s to improve performance for very large numbers. This will involve recursive calls operating on `disk_int` sub-ranges.

### b. Parallelization of Chunk Operations
*   **Action:** Explore parallelizing parts of the chunked multiplication (e.g., calculating partial products `chunk_a[i] * chunk_b[j]`) using the existing `calc_thread_pool`.

This refined list should guide your next phase of development. The focus should be heavily on the mathematical correctness and robustness of the chunked arithmetic before moving to more advanced optimizations.