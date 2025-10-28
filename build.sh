#!/bin/bash

# Simple build script for CSI Camera H.264 Encoder Tool

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Configuration
BUILD_DIR="build"
TARGET_NAME="csi_camera_h264_encode"

# Function to clean build directory
clean_build() {
    print_status "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    print_success "Build directory cleaned"
}

# Function to create build directory
create_build_dir() {
    print_status "Creating build directory..."
    mkdir -p "$BUILD_DIR"
    print_success "Build directory created"
}

# Function to configure with CMake
configure_cmake() {
    print_status "Configuring with CMake..."
    cd "$BUILD_DIR"
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=14 \
        -DCMAKE_CXX_STANDARD_REQUIRED=ON
    cd ..
    print_success "CMake configuration completed"
}

# Function to build the project
build_project() {
    print_status "Building project..."
    cd "$BUILD_DIR"
    make -j$(nproc)
    cd ..
    print_success "Build completed"
}

# Function to show help
show_help() {
    echo "CSI Camera H.264 Encoder Tool Build Script"
    echo ""
    echo "Usage: $0 [COMMAND]"
    echo ""
    echo "Commands:"
    echo "  build                    Build the project (default)"
    echo "  clean                    Clean build directory"
    echo "  rebuild                  Clean and build"
    echo "  run [ARGS]               Run the built executable"
    echo "  help                     Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 build                # Build the project"
    echo "  $0 clean                # Clean build directory"
    echo "  $0 rebuild              # Clean and rebuild"
    echo "  $0 run -d 10 -r 1920x1080"
    echo ""
}

# Function to run the executable
run_executable() {
    local args="$*"
    if [ -f "$BUILD_DIR/$TARGET_NAME" ]; then
        print_status "Running $TARGET_NAME with args: $args"
        cd "$BUILD_DIR"
        ./$TARGET_NAME $args
        cd ..
    else
        print_error "Executable not found. Please build first with: $0 build"
        exit 1
    fi
}

# Main script logic
main() {
    case "${1:-build}" in
        "build")
            create_build_dir
            configure_cmake
            build_project
            print_success "Build completed successfully!"
            print_status "Executable: $BUILD_DIR/$TARGET_NAME"
            ;;
        "clean")
            clean_build
            ;;
        "rebuild")
            clean_build
            create_build_dir
            configure_cmake
            build_project
            print_success "Rebuild completed successfully!"
            ;;
        "run")
            shift
            run_executable "$@"
            ;;
        "help"|"-h"|"--help")
            show_help
            ;;
        *)
            print_error "Unknown command: $1"
            show_help
            exit 1
            ;;
    esac
}

# Run main function with all arguments
main "$@"
