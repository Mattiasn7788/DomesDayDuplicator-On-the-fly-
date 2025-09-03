#!/bin/bash

# Build script for DomesdayDuplicator using system FFmpeg libraries
# This ensures we don't accidentally use brew or other non-system dependencies

# Set PKG_CONFIG_PATH to prioritize system libraries
export PKG_CONFIG_PATH="/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig"

# Print which versions we're using
echo "=== Using System FFmpeg Libraries ==="
pkg-config --modversion libswresample libavutil
pkg-config --libs libswresample libavutil
echo "======================================"

# Clean previous build
rm -rf build

# Configure with cmake
echo "Configuring build..."
cmake -B build -S .

# Build
echo "Building..."
cmake --build build -j$(nproc)

# Verify system library linkage
echo "=== Verifying Library Linkage ==="
ldd build/DomesdayDuplicator/DomesdayDuplicator | grep -E "(swresample|avutil)"
echo "================================="

echo "Build complete! Executable: build/DomesdayDuplicator/DomesdayDuplicator"
