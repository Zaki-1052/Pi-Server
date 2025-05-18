# Pi Calculator Server

A high-performance Pi digit calculation service with HTTP API and CLI support, optimized for ARM64 architecture.

## Overview

Pi Calculator Server is a hybrid solution for computing Pi digits using mathematically rigorous algorithms. It supports both direct command-line calculation and a RESTful API server mode, making it versatile for various computational needs from personal research to integration with other applications.

The implementation features memory-efficient computation techniques, allowing calculations of millions of digits of Pi even on resource-constrained systems through an innovative out-of-core computation approach.

## Features

- **Multiple Algorithms**:
  - Gauss-Legendre algorithm (faster for smaller calculations)
  - Chudnovsky algorithm (more efficient for large calculations)

- **Computation Modes**:
  - In-memory calculation for speed
  - Out-of-core calculation for handling extremely large digit counts
  - Automatic mode selection based on calculation size and available resources

- **Server Capabilities**:
  - RESTful HTTP API for remote calculation requests
  - Synchronous or asynchronous calculation options
  - Job queuing and status tracking
  - Multi-threaded request handling

- **Performance Optimizations**:
  - Thread pools for parallel processing
  - ARM64 specific optimizations
  - Memory usage optimization through disk-based arithmetic
  - Chunked binary splitting for large calculations

- **Additional Features**:
  - Checkpointing for long-running calculations
  - Comprehensive logging
  - JSON-based configuration
  - Progress tracking

## System Requirements

- Linux-based operating system (tested on Ubuntu)
- GCC compiler with C11 support
- At least 2GB RAM (more recommended for larger calculations)
- Sufficient disk space for out-of-core calculations (10x the memory required for calculation)
- Required libraries:
  - GMP (GNU Multiple Precision Arithmetic Library)
  - MPFR (Multiple Precision Floating-Point Reliable Library)
  - json-c (JSON manipulation library)
  - POSIX threads

## Installation

### Prerequisites

Install the required libraries:

```bash
# For Debian/Ubuntu
sudo apt-get update
sudo apt-get install libgmp-dev libmpfr-dev libjson-c-dev

# For macOS (using Homebrew)
brew install gmp mpfr json-c
```

### Building from Source

```bash
# Clone or download the source code
git clone https://github.com/yourusername/pi-calculator-server.git
cd pi-calculator-server

# Compile the code
gcc -o pi_calculator pi_calculator.c -lm -lgmp -lmpfr -ljson-c -lpthread -O3

# Create default working directory
mkdir -p ./pi_calc
```

## Configuration

### Configuration File

The server can be configured using a JSON configuration file:

```json
{
  "ip_address": "127.0.0.1",
  "port": 8080,
  "max_http_threads": 16,
  "max_calc_threads": 4,
  "max_digits": 1000000,
  "memory_limit": 20,
  "default_algorithm": "GL",
  "gl_iterations": 10,
  "gl_precision_bits": 128,
  "logging": {
    "level": "info",
    "output": "console"
  },
  "work_dir": "./pi_calc",
  "checkpointing_enabled": true,
  "checkpoint_interval": 600
}
```

### Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `ip_address` | IP address to bind the server | `127.0.0.1` |
| `port` | Port for the HTTP server | `8080` |
| `max_http_threads` | Maximum HTTP worker threads | `16` |
| `max_calc_threads` | Maximum calculation threads | `4` |
| `max_digits` | Maximum allowed digits for calculation | `1000000` |
| `memory_limit` | Memory limit in GB | `20` |
| `default_algorithm` | Default algorithm (`GL` or `CH`) | `GL` |
| `gl_iterations` | Iterations for Gauss-Legendre | `10` |
| `gl_precision_bits` | Extra precision bits for calculations | `128` |
| `logging.level` | Logging level (debug, info, error) | `info` |
| `logging.output` | Log output (console or file path) | `console` |
| `work_dir` | Working directory for calculations | `./pi_calc` |
| `checkpointing_enabled` | Enable calculation checkpoints | `true` |
| `checkpoint_interval` | Seconds between checkpoints | `600` |

## Usage

### Command Line Mode

Calculate Pi digits directly from the command line:

```bash
# Calculate 1000 digits of Pi using default algorithm
./pi_calculator 1000

# Calculate 10000 digits using Chudnovsky algorithm
./pi_calculator -a CH 10000

# Use 8 calculation threads with 30GB memory limit
./pi_calculator -t 8 -m 30 50000

# Specify working directory
./pi_calculator -w /tmp/pi_work 1000
```

### Server Mode

Run as an HTTP server:

```bash
# Start server with default settings
./pi_calculator -s

# Start server on specific IP and port
./pi_calculator -s -i 0.0.0.0 -p 9000

# Start server with configuration file
./pi_calculator -s -c config.json
```

### Command Line Options

| Option | Description |
|--------|-------------|
| `-a <algo>` | Algorithm (GL: Gauss-Legendre, CH: Chudnovsky) |
| `-d <digits>` | Number of digits to calculate |
| `-t <threads>` | Number of calculation threads |
| `-m <memory>` | Memory limit in GB |
| `-w <dir>` | Working directory |
| `-p <port>` | Server port |
| `-i <ip>` | Server IP address |
| `-c <file>` | Configuration file |
| `-s` | Run in server mode |

## API Documentation

The HTTP API provides endpoints for Pi calculation and job management.

### Calculate Pi (Synchronous)

```
GE
- `mode`: Calculation mode (sync or async)

Response:
```json
{
  "algorithm": "GL",
  "digits": 1000,
  "out_of_core": false,
  "time_taken": 153.42,
  "timestamp": "2023-04-01T12:34:56Z",
  "file_path": "results/pi_1000.txt",
  "status": "success",
  "message": "Calculation completed"
}
```

### Calculate Pi (Asynchronous)

```
GET /pi?algo=CH&digits=10000&mode=async
```

Response:
```json
{
  "status": "success",
  "message": "Calculation queued",
  "job_id": "550e8400-e29b-41d4-a716-446655440000",
  "algorithm": "CH",
  "digits": 10000
}
```

### Check Job Status

```
GET /pi/status?id=550e8400-e29b-41d4-a716-446655440000
```

Response:
```json
{
  "job_id": "550e8400-e29b-41d4-a716-446655440000",
  "status": "running",
  "algorithm": "CH",
  "digits": 10000,
  "progress": 0.75,
  "creation_time": 1680358496,
  "start_time": 1680358497
}
```

Possible status values: `queued`, `running`, `completed`, `failed`, `canceled`

### Get Calculation Result

```
GET /pi/result?id=550e8400-e29b-41d4-a716-446655440000
```

Response: Plain text file with Pi digits

## Performance Considerations

### Algorithm Selection

- **Gauss-Legendre (GL)**: Efficient for calculations up to ~100,000 digits
- **Chudnovsky (CH)**: More efficient for larger calculations

### Memory Usage

The program automatically selects between in-memory and out-of-core computation:

- **In-memory**: Faster but requires more RAM
- **Out-of-core**: Uses disk storage for calculations that would exceed available RAM

Approximate memory usage:
- Gauss-Legendre: ~4x digits in bytes
- Chudnovsky: ~0.6x digits in bytes (in-memory mode)

### Storage Requirements

For out-of-core calculations, ensure sufficient disk space:
- At least 10x the digits count in bytes
- Fast storage (SSD preferred) for better performance

## Algorithms Explained

### Gauss-Legendre Algorithm

An iterative algorithm based on arithmetic-geometric mean:

1. Initialize: a₀ = 1, b₀ = 1/√2, t₀ = 1/4, p₀ = 1
2. For each iteration:
   - aₙ₊₁ = (aₙ + bₙ)/2
   - bₙ₊₁ = √(aₙ·bₙ)
   - tₙ₊₁ = tₙ - pₙ(aₙ - aₙ₊₁)²
   - pₙ₊₁ = 2·pₙ
3. Calculate Pi: π ≈ (aₙ + bₙ)²/(4·tₙ)

### Chudnovsky Algorithm

A binary splitting implementation of the Chudnovsky series:

π = (426880√10005)∑[k=0→∞]((-1)^k(6k)!(545140134k+13591409))/((3k)!(k!)^3(640320)^(3k+3/2))

The binary splitting optimization divides large calculations into manageable chunks that can be processed in parallel.

## Troubleshooting

### Common Issues

1. **"Failed to create socket" error**
   - Another process may be using the specified port
   - Try changing the port using the `-p` option

2. **Out of memory errors**
   - Reduce the number of digits
   - Increase the memory limit with `-m` option
   - Ensure out-of-core mode is enabled for large calculations

3. **Slow calculation performance**
   - Increase calculation threads with `-t` option
   - Use Chudnovsky algorithm for large calculations
   - Use SSD storage for out-of-core calculations
   - Check for other CPU-intensive processes

### Debug Logging

Enable debug logging by modifying the configuration:

```json
"logging": {
  "level": "debug",
  "output": "pi_calculator.log"
}
```

## Limitations

- Maximum digit support is based on available system resources
- No HTTPS support for the API
- Limited error recovery for interrupted calculations
- Temporary files may not be cleaned up if the process is killed
- ARM64 optimization is primarily through standard GMP/MPFR libraries

## License and Attribution

This software implements:
- Gauss-Legendre algorithm by Carl Friedrich Gauss and Adrien-Marie Legendre
- Chudnovsky algorithm by David and Gregory Chudnovsky

The implementation uses:
- GMP for arbitrary precision arithmetic
- MPFR for floating-point calculations
- json-c for JSON parsing

---
