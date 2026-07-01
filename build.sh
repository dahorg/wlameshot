#!/bin/bash

# Build script for Wlameshot

set -e

BUILD_DIR="build"

echo "Building Wlameshot..."

if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

cmake .. $@

make -j$(nproc)

echo "Build complete!"
echo "Run: sudo make install (optional)"
