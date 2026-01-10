#!/bin/bash

# Build script for clay_bar
# This script handles the compilation with proper flags and libraries

set -e

echo "Building clay_bar..."

# Check if required files exist
if [ ! -f "clay_bar.c" ]; then
    echo "Error: clay_bar.c not found in current directory"
    exit 1
fi

# Compile with appropriate flags
gcc -O2 -Wall -Wextra -std=c99 \
    -o clay_bar \
    clay_bar.c \
    -lX11 -lcairo -lm

if [ $? -eq 0 ]; then
    echo "Build successful! Executable: clay_bar"
    echo "Usage: ./clay_bar"
else
    echo "Build failed!"
    exit 1
fi
