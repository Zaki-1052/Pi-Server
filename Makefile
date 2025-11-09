# Makefile for Pi Calculator Server (Modular Version)
#
# This Makefile builds the refactored modular version of the Pi Calculator.
# All source files must be present for successful compilation.

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O3 -g
LIBS = -lm -lgmp -lmpfr -ljson-c -lpthread

# Target executable
TARGET = pi-server

# Source files (in dependency order)
SOURCES = utils.c \
          logging.c \
          config.c \
          disk_int.c \
          algorithms.c \
          calc_state.c \
          thread_pool.c \
          http_pool.c \
          job_manager.c \
          http_server.c \
          signal_handlers.c \
          pi_calculator.c

# Object files (generated from sources)
OBJECTS = $(SOURCES:.c=.o)

# Header files (for dependency tracking)
HEADERS = common.h \
          utils.h \
          logging.h \
          config.h \
          disk_int.h \
          algorithms.h \
          calc_state.h \
          thread_pool.h \
          http_pool.h \
          job_manager.h \
          http_server.h \
          signal_handlers.h

# Default target
all: $(TARGET)

# Link object files to create executable
$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)
	@echo "Build complete: $(TARGET)"

# Compile source files to object files
%.o: %.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(OBJECTS) $(TARGET)
	@echo "Clean complete"

# Clean and rebuild
rebuild: clean all

# Install target (optional)
install: $(TARGET)
	@echo "Installing $(TARGET) to /usr/local/bin..."
	sudo cp $(TARGET) /usr/local/bin/
	@echo "Installation complete"

# Check if all required source files exist
check:
	@echo "Checking for required source files..."
	@for file in $(SOURCES); do \
		if [ ! -f $$file ]; then \
			echo "ERROR: Missing source file: $$file"; \
			exit 1; \
		fi; \
	done
	@echo "Checking for required header files..."
	@for file in $(HEADERS); do \
		if [ ! -f $$file ]; then \
			echo "ERROR: Missing header file: $$file"; \
			exit 1; \
		fi; \
	done
	@echo "All required files are present"

# Build with original monolithic version for comparison
original:
	@echo "Building original monolithic version..."
	$(CC) $(CFLAGS) -o pi-server-original pi_calculator.c.backup $(LIBS)
	@echo "Original version built as: pi-server-original"

# Help target
help:
	@echo "Pi Calculator Server - Makefile Help"
	@echo ""
	@echo "Available targets:"
	@echo "  all       - Build the pi-server executable (default)"
	@echo "  clean     - Remove all build artifacts"
	@echo "  rebuild   - Clean and rebuild from scratch"
	@echo "  check     - Verify all required source/header files exist"
	@echo "  install   - Install pi-server to /usr/local/bin (requires sudo)"
	@echo "  original  - Build the original monolithic version for comparison"
	@echo "  help      - Display this help message"
	@echo ""
	@echo "Dependencies:"
	@echo "  - GMP library (libgmp-dev)"
	@echo "  - MPFR library (libmpfr-dev)"
	@echo "  - JSON-C library (libjson-c-dev)"
	@echo "  - pthread (usually included with gcc)"
	@echo ""
	@echo "Usage examples:"
	@echo "  make              # Build the project"
	@echo "  make clean        # Clean build artifacts"
	@echo "  make rebuild      # Clean and rebuild"
	@echo "  make check        # Verify all files are present"

.PHONY: all clean rebuild install check original help
