#!/bin/bash
# Build with debug symbols

echo "Building with DEBUG symbols..."
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make camera_server_simple
echo ""
echo "Debug build complete! Run with gdb for debugging:"
echo "  gdb --args ./camera_server_simple -i 1 -r 1280x720 -f 30"
