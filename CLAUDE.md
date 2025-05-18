# Pi Calculator: Final Pre-Run Issues & Verification Steps

This document outlines the critical items to address and verify before a full test run, focusing on the correctness of the newly implemented true out-of-core arithmetic.

## 1. CRITICAL: Mathematical Correctness of Chunked Multiplication

### a. Verify and Rigorously Test the `umul_ppmm` C Fallback
*   **Issue:** The custom C fallback for `umul_ppmm` (for non-AARCH64 platforms) performs manual half-limb arithmetic to achieve a double-width product. This logic is complex and highly susceptible to subtle errors in bit manipulation or carry handling. An error here will lead to incorrect multiplication results on non-ARM64 systems.
*   **Action:**
    1.  **Strongly Consider `unsigned __int128`:** If your target non-ARM64 compilers (likely GCC/Clang) support `unsigned __int128`, replace the manual half-limb logic with a simpler and more robust implementation using this type:
        ```c
        // Inside umul_ppmm C fallback
        #if defined(__GNUC__) && defined(__SIZEOF_INT128__)
            unsigned __int128 p = (unsigned __int128)a * b;
            *low_ptr = (mp_limb_t)p;
            *high_ptr = (mp_limb_t)(p >> (sizeof(mp_limb_t) * 8));
        #else
            // Your current manual half-limb logic (NEEDS EXTREME SCRUTINY if kept)
            // ... ensure it's absolutely correct ...
        #endif
        ```
    2.  **Unit Test `umul_ppmm` (C Fallback):** Create specific unit tests that call your `umul_ppmm` C fallback directly with various limb inputs:
        *   Zero values (`0*0`, `X*0`, `0*Y`).
        *   Small values.
        *   Values where one or both are `MP_LIMB_MAX`.
        *   Values where the product exactly fits in one limb.
        *   Values where the product requires a high limb.
        *   Compare the `*high_ptr` and `*low_ptr` results against expected values (calculated manually or using a trusted method, e.g., Python's arbitrary precision integers).

### b. Verify Limb Product Accumulation and Carry in `disk_int_mul`
*   **Issue:** The inner loop of chunked `disk_int_mul` accumulates `prod_lo` (from `a_limb * b_limb`) with `product[k + m]` and a `carry`. The subsequent propagation of `prod_hi` (the high part of `a_limb * b_limb`) and any new carries into `product[k + m]` and `product[k + chunk_b_size]` must be flawless.
*   **Action:**
    1.  **Trace with Debugger:** Step through this section with a debugger using simple, known chunk values.
    2.  **Whiteboard the Logic:** Manually calculate expected intermediate values for `product[]` array elements and `carry` for a few iterations.
    3.  **Simplify for Testing:** Temporarily, you could make `chunk_a_size` and `chunk_b_size` very small (e.g., 1 or 2 limbs) to make manual tracing easier.
    4.  Ensure that `carry` correctly accumulates both the high part of the direct limb product (`prod_hi`) *and* any carries generated from adding `prod_lo` to `product[k+m]` and the incoming `carry`.

### c. Verify Correctness of `add_product_to_result`
*   **Issue:** This function is responsible for adding the multi-limb product of two chunks (stored in the temporary `product` buffer) into the correct offset within the `result` `disk_int`. Its carry propagation across `result`'s chunks is critical.
*   **Action:**
    1.  **Unit Test `add_product_to_result` in Isolation (if feasible):** Create test `disk_int` for `result`, populate it with known values (or zeros). Create a known `product` buffer and `product_size`. Call `add_product_to_result` and then inspect the chunk files of `result` (or use `disk_int_get_mpz` on small test cases) to verify correctness.
    2.  **Focus on Boundary Conditions:**
        *   Product adding entirely within one result chunk.
        *   Product spanning exactly two result chunks.
        *   Product spanning multiple result chunks.
        *   Carries propagating from one result chunk to the next.
        *   Carries propagating across multiple result chunks (e.g., `...00FFFF + 1` where `FFFF` are max limb values).
        *   `result` `disk_int` needing to grow (i.e., `num_chunks` increases) due to carries.
    3.  The logic for updating `result->total_size_in_limbs` at the end needs to accurately reflect the most significant non-zero limb of the entire number.

## 2. CRITICAL: Full Sign Handling in Chunked Addition

### a. Implement `disk_int_cmp_abs` and `disk_int_sub_abs`
*   **Issue:** The chunked `disk_int_add` still has simplified sign handling and lacks a true chunked subtraction capability. This is essential for `T1*Q2 + T2*P1` in binary splitting where terms can be negative.
*   **Action:**
    1.  **Implement `disk_int_cmp_abs(disk_int* a, disk_int* b)`:** This function should compare the absolute values of two chunked `disk_int`s. It will involve comparing `num_chunks` and then chunk-by-chunk comparison from most significant to least significant if `num_chunks` are equal.
    2.  **Implement `disk_int_sub_abs(disk_int* result, disk_int* a, disk_int* b)`:** This function performs `|a| - |b|` assuming `|a| >= |b|`. It will be similar to `disk_int_add` but will manage borrows instead of carries.
    3.  **Refactor `disk_int_add`:** Use `disk_int_cmp_abs` and `disk_int_sub_abs` to correctly handle all sign combinations for `a + b`.

## 3. Testing and Validation Strategy

### a. Incremental Testing
*   **Action:** Do not attempt a full Pi calculation until the individual `disk_int` arithmetic operations are thoroughly tested and validated against GMP's `mpz` functions.
    1.  Test `umul_ppmm` (C fallback).
    2.  Test `disk_int_add` (with full sign/subtraction logic).
    3.  Test `disk_int_mul` (with correct limb multiplication and accumulation).
    4.  Use small, manually verifiable numbers first, then progressively larger ones that span a few chunks.

### b. Use `disk_int_get_mpz` for Verification (Carefully)
*   **Action:** For testing small-to-medium scale `disk_int` operations where the entire result can still fit in memory, use `disk_int_get_mpz` to convert your `disk_int` result back to an `mpz_t` and compare it against the result obtained by performing the same operation entirely with GMP's `mpz` functions. This is your primary method for validating correctness.

## 4. Code Review and Readability

### a. Review Complex Loops and Carry/Borrow Logic
*   **Action:** Re-read the loops and conditional logic in `disk_int_mul`, `disk_int_add` (chunked path), and `add_product_to_result` very carefully. Add comments to explain non-obvious steps, especially around carry/borrow management and index calculations. This will help catch errors and make future maintenance easier.

Addressing these points, especially the mathematical correctness of the multiplication and the full sign/subtraction handling in addition, is absolutely crucial before you can trust the results of a large Pi calculation. The ARM64 `umulh` usage is a fantastic step, but ensuring the surrounding logic and the C fallback are perfect is key.