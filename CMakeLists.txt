cmake_minimum_required(VERSION 3.10)
project(CSI_Camera_H264_Encode_Tool)

# Set C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set build type - can be overridden with: cmake -DCMAKE_BUILD_TYPE=Debug ..
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Debug flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
    message(STATUS "Building with DEBUG symbols")
else()
    message(STATUS "Building in RELEASE mode (optimized)")
endif()

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/Argus
    ${CMAKE_SOURCE_DIR}/include/EGL
    ${CMAKE_SOURCE_DIR}/include/EGLStream
    ${CMAKE_SOURCE_DIR}/include/nvmmapi
    /usr/local/cuda/include
    /usr/include/aarch64-linux-gnu
    /usr/include/libdrm
)

# Link directories
link_directories(
    /usr/local/cuda/lib64
    /usr/lib/aarch64-linux-gnu
    /usr/lib/aarch64-linux-gnu/nvidia
    /usr/lib/aarch64-linux-gnu/tegra
)

# Source files for the CSI camera application
set(CSI_CAMERA_SOURCES
    src/main.cpp
    src/rtsp_server.cpp
    utils/Thread.cpp
    utils/NativeBuffer.cpp
    utils/NvNativeBuffer.cpp
    classes/NvVideoEncoder.cpp
    classes/NvBuffer.cpp
    classes/NvElement.cpp
    classes/NvElementProfiler.cpp
    classes/NvV4l2Element.cpp
    classes/NvV4l2ElementPlane.cpp
    classes/NvLogging.cpp
    classes/NvApplicationProfiler.cpp
    classes/NvBufSurface.cpp
)

# Source files for the enhanced camera client
set(ENHANCED_CLIENT_SOURCES
    src/enhanced_camera_client.cpp
)

# Source files for the simplified camera server (now with real camera capture)
set(SIMPLE_SERVER_SOURCES
    src/enhanced_camera_server_simple.cpp
    classes/NvBuffer.cpp
    classes/NvBufSurface.cpp
    classes/NvElement.cpp
    classes/NvLogging.cpp
    classes/NvElementProfiler.cpp
    classes/NvApplicationProfiler.cpp
)

# Create the main CSI camera executable
add_executable(csi_camera_h264_encode ${CSI_CAMERA_SOURCES})

# Create the enhanced camera client executable
add_executable(camera_client ${ENHANCED_CLIENT_SOURCES})

# Create the simplified camera server executable (with NVIDIA libraries for real camera capture)
add_executable(camera_server_simple ${SIMPLE_SERVER_SOURCES})

# Link libraries for CSI camera application
target_link_libraries(csi_camera_h264_encode
    pthread
    nvv4l2
    EGL
    GLESv2
    X11
    nvbufsurface
    nvbufsurftransform
    nvosd
    nvargus_socketclient
    nvvideo
    nvcamerautils
    nvrm_mem
    nvrm_sync
)

# Link libraries for enhanced camera client (minimal dependencies)
target_link_libraries(camera_client
    pthread
)

# Link libraries for simplified camera server (now with NVIDIA libraries for real camera capture)
target_link_libraries(camera_server_simple
    pthread
    nvv4l2
    EGL
    GLESv2
    X11
    nvbufsurface
    nvbufsurftransform
    nvosd
    nvargus_socketclient
    nvvideo
    nvcamerautils
    nvrm_mem
    nvrm_sync
)

# Compiler flags for CSI camera application
target_compile_options(csi_camera_h264_encode PRIVATE
    -std=c++14
    -Wall
    -Wextra
)

# Compiler flags for enhanced camera client
target_compile_options(camera_client PRIVATE
    -std=c++14
    -Wall
    -Wextra
)

# Compiler flags for simplified camera server
target_compile_options(camera_server_simple PRIVATE
    -std=c++14
    -Wall
    -Wextra
)

# Print configuration summary
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "CUDA path: ${CUDA_PATH}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")

# Install targets
install(TARGETS csi_camera_h264_encode DESTINATION bin)
install(TARGETS camera_client DESTINATION bin)
install(TARGETS camera_server_simple DESTINATION bin)
