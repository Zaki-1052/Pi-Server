# Implementation Plan for True Out-of-Core Arithmetic

## Problem Statement

The current implementation of `disk_int` operations loads entire numbers into memory before performing arithmetic, which negates the benefits of out-of-core computation for very large numbers. This document outlines a strategy to implement true out-of-core arithmetic operations.

## Current Limitations

Currently, in `disk_int_add` and `disk_int_mul`:
- Both operands are fully loaded into memory using `disk_int_get_mpz`
- The operation is performed entirely in memory with GMP
- The result is written back to disk

This approach works fine for numbers that can fit in memory, but fails for truly massive computations where memory consumption is a critical constraint.

## Proposed Approach

### 1. Chunked Representation

Modify the `disk_int` structure to support explicit chunking:

```c
struct disk_int {
    char file_path[MAX_PATH];   // Base path for the chunks
    size_t total_size_in_limbs; // Total size across all chunks
    int sign;                   // Sign of the number (1, 0, -1)
    size_t chunk_size;          // Size of each chunk in limbs
    size_t num_chunks;          // Number of chunks
    void* cache;                // Memory cache for active chunk
    size_t cache_chunk_idx;     // Index of cached chunk
    bool dirty;                 // Whether cache needs writing back to disk
    pthread_mutex_t lock;       // Mutex for thread safety
};
```

Each chunk will be stored in a separate file:
- `<file_path>_chunk_0.bin`
- `<file_path>_chunk_1.bin`
- etc.

### 2. Chunked Addition Algorithm

For adding two disk_ints (`c = a + b`):

1. Determine optimal chunk size based on available memory
2. Process chunks sequentially from least to most significant
3. Maintain a carry value between chunk operations

```
function disk_int_add(result, a, b):
    initialize carry = 0
    for i = 0 to max(a.num_chunks, b.num_chunks):
        load chunk_a = load_chunk(a, i) or 0 if beyond a's chunks
        load chunk_b = load_chunk(b, i) or 0 if beyond b's chunks
        
        // Perform addition on this chunk with carry from previous
        chunk_result = chunk_a + chunk_b + carry
        
        // Extract new carry for next chunk
        if chunk_result overflows chunk_size:
            carry = 1
            chunk_result = chunk_result - (2^(chunk_size*bits_per_limb))
        else:
            carry = 0
            
        // Save result chunk
        save_chunk(result, i, chunk_result)
    
    // Handle final carry if exists
    if carry > 0:
        save_chunk(result, i, carry)
```

### 3. Chunked Multiplication Algorithm

Implement a chunked multiplication algorithm based on the schoolbook method:

1. Split numbers into manageable chunks
2. Perform multiplications between all pairs of chunks
3. Add the results with appropriate shifts

For large multiplications, implement Karatsuba or Toom-Cook algorithms adapted for chunked representation:

```
function disk_int_mul(result, a, b):
    // Clear result first
    initialize all result chunks to 0
    
    // For each chunk in a
    for i = 0 to a.num_chunks - 1:
        chunk_a = load_chunk(a, i)
        
        // For each chunk in b
        for j = 0 to b.num_chunks - 1:
            chunk_b = load_chunk(b, j)
            
            // Multiply these chunks
            chunk_product = chunk_a * chunk_b
            
            // Add to appropriate position in result
            // This requires a chunked addition that spans multiple chunks
            add_product_to_result(result, chunk_product, i + j)
```

The `add_product_to_result` function needs to handle adding the product to potentially multiple chunks in the result, managing carries appropriately.

### 4. Memory Management Strategies

Implement intelligent caching strategies:
- LRU (Least Recently Used) cache for chunks
- Predictive loading for sequential operations
- Memory-aware chunk size determination based on system resources

```c
size_t determine_optimal_chunk_size(size_t available_memory, size_t total_size) {
    // Default to 1MB chunks or 10% of available memory, whichever is smaller
    size_t optimal_size = MIN(1024*1024, available_memory / 10);
    
    // Don't make chunks too small relative to the total size
    if (total_size < 100 * optimal_size) {
        optimal_size = MAX(total_size / 100, MIN_CHUNK_SIZE);
    }
    
    return optimal_size;
}
```

### 5. Performance Optimizations

1. **Parallel Processing**: Process independent chunk operations in parallel using the existing thread pool
2. **Adaptive Chunking**: Adjust chunk sizes based on operation type and available memory
3. **Operation Fusion**: Combine multiple operations to minimize I/O overhead
4. **Memory Mapping**: Use `mmap` for efficient chunk access
5. **SSE/AVX Instructions**: Leverage SIMD instructions for chunk operations where appropriate

### 6. Integration with Existing Code

1. Modify `binary_split_disk` to use the new chunked operations
2. Update `calculate_pi_chudnovsky` to properly utilize chunked representations
3. Ensure backward compatibility with existing code paths

## Implementation Phases

### Phase 1: Core Infrastructure
- Modify `disk_int` structure to support chunking
- Implement chunk loading/saving operations
- Add memory management utilities

### Phase 2: Basic Arithmetic Operations
- Implement chunked addition
- Implement chunked multiplication (basic algorithm)
- Add basic test cases

### Phase 3: Advanced Features
- Implement advanced multiplication algorithms (Karatsuba, etc.)
- Add parallel processing for chunk operations
- Implement adaptive chunking

### Phase 4: Integration and Optimization
- Integrate with binary splitting algorithm
- Performance testing and optimization
- Documentation

## Estimated Effort and Challenges

The implementation will require significant effort, particularly:

1. **Algorithm Design**: Adapting mathematical algorithms to out-of-core operation
2. **Memory Management**: Ensuring efficient use of available memory
3. **Correctness**: Maintaining mathematical precision across chunk boundaries
4. **Performance**: Balancing I/O operations with computation

## Conclusion

A true out-of-core implementation will enable the Pi Calculator to handle computations of significantly larger scale, potentially enabling calculations with trillions of digits on standard hardware.

This approach maintains the hybrid nature of the system - for smaller calculations, the existing in-memory implementation remains optimal, while the new chunked implementation will enable truly massive calculations that were previously impossible due to memory constraints.