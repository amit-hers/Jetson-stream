#!/bin/bash
# Build in Release mode (optimized, no debug symbols)

echo "Building in RELEASE mode..."
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make camera_server_simple
echo ""
echo "Release build complete!"
