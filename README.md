# Pi Calculator - Hybrid Pi Computation Engine

A high-performance, memory-efficient calculator for computing π (pi) to billions of digits on resource-constrained systems.

## Overview

This project implements a hybrid π calculator that combines multiple algorithms and computational techniques to calculate π to arbitrary precision, even on machines with limited memory. It features both a command-line interface and an HTTP API server, making it flexible for various use cases from local calculations to service-oriented deployments.

## Features

- **Multiple Algorithms**:
  - Gauss-Legendre (GL) for small to medium calculations (fast for <100,000 digits)
  - Chudnovsky (CH) for large calculations (highly efficient for millions/billions of digits)

- **Memory Optimization**:
  - Out-of-core computation for handling calculations larger than available RAM
  - Disk-based integer implementation for working with extremely large numbers
  - Automatic mode selection based on calculation size and available memory

- **Parallel Processing**:
  - Multi-threaded implementation using thread pools
  - Chunked computation with binary splitting for the Chudnovsky algorithm

- **Dual Interface**:
  - Command-line mode for direct calculations
  - HTTP server with a RESTful API for remote access

- **Asynchronous Processing**:
  - Job management system for long-running calculations
  - Progress tracking and status reporting
  - Result persistence and retrieval

- **Cross-Platform Compatibility**:
  - Optimized for ARM64 architecture
  - Compatible with x86_64 systems
  - Tested on Ubuntu Linux

## Installation

### Prerequisites

The following libraries are required:

```bash
sudo apt-get install libgmp-dev libmpfr-dev libjson-c-dev
```

### Building from Source

```bash
git clone https://github.com/yourusername/pi-calculator.git
cd pi-calculator
gcc -o pi_hybrid pi_hybrid.c -lgmp -lmpfr -ljson-c -lm -pthread -O3
```

## Usage

### Command-Line Mode

Calculate π to a specific number of digits:

```bash
# Calculate 1 million digits using default algorithm
./pi_hybrid 1000000

# Specify algorithm (GL = Gauss-Legendre, CH = Chudnovsky)
./pi_hybrid -a CH 1000000

# Specify thread count and memory limit
./pi_hybrid -t 4 -m 8 1000000
```

### Server Mode

Start the HTTP server for API access:

```bash
# Start with default settings (from config.json)
./pi_hybrid -s

# Specify custom port and IP
./pi_hybrid -s -p 8080 -i 0.0.0.0

# Use a specific configuration file
./pi_hybrid -c custom_config.json -s
```

## HTTP API Endpoints

### Calculate Pi

```
GET /pi?digits=1000000&algo=CH&mode=async
```

Parameters:
- `digits`: Number of digits to calculate (required)
- `algo`: Algorithm to use - CH (Chudnovsky) or GL (Gauss-Legendre) (optional, default is CH)
- `mode`: sync or async (optional, default is sync)
- `out_of_core`: true or false (optional, default is auto)
- `checkpoint`: true or false (optional, default is true)

For synchronous requests, returns the calculation result directly. For asynchronous requests, returns a job ID.

### Check Job Status

```
GET /pi/status?id=job_uuid
```

Returns the current status of an asynchronous calculation job.

### Get Job Result

```
GET /pi/result?id=job_uuid
```

Returns the calculated digits of π for a completed job.

## Configuration

The application can be configured via a JSON file (`config.json` by default):

```json
{
  "ip_address": "0.0.0.0",
  "port": 8081,
  "max_http_threads": 4,
  "max_calc_threads": 4,
  "max_digits": 5000000000,
  "memory_limit": 20,
  "default_algorithm": "CH",
  "gl_iterations": 10,
  "gl_precision_bits": 128,
  "logging": {
    "level": "debug",
    "output": "piserver.log"
  },
  "work_dir": "./pi_calc",
  "checkpointing_enabled": true,
  "checkpoint_interval": 600
}
```

### Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `ip_address` | IP address for HTTP server | "0.0.0.0" |
| `port` | Port for HTTP server | 8081 |
| `max_http_threads` | Maximum HTTP worker threads | 4 |
| `max_calc_threads` | Maximum calculation threads | 4 |
| `max_digits` | Maximum digits allowed | 5000000000 |
| `memory_limit` | Memory limit in GB | 20 |
| `default_algorithm` | Default algorithm (CH or GL) | "CH" |
| `gl_iterations` | Iterations for Gauss-Legendre | 10 |
| `gl_precision_bits` | Extra precision bits for GL | 128 |
| `logging.level` | Logging level (debug, info, error) | "debug" |
| `logging.output` | Log output (file path or "console") | "piserver.log" |
| `work_dir` | Working directory for calculations | "./pi_calc" |
| `checkpointing_enabled` | Enable/disable checkpointing | true |
| `checkpoint_interval` | Checkpoint interval in seconds | 600 |

## Algorithm Details

### Gauss-Legendre

The Gauss-Legendre algorithm is based on the arithmetic-geometric mean and converges quadratically, making it very efficient for small to medium calculations. It's implemented in-memory using the MPFR library.

Formula:
```
a₀ = 1
b₀ = 1/√2
t₀ = 1/4
p₀ = 1

For each iteration:
  aₙ₊₁ = (aₙ + bₙ)/2
  bₙ₊₁ = √(aₙ * bₙ)
  tₙ₊₁ = tₙ - pₙ(aₙ - aₙ₊₁)²
  pₙ₊₁ = 2pₙ

Final π approximation: π ≈ (aₙ + bₙ)²/(4tₙ)
```

### Chudnovsky

The Chudnovsky algorithm is one of the most efficient algorithms for calculating π to extreme precision. It uses a rapidly converging series based on a Ramanujan-type formula. The implementation uses binary splitting with out-of-core computation for memory efficiency.

Formula:
```
π = (Q * (C/24)^(3/2)) / (T * 12)

Where Q, T are computed using binary splitting on the series:
∑ ((-1)^k * (6k)! * (A + Bk)) / ((3k)! * (k!)^3 * C^(3k))

With constants:
A = 13591409
B = 545140134
C = 640320
```

## Performance Considerations

### Memory Usage

- **Gauss-Legendre**: Memory usage is approximately 4 * digits bytes
- **Chudnovsky**: Memory usage depends on implementation mode:
  - In-memory: Approximately 8 * digits bytes
  - Out-of-core: Can run with limited RAM (controlled by `memory_limit`)

### Disk Space

For large calculations using out-of-core mode:
- Working files: ~30-50 times the final result size
- Final result: ~1.05 bytes per digit

### Calculation Speed

Performance scales approximately with:
- Chudnovsky: O(n * log(n)^3)
- Gauss-Legendre: O(n * log(n)^2)

Where n is the number of digits.

## Code Structure

The project is organized into several logical components:

- **Disk-based Integer Implementation**: Custom implementation for handling arbitrary-precision integers using disk storage
- **Algorithm Implementations**: Separate implementations for Gauss-Legendre and Chudnovsky algorithms
- **Thread Pools**: For both HTTP request handling and calculation tasks
- **Job Management**: Asynchronous job tracking and status reporting
- **HTTP Server**: RESTful API for remote access
- **Configuration**: JSON-based configuration with command-line overrides
- **Logging**: Configurable logging system

## Examples

### Calculating 1 Million Digits via CLI

```bash
./pi_hybrid -a CH -t 8 1000000
```

### Calculating 1 Billion Digits via API

```bash
curl "http://localhost:8081/pi?digits=1000000000&algo=CH&mode=async"
```

Response:
```json
{
  "status": "success",
  "message": "Calculation queued",
  "job_id": "550e8400-e29b-41d4-a716-446655440000",
  "algorithm": "CH",
  "digits": 1000000000
}
```

### Checking Job Status

```bash
curl "http://localhost:8081/pi/status?id=550e8400-e29b-41d4-a716-446655440000"
```

Response:
```json
{
  "job_id": "550e8400-e29b-41d4-a716-446655440000",
  "status": "running",
  "algorithm": "CH",
  "digits": 1000000000,
  "progress": 0.45,
  "creation_time": 1684251478,
  "start_time": 1684251480
}
```

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Acknowledgments

- The implementation is based on well-known algorithms by Gauss-Legendre and the Chudnovsky brothers
- Makes use of the GMP and MPFR libraries for arbitrary-precision arithmetic
- Inspired by other π calculators including y-cruncher

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## Disclaimer

For very large calculations (billions of digits), this software requires significant computational resources and time. Be prepared for long run times and ensure your system has adequate cooling for extended high CPU utilization.

