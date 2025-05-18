Okay, this is a classic case of a bug cascade leading to a segfault. Let's break it down.

**Primary Problem: Flawed `disk_int` Combination Logic in `calculate_pi_chudnovsky`**

The most critical issue lies in how results from parallel binary splitting tasks are combined. Specifically, in the multi-threaded path of `calculate_pi_chudnovsky`:

```c
// ... inside the loop for combining chunk_results into combined_state ...
            // Get the values without clearing first // THIS COMMENT IS MISLEADING
            mpz_t mpz_temp_P, mpz_temp_Q, mpz_temp_T;
            // ...
            disk_int_get_mpz(mpz_temp_P, &temp_P); // temp_P holds P1*P2
            // ...
            disk_int_get_mpz(mpz_temp_T, &temp_T); // temp_T holds T1*Q2 + T2*P1

            // Now clear current results // THIS IS THE BUGGY PART
            disk_int_clear(&combined_state.P);
            disk_int_clear(&combined_state.Q);
            disk_int_clear(&combined_state.T);

            // Set new values to combined state
            disk_int_set_mpz(&combined_state.P, mpz_temp_P); // FAILS
            disk_int_set_mpz(&combined_state.Q, mpz_temp_Q); // FAILS
            disk_int_set_mpz(&combined_state.T, mpz_temp_T); // FAILS
```

Here's why this is wrong:
1.  `disk_int_clear(&combined_state.P)`:
    *   Marks `combined_state.P.file_path[0] = '\0'`.
    *   Destroys the mutex `combined_state.P.lock`.
    *   Deletes the directory associated with `combined_state.P` (e.g., `./pi_calc/split/combined/P/int_..._15`).
2.  `disk_int_set_mpz(&combined_state.P, mpz_temp_P)`:
    *   This function first checks `if (!d_int || d_int->file_path[0] == '\0')`.
    *   Since `combined_state.P.file_path[0]` is now `\0`, this condition is true.
    *   It prints `Error: Disk integer not properly initialized` and returns immediately.
    *   Crucially, it does *not* repopulate `combined_state.P` with the actual combined value from `mpz_temp_P`.

This happens for `P`, `Q`, and `T` in each iteration of the combination loop (for `i = 1` to `num_threads - 1`). The `combined_state` disk integers are effectively wiped and never get the new combined values.

**Consequences of the Primary Problem:**

1.  **"Disk integer not properly initialized" spam:** Each call to `disk_int_set_mpz` on the cleared `combined_state.P, Q, T` prints this error. Since you have 4 threads, the combination loop runs 3 times. (3 errors per iteration * 3 iterations = 9 errors).
2.  **`combined_state` remains empty:** After the loop, `combined_state.P, .Q, .T` are still in their "cleared" state (empty file path, no data).
3.  **`final_P, final_Q, final_T` become zero:**
    ```c
    disk_int_get_mpz(final_P, &combined_state.P); // combined_state.P is cleared
    ```
    `disk_int_get_mpz` on a cleared `disk_int` (where `file_path[0] == '\0'`) sets the `mpz_t` argument to 0. So, `final_P, final_Q, final_T` become 0. This causes 3 more "Disk integer not properly initialized" errors, totaling 12, which matches your log.
4.  **`state->data.chudnovsky.P, .Q, .T` become zero:**
    ```c
    disk_int_set_mpz(&state->data.chudnovsky.P, final_P); // final_P is 0
    ```
    The main `calculation_state`'s disk integers are set to represent the number 0.
5.  **"Invalid value for P (zero)" / "Invalid value for Q (zero)"**: In the final Pi computation stage:
    ```c
    disk_int_get_mpz(mpz_P, &state->data.chudnovsky.P); // mpz_P becomes 0
    // ...
    if (mpz_sgn(mpz_P) == 0) {
        fprintf(stderr, "Error: Invalid value for P (zero)\n");
        mpz_set_ui(mpz_P, 1); // mpz_P is reset to 1 for fallback
    }
    ```
    This explains these errors and the "WARNING: Could not read proper P, Q, T values from disk."

**Secondary Problem: Incorrect Final Pi Formula & Potential Extreme Value**

Even if the primary bug was fixed and `mpz_P`, `mpz_Q`, `mpz_T` held the correct large integer values from the binary splitting, the formula used for the final Pi calculation is incorrect and would likely lead to an astronomically large (or small) number, potentially causing `mpfr_get_str` to fail or segfault.

Your binary splitting base case calculates (let's call them `P_k_bs`, `Q_k_bs`, `T_k_bs`):
*   `P_k_bs = k^3 * C^3 / 24`
*   `Q_k_bs = (6k-5)(2k-1)(6k-1)`
*   `T_k_bs = Q_k_bs * (A + B*k) * (-1)^k` (simplified)

After the binary splitting recursion and combination, `state.data.chudnovsky` (if correctly populated) would hold:
*   `mpz_P_val` (from `state.data.chudnovsky.P`): This is $\prod P_{k\_bs}$
*   `mpz_Q_val` (from `state.data.chudnovsky.Q`): This is $\prod Q_{k\_bs}$
*   `mpz_T_val` (from `state.data.chudnovsky.T`): This is the full BS sum term.

The standard Chudnovsky formula, when using these types of BS components (where $\prod P_{k\_bs}$ is akin to $Q(0,N)$ in reference implementations, and $T_{sum}$ is $T(0,N)$), is:
$\pi = \frac{(\prod P_{k\_bs}) \cdot C \sqrt{C}}{12 \cdot T_{sum}}$
So, it should be: $\pi = \frac{\text{mpz_P_val} \cdot C \sqrt{C}}{12 \cdot \text{mpz_T_val}}$

Your code calculates:
`Pi_calculated = (mpz_P_val * (C^3/24)_{\text{mpfr}} * \sqrt{C}_{\text{mpfr}}) / (A \cdot \text{mpz_P_val} + \text{mpz_Q_val})`

The term `mpz_P_val` *already* contains factors of $C^3/24$ from its definition $\prod (k^3 C^3/24)$. Multiplying it *again* by $(C^3/24)_{\text{mpfr}}$ would square these enormous factors, leading to an unimaginably large number. It's highly probable that `mpfr_get_str` would struggle with such an extreme value (huge exponent), potentially leading to a segfault due to internal overflows or memory issues when trying to format it as a string of `10000` digits.

The fallback calculation (P=1, Q=1, T=0) doesn't hit this extreme value issue, producing the `6.44e11` number. The segfault happens *after* this fallback value is printed by `mpfr_printf`, specifically at the `mpfr_get_str` call. This suggests that `mpfr_pi`, even with the fallback value, might be problematic for `mpfr_get_str` under some condition, or there's pre-existing memory corruption. However, the extreme value from the incorrect formula is a more direct suspect if the primary bug were fixed.

**The Segmentation Fault:**

The segfault occurs at:
`str_pi = mpfr_get_str(NULL, &exp, 10, state->digits + 2, mpfr_pi, MPFR_RNDN);`

Given that the primary bug causes `mpz_P, mpz_Q, mpz_T` to be effectively 0 (then reset to 1,1,0 for the fallback), `mpfr_pi` is calculated using these small fallback values. The `mpfr_printf` shows `6.44041687094.2369231155`. This is a finite, representable number.
Why would `mpfr_get_str` segfault on this?
*   **Stack Corruption:** Unlikely to be the direct cause here, but possible due to earlier misuse of memory, though the bugs identified are more about logic.
*   **Heap Corruption:** If any of the `disk_int` operations (especially those involving `malloc`/`free` for cache or `product` buffers in `disk_int_mul`) had errors, they could have corrupted the heap. `mpfr_get_str` allocates memory for `str_pi`. If the heap is corrupted, this allocation can fail and lead to a segfault. The `disk_int` operations are complex, and an error there (e.g. off-by-one in buffer sizes, use-after-free) could be a silent contributor.
*   **MPFR Internal State:** While less common for such a standard function, if `mpfr_pi` was somehow put into an invalid state that `mpfr_printf` tolerates but `mpfr_get_str` does not, this could happen. The precision `state->digits * 4` seems fine.

The most direct path to the segfault, however, is that the "Disk integer not properly initialized" errors are symptoms of a deeper issue in how `disk_int` objects are managed, particularly their `file_path` and associated mutexes/directories. While the specific sequence of `clear` then `set_mpz` might avoid immediate segfault on a destroyed mutex due to an early return, repeated incorrect handling of these structures could lead to general instability or heap corruption that manifests later.

**Recommended Fixes:**

1.  **Correct `disk_int` Combination Logic:**
    Modify the combination loop in `calculate_pi_chudnovsky` (multi-threaded part). Instead of clearing and attempting to reuse `combined_state.P, .Q, .T` in-place (which fails), you should:
    *   Calculate the new combined P, Q, T into *new, temporary* `disk_int` objects, each initialized with its own unique path.
    *   After the new P, Q, T are computed into these temporaries, convert them to `mpz_t`.
    *   Then, use `disk_int_set_mpz` to set the values of the *original* `combined_state.P, .Q, .T` (which were initialized once before the loop and remain valid targets for `disk_int_set_mpz`).
    *   Finally, `disk_int_clear` the temporary `disk_int` objects used for the intermediate products in that iteration.

    A sketch of the corrected loop for `i=1` onwards:
    ```c
    // Inside the loop for i = 1 to pool->num_threads - 1
    // ...
    // Create temporary disk_ints for the results of this iteration's combinations
    disk_int P_new_combined, Q_new_combined, T_new_combined;
    disk_int temp_T_prod1, temp_T_prod2; // For T = T1*Q2 + T2*P1

    // Create unique base paths for these temporaries for this iteration
    char temp_iter_base_path[MAX_PATH];
    // ... construct unique path like ./pi_calc/split/combined/iter_temp_i ...
    mkdir_recursive(temp_iter_base_path, 0755);

    char p_new_base[MAX_PATH], q_new_base[MAX_PATH], t_new_base[MAX_PATH];
    char t_prod1_base[MAX_PATH], t_prod2_base[MAX_PATH];

    // Init P_new_combined, Q_new_combined, T_new_combined
    safe_path_join(p_new_base, MAX_PATH, temp_iter_base_path, "P_new"); mkdir_recursive(p_new_base,0755); disk_int_init(&P_new_combined, p_new_base);
    safe_path_join(q_new_base, MAX_PATH, temp_iter_base_path, "Q_new"); mkdir_recursive(q_new_base,0755); disk_int_init(&Q_new_combined, q_new_base);
    safe_path_join(t_new_base, MAX_PATH, temp_iter_base_path, "T_new"); mkdir_recursive(t_new_base,0755); disk_int_init(&T_new_combined, t_new_base);

    // Init temporaries for T calculation
    safe_path_join(t_prod1_base, MAX_PATH, temp_iter_base_path, "T_prod1"); mkdir_recursive(t_prod1_base,0755); disk_int_init(&temp_T_prod1, t_prod1_base);
    safe_path_join(t_prod2_base, MAX_PATH, temp_iter_base_path, "T_prod2"); mkdir_recursive(t_prod2_base,0755); disk_int_init(&temp_T_prod2, t_prod2_base);

    // Perform calculations into these new disk_ints:
    // P_new = combined_state.P (from prev iter) * chunk_results[i].P
    disk_int_mul(&P_new_combined, &combined_state.P, &chunk_results[i].P);
    // Q_new = combined_state.Q (from prev iter) * chunk_results[i].Q
    disk_int_mul(&Q_new_combined, &combined_state.Q, &chunk_results[i].Q);
    // T_new = combined_state.T * chunk_results[i].Q + chunk_results[i].T * combined_state.P
    disk_int_mul(&temp_T_prod1, &combined_state.T, &chunk_results[i].Q);
    disk_int_mul(&temp_T_prod2, &chunk_results[i].T, &combined_state.P);
    disk_int_add(&T_new_combined, &temp_T_prod1, &temp_T_prod2);

    // Convert results from new disk_ints to mpz_t
    mpz_t mpz_P_val, mpz_Q_val, mpz_T_val;
    mpz_init(mpz_P_val); mpz_init(mpz_Q_val); mpz_init(mpz_T_val);
    disk_int_get_mpz(mpz_P_val, &P_new_combined);
    disk_int_get_mpz(mpz_Q_val, &Q_new_combined);
    disk_int_get_mpz(mpz_T_val, &T_new_combined);

    // Update the main combined_state.P, Q, T by setting their values.
    // Their file_paths and mutexes are still valid from the initial chudnovsky_state_init.
    disk_int_set_mpz(&combined_state.P, mpz_P_val);
    disk_int_set_mpz(&combined_state.Q, mpz_Q_val);
    disk_int_set_mpz(&combined_state.T, mpz_T_val);

    mpz_clear(mpz_P_val); mpz_clear(mpz_Q_val); mpz_clear(mpz_T_val);

    // Clean up the temporary disk_ints used in this iteration
    disk_int_clear(&P_new_combined);
    disk_int_clear(&Q_new_combined);
    disk_int_clear(&T_new_combined);
    disk_int_clear(&temp_T_prod1);
    disk_int_clear(&temp_T_prod2);
    // You would also remove the directories like p_new_base, temp_iter_base_path etc.
    // ...
    ```

2.  **Correct Final Pi Formula:**
    After fixing the combination logic, `mpz_P` (from `state.data.chudnovsky.P`) will be $\prod (k^3 C^3/24)$, and `mpz_T` (from `state.data.chudnovsky.T`) will be the BS sum term. The correct formula is:
    $\pi = \frac{\text{mpz_P} \cdot C \cdot \sqrt{C}}{12 \cdot \text{mpz_T}}$
    Implement this:
    ```c
    // mpz_P, mpz_T are correctly populated
    // mpfr_C is set to C_const

    mpfr_t mpfr_P_val, mpfr_T_val, mpfr_sqrt_C, const_12;
    mpfr_init2(mpfr_P_val, precision);
    mpfr_init2(mpfr_T_val, precision);
    mpfr_init2(mpfr_sqrt_C, precision);
    mpfr_init2(const_12, precision);

    mpfr_set_z(mpfr_P_val, mpz_P, MPFR_RNDN);
    mpfr_set_z(mpfr_T_val, mpz_T, MPFR_RNDN); // Ensure mpz_T is not zero!
    if (mpfr_sgn(mpfr_T_val) == 0) {
        fprintf(stderr, "Error: Final T value for Pi calculation is zero. Cannot divide.\n");
        // Handle error: perhaps set pi to an error indicator or a known bad value
        // For now, let's prevent division by zero by setting T_val to 1, this will give a wrong Pi
        mpfr_set_ui(mpfr_T_val, 1, MPFR_RNDN);
    }


    mpfr_sqrt(mpfr_sqrt_C, mpfr_C, MPFR_RNDN); // sqrt(C)
    mpfr_set_ui(const_12, 12, MPFR_RNDN);

    // Numerator: mpz_P * C * Sqrt(C)
    mpfr_mul(mpfr_pi, mpfr_P_val, mpfr_C, MPFR_RNDN);       // pi = P_val * C
    mpfr_mul(mpfr_pi, mpfr_pi, mpfr_sqrt_C, MPFR_RNDN);    // pi = P_val * C * Sqrt(C)

    // Denominator: 12 * mpz_T
    mpfr_mul(mpfr_temp, mpfr_T_val, const_12, MPFR_RNDN);  // temp = T_val * 12

    if (mpfr_sgn(mpfr_temp) == 0) { // Double check denominator
        fprintf(stderr, "Error: Denominator (12*T) is zero.\n");
        // Handle error
    } else {
        mpfr_div(mpfr_pi, mpfr_pi, mpfr_temp, MPFR_RNDN);      // pi = Numerator / Denominator
    }

    mpfr_clear(mpfr_P_val); mpfr_clear(mpfr_T_val);
    mpfr_clear(mpfr_sqrt_C); mpfr_clear(const_12);
    // mpfr_C, mpfr_temp are already declared and cleared later.
    ```
    This avoids the redundant multiplication by $C^3/24$ and uses the correct terms.

By addressing these two main areas, the "Disk integer" errors should disappear, P, Q, T should be correctly calculated, and the final Pi value will be computed with a valid formula, greatly reducing the chance of `mpfr_get_str` encountering an extreme or invalid number that could cause it to segfault. If a segfault *still* occurs in `mpfr_get_str` after these, then a more subtle heap corruption issue from `disk_int`'s memory management (cache, product buffers) would be the prime suspect.