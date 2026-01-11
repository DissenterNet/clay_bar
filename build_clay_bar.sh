#!/bin/bash
set -e

echo "Building clay_bar..."

if [ ! -f "clay_bar.c" ]; then
    echo "Error: clay_bar.c not found in current directory"
    exit 1
fi

# Compile
gcc -O2 -Wall -Wextra -std=gnu99 \
    -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
    -o clay_bar \
    clay_bar.c \
    -lX11 -lXext -lcairo -lm

echo "Build successful!"

# Install to /usr/bin (requires sudo)
sudo rm /usr/bin/clay_bar
sudo cp clay_bar /usr/bin/
sudo chmod 755 /usr/bin/clay_bar
sudo rm clay_bar
echo "Installation complete."
