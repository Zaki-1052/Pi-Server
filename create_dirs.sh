#!/bin/bash
# Create all needed directories for pi calculator

ROOT="./pi_calc"
mkdir -p $ROOT

# Create base structure
mkdir -p $ROOT/current/{P,Q,T}
mkdir -p $ROOT/split

# Create chunk directories
for i in {0..7}; do
  mkdir -p $ROOT/split/chunk_$i/{P,Q,T}
done

# Create combined and temp directories
mkdir -p $ROOT/split/combined
mkdir -p $ROOT/split/left
mkdir -p $ROOT/split/right
mkdir -p $ROOT/split/temp{1,2}

# Fix permissions
chmod -R 755 $ROOT
