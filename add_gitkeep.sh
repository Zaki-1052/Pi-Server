#!/bin/bash
# Script to add .gitkeep files to important directories

# Base directories
BASE_DIRS=(
  "pi_calc/current/P"
  "pi_calc/current/Q"
  "pi_calc/current/T"
  "pi_calc/split/combined/P"
  "pi_calc/split/combined/Q"
  "pi_calc/split/combined/T"
  "pi_calc/split/left/P"
  "pi_calc/split/left/Q"
  "pi_calc/split/left/T"
  "pi_calc/split/right/P"
  "pi_calc/split/right/Q"
  "pi_calc/split/right/T"
  "pi_calc/split/temp1"
  "pi_calc/split/temp2"
)

# Add .gitkeep to base directories
for dir in "${BASE_DIRS[@]}"; do
  touch "$dir/.gitkeep"
  echo "Added .gitkeep to $dir"
done

# Add .gitkeep to chunk directories
for i in {0..7}; do
  # Main chunk directories
  CHUNK_DIRS=(
    "pi_calc/split/chunk_$i/P"
    "pi_calc/split/chunk_$i/Q"
    "pi_calc/split/chunk_$i/T"
    "pi_calc/split/chunk_$i/left/P"
    "pi_calc/split/chunk_$i/left/Q"
    "pi_calc/split/chunk_$i/left/T"
    "pi_calc/split/chunk_$i/right/P"
    "pi_calc/split/chunk_$i/right/Q"
    "pi_calc/split/chunk_$i/right/T"
    "pi_calc/split/chunk_$i/temp1"
    "pi_calc/split/chunk_$i/temp2"
  )
  
  for dir in "${CHUNK_DIRS[@]}"; do
    touch "$dir/.gitkeep"
    echo "Added .gitkeep to $dir"
  done
done

echo "Finished adding .gitkeep files"