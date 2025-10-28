/*
 * CSI Camera H.264 Encoder
 * 
 * This sample demonstrates how to capture video from CSI camera using Argus SDK
 * and encode it to H.264 using NVIDIA's hardware encoder without any external
 * libraries like GStreamer.
 * 
 * Build:
 * make
 * 
 * Usage:
 * ./csi_camera_h264_encode -o output.h264 -d 10 -r 1920x1080 -f 30
 */

// Include system headers first to avoid conflicts
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <getopt.h>
#include <queue>
#include <vector>
#include <map>
#include <sys/socket.h>
#include <chrono>
#include <thread>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "rtsp_server.h"

// Define guards to prevent V4L2 conflicts
#define V4L2_H264_SPS_H
#define V4L2_H264_PPS_H
#define V4L2_H264_SCALING_MATRIX_H
#define V4L2_H264_WEIGHT_FACTORS_H
#define V4L2_H264_DPB_ENTRY_H

// Include NVIDIA headers with conflict resolution
#include <Argus/Argus.h>
#include <NvVideoEncoder.h>
#include <NvApplicationProfiler.h>
#include "NvBufSurface.h"

// Include utility headers
#include "Error.h"
#include "Thread.h"
#include "nvmmapi/NvNativeBuffer.h"

using namespace Argus;

/* Configuration */
static const int    MAX_ENCODER_FRAMES = 5;
static const int    DEFAULT_FPS        = 30;
static const int    NUM_BUFFERS        = 10;

/* Configurable parameters */
static int          CAPTURE_TIME = 10; // In seconds
static uint32_t     CAMERA_INDEX = 0;
static Size2D<uint32_t> STREAM_SIZE (1920, 1080);
static std::string  OUTPUT_FILENAME ("");  // Empty means no file output by default
static uint32_t     ENCODER_PIXFMT = V4L2_PIX_FMT_H264;
static bool         DO_STAT = false;
static bool         VERBOSE_ENABLE = false;
static bool         HEADLESS_MODE = false;
static bool         LIST_CAMERAS = false;
static int          SENSOR_MODE_INDEX = -1;  // -1 means auto-select based on resolution
static int          TARGET_FPS = -1;  // -1 means use sensor mode default FPS
static bool         RAW_FRAME_MODE = false;  // Skip encoding, just capture raw frames

/* Debug statistics */
// Debug statistics (legacy - kept for compatibility)
static uint64_t     DEBUG_PACKET_COUNT = 0;
static uint64_t     DEBUG_BYTES_SENT = 0;

/* UDP Streaming parameters */
static bool         UDP_STREAMING = false;
static std::string  UDP_HOST ("192.254.254.99");
static int          UDP_PORT = 5000;
static int          UDP_SOCKET = -1;
static struct sockaddr_in UDP_ADDR;

/* RTP H.264 payload parameters */
static uint16_t     rtp_sequence_number = 100;  // Start with a higher number to avoid confusion
// Removed duplicate - using global rtp_timestamp below
static uint32_t     rtp_ssrc = 0x12345678;
static bool         sps_pps_sent = false;
static char*        sps_data = NULL;
static size_t       sps_size = 0;

// Store multiple PPS with their IDs
static std::map<uint8_t, PPSData> pps_map;

// Live parameter set tracking
struct SPSData {
    char* data;
    size_t size;
    uint8_t sps_id;

    SPSData() : data(NULL), size(0), sps_id(0) {}
    ~SPSData() { if (data) delete[] data; }
};

// Maps to store latest parameter sets by ID
static std::map<uint8_t, SPSData> latest_sps_map;
static std::map<uint8_t, PPSData> latest_pps_map;

// Periodic refresh counter
static uint32_t refresh_counter = 0;
static const uint32_t REFRESH_INTERVAL = 30; // Send SPS/PPS every 30 frames

// RTP sequence number (strictly monotonic) - using global rtp_sequence_number above

// RTP timestamp (90kHz clock)
static uint32_t rtp_timestamp = 0;

// Statistics tracking variables
static uint32_t stats_frames_extracted = 0;
static uint32_t stats_packets_sent = 0;
static uint32_t stats_bytes_sent = 0;
static auto stats_start_time = std::chrono::high_resolution_clock::now();
static auto stats_last_report_time = std::chrono::high_resolution_clock::now();
static auto frame_extraction_start_time = std::chrono::high_resolution_clock::now();

/* RTP Header structure - using direct byte manipulation */
struct RTPHeader {
    uint8_t byte0;           // Version(2) + Padding(1) + Extension(1) + CSRC Count(4)
    uint8_t byte1;           // Marker(1) + Payload Type(7)
    uint16_t sequence_number; // Sequence number
    uint32_t timestamp;      // Timestamp
    uint32_t ssrc;           // SSRC identifier
} __attribute__((packed));

// MTU and fragmentation constants (conservative limits to avoid IP fragmentation)
static const size_t ETH_MTU            = 1500;
static const size_t IP_UDP_RTP_OVERHEAD= 20 + 8 + 12;    // IP + UDP + RTP = 40 bytes
static const size_t FU_A_OVERHEAD      = 2;              // FU indicator + FU header
static const size_t MAX_RTP_PACKET     = 1400;           // Conservative limit (safer than 1500)
static const size_t MAX_SINGLE_PAYLOAD = MAX_RTP_PACKET - sizeof(RTPHeader) - 1; // single NAL
static const size_t MAX_FRAG_PAYLOAD   = MAX_RTP_PACKET - sizeof(RTPHeader) - FU_A_OVERHEAD; // FU-A

/* RTP H.264 payload header - single byte */
struct RTPH264PayloadHeader {
    uint8_t forbidden_zero_bit:1;  // Always 0
    uint8_t nal_ref_idc:2;        // NAL reference IDC
    uint8_t nal_unit_type:5;      // NAL unit type
} __attribute__((packed));

/* RTP H.264 fragmentation header - single byte */
struct RTPH264FragmentationHeader {
    uint8_t start_bit:1;   // Start bit
    uint8_t end_bit:1;     // End bit
    uint8_t reserved:1;    // Reserved (must be 0)
    uint8_t nal_unit_type:5; // NAL unit type
} __attribute__((packed));

/* RTSP Server parameters */
static bool         RTSP_SERVER = false;
static int          RTSP_PORT = 8522;
static RTSPServer   rtsp_server;

/* Debug print macros */
#define PRODUCER_PRINT(...) printf("PRODUCER: " __VA_ARGS__)
#define CONSUMER_PRINT(...) printf("CONSUMER: " __VA_ARGS__)

static EGLDisplay   eglDisplay = EGL_NO_DISPLAY;
static EGLContext   eglContext = EGL_NO_CONTEXT;
static EGLSurface   eglSurface = EGL_NO_SURFACE;

/* UDP Threading (outside namespace for global access) */
static pthread_t    UDP_THREAD;
static pthread_mutex_t UDP_QUEUE_MUTEX = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t UDP_QUEUE_COND = PTHREAD_COND_INITIALIZER;
static bool         UDP_THREAD_RUNNING = false;
static bool         UDP_THREAD_STOP = false;

struct UDPPacket {
    char* data;
    size_t size;
    UDPPacket(const char* d, size_t s) : size(s) {
        data = new char[s];
        memcpy(data, d, s);
    }
    ~UDPPacket() { delete[] data; }
};

static std::queue<UDPPacket*> UDP_PACKET_QUEUE;

/**
 * Print statistics every 5 seconds
 */
static void printStatistics() {
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - stats_last_report_time);
    
    if (elapsed.count() >= 5000) { // Every 5 seconds
        auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - stats_start_time);
        double fps = (double)stats_frames_extracted / (total_elapsed.count() / 1000.0);
        double avg_latency = stats_frames_extracted > 0 ? (double)elapsed.count() / stats_frames_extracted : 0;
        
        printf("STATS: Frames=%u, FPS=%.1f, Packets=%u, Bytes=%u, Latency=%.1fms\n",
               stats_frames_extracted, fps, stats_packets_sent, stats_bytes_sent, avg_latency);
        
        stats_last_report_time = now;
    }
}

namespace ArgusSamples
{

// Forward declarations
static uint8_t extractSPSId(const char* sps_data, size_t sps_size);
static void sendParameterSetsBeforeIDR();
static void sendParameterSetsPeriodically();
static void updateRTSPWithLatestParameterSets();
static void createSTAPAPacket(char* packet, size_t* packet_size, 
                             const std::map<uint8_t, SPSData>& sps_map,
                             const std::map<uint8_t, PPSData>& pps_map);

/**
 * Initialize EGL for headless operation
 */
static bool initializeHeadlessEGL()
{
    EGLint major, minor;
    EGLConfig config;
    EGLint numConfigs;
    EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLint pbufferAttribs[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_NONE
    };

    // Get EGL display
    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY) {
        printf("Failed to get EGL display\n");
        return false;
    }

    // Initialize EGL
    if (!eglInitialize(eglDisplay, &major, &minor)) {
        printf("Failed to initialize EGL\n");
        return false;
    }

    if (VERBOSE_ENABLE)
        printf("EGL initialized: %d.%d\n", major, minor);

    // Choose config
    if (!eglChooseConfig(eglDisplay, configAttribs, &config, 1, &numConfigs)) {
        printf("Failed to choose EGL config\n");
        return false;
    }

    // Create context
    eglContext = eglCreateContext(eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext == EGL_NO_CONTEXT) {
        printf("Failed to create EGL context\n");
        return false;
    }

    // Create pbuffer surface
    eglSurface = eglCreatePbufferSurface(eglDisplay, config, pbufferAttribs);
    if (eglSurface == EGL_NO_SURFACE) {
        printf("Failed to create EGL surface\n");
        return false;
    }

    // Make context current
    if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
        printf("Failed to make EGL context current\n");
        return false;
    }

    if (VERBOSE_ENABLE)
        printf("Headless EGL initialized successfully\n");

    return true;
}

/**
 * Cleanup EGL resources
 */
static void cleanupEGL()
{
    if (eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        
        if (eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay, eglSurface);
            eglSurface = EGL_NO_SURFACE;
        }
        
        if (eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay, eglContext);
            eglContext = EGL_NO_CONTEXT;
        }
        
        eglTerminate(eglDisplay);
        eglDisplay = EGL_NO_DISPLAY;
    }
}

/**
 * List all available cameras and their sensor modes
 */
static bool listAvailableCameras()
{
    printf("=== Available Cameras and Sensor Modes ===\n");
    
    /* Create the CameraProvider object and get the core interface */
    UniqueObj<CameraProvider> cameraProvider = UniqueObj<CameraProvider>(CameraProvider::create());
    ICameraProvider *iCameraProvider = interface_cast<ICameraProvider>(cameraProvider);
    if (!iCameraProvider) {
        printf("Failed to create CameraProvider\n");
        return false;
    }
    
    /* Get the camera devices */
    std::vector<CameraDevice*> cameraDevices;
    iCameraProvider->getCameraDevices(&cameraDevices);
    if (cameraDevices.size() == 0) {
        printf("No cameras available\n");
        return false;
    }
    
    printf("Found %zu camera device(s):\n\n", cameraDevices.size());
    
    for (uint32_t i = 0; i < cameraDevices.size(); i++) {
        ICameraProperties *iCameraProperties = interface_cast<ICameraProperties>(cameraDevices[i]);
        if (!iCameraProperties) {
            printf("Camera %d: Failed to get properties\n", i);
            continue;
        }
        
        printf("Camera %d:\n", i);
        printf("  Device: %p\n", cameraDevices[i]);
        
        /* Get sensor modes */
        std::vector<SensorMode*> sensorModes;
        iCameraProperties->getBasicSensorModes(&sensorModes);
        
        if (sensorModes.size() == 0) {
            printf("  No sensor modes available\n");
            continue;
        }
        
        printf("  Available sensor modes (%zu):\n", sensorModes.size());
        
        for (uint32_t j = 0; j < sensorModes.size(); j++) {
            ISensorMode *iSensorMode = interface_cast<ISensorMode>(sensorModes[j]);
            if (!iSensorMode) {
                printf("    Mode %d: Failed to get sensor mode interface\n", j);
                continue;
            }
            
            Size2D<uint32_t> resolution = iSensorMode->getResolution();
            uint64_t frameDuration = iSensorMode->getFrameDurationRange().min();
            uint32_t fps = (uint32_t)(1000000000ULL / frameDuration);
            
            printf("    Mode %d: %dx%d @ %d fps (frame duration: %lu ns)\n", 
                   j, resolution.width(), resolution.height(), fps, frameDuration);
            
            /* Get additional sensor mode properties */
            Range<uint64_t> frameDurationRange = iSensorMode->getFrameDurationRange();
            printf("      Frame duration range: %lu - %lu ns\n", 
                   frameDurationRange.min(), frameDurationRange.max());
            
            /* Get analog gain range */
            Range<float> analogGainRange = iSensorMode->getAnalogGainRange();
            printf("      Analog gain range: %.2f - %.2f\n", 
                   analogGainRange.min(), analogGainRange.max());
            
            /* Get exposure time range */
            Range<uint64_t> exposureTimeRange = iSensorMode->getExposureTimeRange();
            printf("      Exposure time range: %lu - %lu ns\n", 
                   exposureTimeRange.min(), exposureTimeRange.max());
        }
        printf("\n");
    }
    
    return true;
}

/**
 * UDP Sender Thread Function
 */
static void* udpSenderThread(void* /*arg*/)
{
    UDP_THREAD_RUNNING = true;
    
    while (!UDP_THREAD_STOP) {
        pthread_mutex_lock(&UDP_QUEUE_MUTEX);
        
        // Wait for packets or stop signal
        while (UDP_PACKET_QUEUE.empty() && !UDP_THREAD_STOP) {
            pthread_cond_wait(&UDP_QUEUE_COND, &UDP_QUEUE_MUTEX);
        }
        
        if (UDP_THREAD_STOP) {
            pthread_mutex_unlock(&UDP_QUEUE_MUTEX);
            break;
        }
        
        // Get packet from queue
        UDPPacket* packet = UDP_PACKET_QUEUE.front();
        UDP_PACKET_QUEUE.pop();
        pthread_mutex_unlock(&UDP_QUEUE_MUTEX);
        
        // Send packet as single UDP datagram (RTP packet must not be split)
        ssize_t sent = sendto(UDP_SOCKET, packet->data, packet->size, 0,
                             (struct sockaddr*)&UDP_ADDR, sizeof(UDP_ADDR));
        
        if (sent < 0) {
            printf("Failed to send UDP packet: %s\n", strerror(errno));
        } else {
            DEBUG_PACKET_COUNT++;
            DEBUG_BYTES_SENT += sent;
        }
        
        
        // Clean up packet
        delete packet;
    }
    
    UDP_THREAD_RUNNING = false;
    return NULL;
}

/**
 * Initialize UDP socket for streaming
 */
static bool initializeUDP()
{
    UDP_SOCKET = socket(AF_INET, SOCK_DGRAM, 0);
    if (UDP_SOCKET < 0) {
        printf("Failed to create UDP socket\n");
        return false;
    }

    memset(&UDP_ADDR, 0, sizeof(UDP_ADDR));
    UDP_ADDR.sin_family = AF_INET;
    UDP_ADDR.sin_port = htons(UDP_PORT);
    
    if (inet_pton(AF_INET, UDP_HOST.c_str(), &UDP_ADDR.sin_addr) <= 0) {
        printf("Invalid UDP host address: %s\n", UDP_HOST.c_str());
        close(UDP_SOCKET);
        UDP_SOCKET = -1;
        return false;
    }
    
    printf("UDP: Socket created successfully, sending to %s:%d\n", UDP_HOST.c_str(), UDP_PORT);

    // Start UDP sender thread
    UDP_THREAD_STOP = false;
    if (pthread_create(&UDP_THREAD, NULL, udpSenderThread, NULL) != 0) {
        printf("Failed to create UDP sender thread\n");
        close(UDP_SOCKET);
        UDP_SOCKET = -1;
        return false;
    }
    
    printf("UDP: Sender thread started\n");

    return true;
}

/**
 * Find NAL unit start codes in H.264 data
 */
static const char* findNALStartCode(const char* data, size_t size, size_t* start_code_length)
{
    for (size_t i = 0; i < size - 3; i++) {
        if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 && data[i+3] == 0x01) {
            *start_code_length = 4;
            return data + i;
        } else if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x01) {
            *start_code_length = 3;
            return data + i;
        }
    }
    return NULL;
}

/**
 * Extract SPS and PPS from H.264 stream
 */
static void extractSPSPPS(const char* data, size_t size)
{
    const char* ptr = data;
    size_t remaining = size;
    
    while (remaining > 4) {
        size_t start_code_length;
        const char* nal_start = findNALStartCode(ptr, remaining, &start_code_length);
        
        if (!nal_start) break;
        
        // Move past start code
        const char* nal_data = nal_start + start_code_length;
        
        // Find the next NAL unit to determine the size of current NAL unit
        const char* next_nal = findNALStartCode(nal_data, remaining - (nal_data - ptr), &start_code_length);
        size_t nal_size;
        
        if (next_nal) {
            nal_size = next_nal - nal_data;
        } else {
            nal_size = remaining - (nal_data - ptr);
        }
        
        if (nal_size > 0) {
            uint8_t nal_header = nal_data[0];
            uint8_t nal_type = nal_header & 0x1F;
            
            if (nal_type == 7) { // SPS
                // Extract SPS ID using Exp-Golomb decoding
                uint8_t sps_id = extractSPSId(nal_data, nal_size);
                
                printf("Extracted SPS ID %d: %zu bytes, first byte: 0x%02X, NAL type: %d\n",
                       sps_id, nal_size, (unsigned char)nal_data[0], nal_type);
                
                // Store this SPS with its actual ID in live tracking map
                SPSData& sps = latest_sps_map[sps_id];
                if (sps.data) delete[] sps.data;
                sps.size = nal_size;
                sps.data = new char[sps.size];
                sps.sps_id = sps_id;
                memcpy(sps.data, nal_data, sps.size);
                
                // Print first few bytes for debugging
                printf("SPS ID %d data: ", sps_id);
                for (size_t i = 0; i < std::min(sps.size, (size_t)16); i++) {
                    printf("%02X ", (unsigned char)sps.data[i]);
                }
                printf("\n");
                
                // Also maintain legacy sps_data for backward compatibility
                if (sps_data) delete[] sps_data;
                sps_size = nal_size;
                sps_data = new char[sps_size];
                memcpy(sps_data, nal_data, sps_size);
                
                printf("Live SPS tracking updated - Total SPS count: %zu\n", latest_sps_map.size());
                
                // Update RTSP server SDP with latest parameter sets
                updateRTSPWithLatestParameterSets();
                
            } else if (nal_type == 8) { // PPS
                // Extract actual PPS ID from the PPS data using proper Exp-Golomb decoding
                // Based on GStreamer's implementation
                uint8_t pps_id = 0;
                
                if (nal_size > 1) {
                    // Skip the NAL header (1 byte) and decode the PPS ID
                    const uint8_t* pps_data_ptr = (const uint8_t*)nal_data + 1;
                    size_t remaining_bytes = nal_size - 1;
                    
                    // Debug: Print first few bytes of PPS data
                    printf("PPS raw data (first 8 bytes): ");
                    for (size_t i = 0; i < std::min(remaining_bytes, (size_t)8); i++) {
                        printf("%02X ", pps_data_ptr[i]);
                    }
                    printf("\n");
                    
                    // Exp-Golomb decoder for PPS ID
                    uint32_t bit_pos = 0;
                    uint32_t leading_zeros = 0;
                    
                    // Count leading zeros
                    while (bit_pos < remaining_bytes * 8) {
                        uint32_t byte_idx = bit_pos / 8;
                        uint32_t bit_idx = bit_pos % 8;
                        
                        if (byte_idx >= remaining_bytes) break;
                        
                        uint8_t bit = (pps_data_ptr[byte_idx] >> (7 - bit_idx)) & 1;
                        if (bit == 0) {
                            leading_zeros++;
                            bit_pos++;
                        } else {
                            break;
                        }
                    }
                    
                    printf("Leading zeros: %u, bit_pos: %u\n", leading_zeros, bit_pos);
                    
                    // Read the value (leading_zeros + 1 bits)
                    uint32_t value = 0;
                    for (uint32_t i = 0; i <= leading_zeros; i++) {
                        uint32_t byte_idx = bit_pos / 8;
                        uint32_t bit_idx = bit_pos % 8;
                        
                        if (byte_idx >= remaining_bytes) break;
                        
                        uint8_t bit = (pps_data_ptr[byte_idx] >> (7 - bit_idx)) & 1;
                        value = (value << 1) | bit;
                        bit_pos++;
                    }
                    
                    printf("Raw value: %u\n", value);
                    pps_id = value - 1; // Exp-Golomb values are offset by 1
                    printf("Final PPS ID: %u\n", pps_id);
                }
                
                printf("Extracted PPS ID %d: %zu bytes, first byte: 0x%02X, NAL type: %d\n", 
                       pps_id, nal_size, (unsigned char)nal_data[0], nal_type);
                
                // Store this PPS with its ACTUAL ID - capture ALL PPS throughout the stream
                PPSData& pps = latest_pps_map[pps_id];
                if (pps.data) delete[] pps.data;
                pps.size = nal_size;
                pps.data = new char[pps.size];
                pps.pps_id = pps_id;
                memcpy(pps.data, nal_data, nal_size);
                
                // Also maintain legacy pps_map for backward compatibility
                PPSData& legacy_pps = pps_map[pps_id];
                if (legacy_pps.data) delete[] legacy_pps.data;
                legacy_pps.size = nal_size;
                legacy_pps.data = new char[legacy_pps.size];
                legacy_pps.pps_id = pps_id;
                memcpy(legacy_pps.data, nal_data, nal_size);
                
                // Print first few bytes for debugging
                printf("PPS ID %d data: ", pps_id);
                for (size_t i = 0; i < std::min(pps.size, (size_t)16); i++) {
                    printf("%02X ", (unsigned char)pps.data[i]);
                }
                printf("\n");
                
                printf("Live PPS tracking updated - Total PPS count: %zu\n", latest_pps_map.size());
                
                // Update RTSP server SDP with latest parameter sets
                updateRTSPWithLatestParameterSets();
            }
        }
        
        // Move to next potential NAL unit
        ptr = nal_start + start_code_length;
        remaining = size - (ptr - data);
    }
}

// Forward declarations
/**
 * Create STAP-A packet for parameter sets (SPS + PPS)
 * STAP-A format: NAL header (type=24) + length1 + NAL1 + length2 + NAL2 + ...
 */
static void createSTAPAPacket(char* packet, size_t* packet_size, 
                             const std::map<uint8_t, SPSData>& sps_map,
                             const std::map<uint8_t, PPSData>& pps_map)
{
    if (!packet || !packet_size || sps_map.empty() || pps_map.empty()) {
        printf("ERROR: Invalid parameters to createSTAPAPacket\n");
        return;
    }
    
    const size_t MAX_STAP_PAYLOAD = MAX_RTP_PACKET - sizeof(RTPHeader) - 1; // -1 for STAP-A header
    
    size_t offset = sizeof(RTPHeader);
    
    // Compute maximum NRI from all contained NALs
    uint8_t max_nri = 0;
    for (const auto& sps_pair : sps_map) {
        const SPSData& sps = sps_pair.second;
        if (sps.data && sps.size > 0) {
            uint8_t nri = (sps.data[0] & 0x60) >> 5;
            if (nri > max_nri) max_nri = nri;
        }
    }
    for (const auto& pps_pair : pps_map) {
        const PPSData& pps = pps_pair.second;
        if (pps.data && pps.size > 0) {
            uint8_t nri = (pps.data[0] & 0x60) >> 5;
            if (nri > max_nri) max_nri = nri;
        }
    }
    
    // STAP-A NAL header (type = 24) with computed NRI
    packet[offset++] = static_cast<uint8_t>((0 << 7) | (max_nri << 5) | 24);
    
    size_t total_size = 1; // STAP-A header size
    
    // Add all SPS first (order matters: SPS before PPS)
    for (const auto& sps_pair : sps_map) {
        const SPSData& sps = sps_pair.second;
        if (sps.data && sps.size > 0) {
            size_t nal_size = sps.size;
            
            // Check if we have room for this NAL + length field
            if (total_size + 2 + nal_size > MAX_STAP_PAYLOAD) {
                printf("WARNING: STAP-A packet would exceed MTU, truncating\n");
                break;
            }
            
            // Add length field (16-bit big-endian)
            packet[offset++] = (nal_size >> 8) & 0xFF;
            packet[offset++] = nal_size & 0xFF;
            
            // Add NAL data
            memcpy(packet + offset, sps.data, nal_size);
            offset += nal_size;
            
            total_size += 2 + nal_size;
            //printf("STAP-A: Added SPS ID %d (%zu bytes)\n", sps.sps_id, nal_size);
        }
    }
    
    // Add all PPS after SPS
    for (const auto& pps_pair : pps_map) {
        const PPSData& pps = pps_pair.second;
        if (pps.data && pps.size > 0) {
            size_t nal_size = pps.size;
            
            // Check if we have room for this NAL + length field
            if (total_size + 2 + nal_size > MAX_STAP_PAYLOAD) {
                printf("WARNING: STAP-A packet would exceed MTU, truncating\n");
                break;
            }
            
            // Add length field (16-bit big-endian)
            packet[offset++] = (nal_size >> 8) & 0xFF;
            packet[offset++] = nal_size & 0xFF;
            
            // Add NAL data
            memcpy(packet + offset, pps.data, nal_size);
            offset += nal_size;
            
            total_size += 2 + nal_size;
            //printf("STAP-A: Added PPS ID %d (%zu bytes)\n", pps.pps_id, nal_size);
        }
    }
    
    // Create RTP header
    RTPHeader* rtp_header = (RTPHeader*)packet;
    rtp_header->byte0 = 0x80; // Version=2, Padding=0, Extension=0, CSRC=0
    rtp_header->byte1 = 0x60; // Marker=0, Payload Type=96 (H.264)
    rtp_header->sequence_number = htons(rtp_sequence_number++);
    rtp_header->timestamp = htonl(rtp_timestamp);
    rtp_header->ssrc = htonl(0x12345678);
    
    *packet_size = offset;
    
    //printf("STAP-A packet created: %zu bytes total (RTP header + STAP-A + %zu NALs)\n", 
    //       *packet_size, (sps_map.size() + pps_map.size()));
}

/**
 * Send SPS and PPS as RTP packets
 */
static void createRTPPacket(char* packet, size_t* packet_size,
                            const uint8_t* nal_full, size_t nal_size,
                            bool is_fragment, bool start_fragment, bool end_fragment)
{
    *packet_size = 0;
    if (!packet || !packet_size || !nal_full || nal_size == 0) return;

    // Original NAL header
    const uint8_t nal_header = nal_full[0];
    const uint8_t F   = (nal_header & 0x80) >> 7;
    const uint8_t NRI = (nal_header & 0x60) >> 5;
    const uint8_t NAL_TYPE = (nal_header & 0x1F);

    // RTP header
    RTPHeader* rtp = reinterpret_cast<RTPHeader*>(packet);
    rtp->byte0 = 0x80; // V=2
    // Marker (set later) + PT=96
    rtp->byte1 = 96;
    rtp->sequence_number = htons(rtp_sequence_number++);
    rtp->timestamp = htonl(rtp_timestamp);
    rtp->ssrc = htonl(rtp_ssrc);

    uint8_t* p = reinterpret_cast<uint8_t*>(packet) + sizeof(RTPHeader);

    if (!is_fragment) {
        // Single NAL unit: write full NAL header then payload
        *p++ = nal_header;
        size_t payload_size = nal_size - 1;
        memcpy(p, nal_full + 1, payload_size);
        p += payload_size;
        // Marker for single-NAL AU is set by caller (last NAL of AU)
        // caller will OR 0x80 into byte1 if needed
        *packet_size = (p - reinterpret_cast<uint8_t*>(packet));
        return;
    }

    // FU-A: Two bytes first: FU indicator + FU header
    // FU indicator = F|NRI|type=28
    const uint8_t fu_indicator = static_cast<uint8_t>((F << 7) | (NRI << 5) | 28);
    // FU header = S|E|R=0|orig_type
    const uint8_t fu_header    = static_cast<uint8_t>(((start_fragment ? 1 : 0) << 7) |
                                                      ((end_fragment   ? 1 : 0) << 6) |
                                                      (0 << 5) | NAL_TYPE);
    *p++ = fu_indicator;
    *p++ = fu_header;

    // Fragment payload = NAL payload without original header
    const uint8_t* frag_payload = nal_full + 1;
    size_t payload_size = nal_size - 1;
    memcpy(p, frag_payload, payload_size);
    p += payload_size;

    // M-bit (marker) must be set ONLY on the **last fragment of the last NAL of the AU**.
    // The caller decides and ORs 0x80 into rtp->byte1 when appropriate.
    *packet_size = (p - reinterpret_cast<uint8_t*>(packet));
}

/**
 * Validate RTP packet integrity (similar to GStreamer validation)
 */
static bool validateRTPPacket(const char* packet, size_t packet_size) {
    if (!packet || packet_size < sizeof(RTPHeader)) {
        return false;
    }
    
    const RTPHeader* header = (const RTPHeader*)packet;
    
    // Check RTP version (must be 2)
    if ((header->byte0 & 0xC0) != 0x80) {
        printf("ERROR: Invalid RTP version\n");
        return false;
    }
    
    // Check payload type (should be 96 for H.264)
    uint8_t payload_type = header->byte1 & 0x7F;
    if (payload_type != 96) {
        printf("ERROR: Invalid payload type: %d\n", payload_type);
        return false;
    }
    
    // Check sequence number (should be increasing)
    static uint16_t last_seq = 0;
    uint16_t current_seq = ntohs(header->sequence_number);
    if (last_seq != 0 && current_seq != (last_seq + 1) && current_seq != 0) {
        printf("WARNING: Sequence number gap: %d -> %d\n", last_seq, current_seq);
    }
    last_seq = current_seq;
    
    return true;
}

/**
 * Send RTP packet to both UDP and RTSP clients
 */
static void sendRTPPacketToAllClients(const char* rtp_packet, size_t packet_size) {
    if (packet_size > 0) {
        // Validate RTP packet integrity
        if (!validateRTPPacket(rtp_packet, packet_size)) {
            printf("ERROR: Invalid RTP packet, not sending\n");
            return;
        }
        
        // Update statistics
        stats_packets_sent++;
        stats_bytes_sent += packet_size;
        
        // Send to UDP clients (only if UDP streaming is enabled)
        if (UDP_STREAMING && UDP_SOCKET >= 0 && UDP_THREAD_RUNNING) {
            UDPPacket* packet = new UDPPacket(rtp_packet, packet_size);
            pthread_mutex_lock(&UDP_QUEUE_MUTEX);
            UDP_PACKET_QUEUE.push(packet);
            pthread_cond_signal(&UDP_QUEUE_COND);
            pthread_mutex_unlock(&UDP_QUEUE_MUTEX);
        }
        
        // Send to RTSP clients if server is running
        if (RTSP_SERVER) {
            rtsp_server.sendRTPPacket(rtp_packet, packet_size);
        }
        
        // Print statistics every 5 seconds
        printStatistics();
    }
}

/**
 * Send H.264 data via UDP using RTP packets
 */
static bool sendRTPData(const char* data, size_t size)
{
    static int frame_count = 0;
    static auto last_frame_time = std::chrono::high_resolution_clock::now();
    frame_count++;
    
    // Calculate frame timing for consistent delivery
    auto current_time = std::chrono::high_resolution_clock::now();
    auto frame_duration = std::chrono::duration_cast<std::chrono::microseconds>(current_time - last_frame_time);
    
    // Target frame rate: 30fps = 33.33ms per frame
    const int target_frame_us = 33333; // 33.33ms in microseconds
    
    // Track frame extraction timing
    frame_extraction_start_time = current_time;
    stats_frames_extracted++;
    
    // If frames are coming too fast, add a small delay to maintain consistent timing
    if (frame_duration.count() < target_frame_us) {
        int delay_us = target_frame_us - frame_duration.count();
        if (delay_us > 0 && delay_us < 10000) { // Only delay up to 10ms
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
        }
    }
    
    last_frame_time = current_time;
    
    if (!data || size == 0) return false;
    
    // Extract SPS/PPS on first frame (for SDP generation)
    if (!sps_pps_sent) {
        extractSPSPPS(data, size);
        sps_pps_sent = true;
    }
    
    const char* ptr = data;
    const char* end = data + size;
    
    int nal_count = 0;
    std::vector<std::pair<const char*, size_t>> nal_units; // Store NAL units to process them together
    
    // First pass: collect all NAL units and capture parameter sets throughout the stream
    while (ptr < end - 4) {
        // Look for NAL unit start code
        if (ptr[0] == 0x00 && ptr[1] == 0x00 && ptr[2] == 0x00 && ptr[3] == 0x01) {
            nal_count++;
            const char* nal_start = ptr + 4;
            const char* nal_end = ptr + 4;
            
            // Find end of NAL unit
            while (nal_end < end - 4) {
                if (nal_end[0] == 0x00 && nal_end[1] == 0x00 && 
                    nal_end[2] == 0x00 && nal_end[3] == 0x01) {
                    break;
                }
                nal_end++;
            }
            
            size_t nal_size = nal_end - nal_start;
            if (nal_size == 0 || nal_size < 1) {
                printf("WARNING: Empty or invalid NAL unit, skipping\n");
                ptr = nal_end;
                continue;
            }
            
            uint8_t nal_type = nal_start[0] & 0x1F;
            
            // Capture SPS/PPS NAL units throughout the entire stream (not just initial ones)
            if (nal_type == 7) { // SPS
                // Extract SPS ID using Exp-Golomb decoding
                uint8_t sps_id = extractSPSId(nal_start, nal_size);
                
                printf("Stream SPS ID %d: %zu bytes, first byte: 0x%02X, NAL type: %d\n", 
                       sps_id, nal_size, (unsigned char)nal_start[0], nal_type);
                
                // Store this SPS with its actual ID in live tracking map
                SPSData& sps = latest_sps_map[sps_id];
                if (sps.data) delete[] sps.data;
                sps.size = nal_size;
                sps.data = new char[sps.size];
                sps.sps_id = sps_id;
                memcpy(sps.data, nal_start, sps.size);
                
                printf("Stream SPS tracking updated - Total SPS count: %zu\n", latest_sps_map.size());
                
                // Update RTSP server SDP with latest parameter sets
                updateRTSPWithLatestParameterSets();
                
                // Continue processing - don't skip SPS, send it as regular NAL
                
            } else if (nal_type == 8) { // PPS
                // Extract actual PPS ID from the PPS data using proper Exp-Golomb decoding
                uint8_t pps_id = 0;
                
                if (nal_size > 1) {
                    // Skip the NAL header (1 byte) and decode the PPS ID
                    const uint8_t* pps_data_ptr = (const uint8_t*)nal_start + 1;
                    size_t remaining_bytes = nal_size - 1;
                    
                    // Exp-Golomb decoder for PPS ID
                    uint32_t bit_pos = 0;
                    uint32_t leading_zeros = 0;
                    
                    // Count leading zeros
                    while (bit_pos < remaining_bytes * 8) {
                        uint32_t byte_idx = bit_pos / 8;
                        uint32_t bit_idx = bit_pos % 8;
                        
                        if (byte_idx >= remaining_bytes) break;
                        
                        uint8_t bit = (pps_data_ptr[byte_idx] >> (7 - bit_idx)) & 1;
                        if (bit == 0) {
                            leading_zeros++;
                            bit_pos++;
                        } else {
                            break;
                        }
                    }
                    
                    // Read the value (leading_zeros + 1 bits)
                    uint32_t value = 0;
                    for (uint32_t i = 0; i <= leading_zeros; i++) {
                        uint32_t byte_idx = bit_pos / 8;
                        uint32_t bit_idx = bit_pos % 8;
                        
                        if (byte_idx >= remaining_bytes) break;
                        
                        uint8_t bit = (pps_data_ptr[byte_idx] >> (7 - bit_idx)) & 1;
                        value = (value << 1) | bit;
                        bit_pos++;
                    }
                    
                    pps_id = value - 1; // Exp-Golomb values are offset by 1
                }
                
                printf("Stream PPS ID %d: %zu bytes, first byte: 0x%02X, NAL type: %d\n", 
                       pps_id, nal_size, (unsigned char)nal_start[0], nal_type);
                
                // Store this PPS with its ACTUAL ID - capture ALL PPS throughout the stream
                PPSData& pps = latest_pps_map[pps_id];
                if (pps.data) delete[] pps.data;
                pps.size = nal_size;
                pps.data = new char[pps.size];
                pps.pps_id = pps_id;
                memcpy(pps.data, nal_start, pps.size);

                // Also maintain legacy pps_map for backward compatibility
                PPSData& legacy_pps = pps_map[pps_id];
                if (legacy_pps.data) delete[] legacy_pps.data;
                legacy_pps.size = nal_size;
                legacy_pps.data = new char[legacy_pps.size];
                legacy_pps.pps_id = pps_id;
                memcpy(legacy_pps.data, nal_start, legacy_pps.size);
                
                printf("Stream PPS tracking updated - Total PPS count: %zu (ID %d captured)\n", latest_pps_map.size(), pps_id);
                
                // Update RTSP server SDP with latest parameter sets
                updateRTSPWithLatestParameterSets();
                
                // Continue processing - don't skip PPS, send it as regular NAL
            }
            
            // Store ALL NAL units for processing (including SPS/PPS)
            nal_units.push_back({nal_start, nal_size});
            
            ptr = nal_end;
        } else {
            ptr++;
        }
    }
    
    // Second pass: process all NAL units with proper marker bit handling
    
    // Check for IDR frames and send parameter sets before them
    bool has_idr_frame = false;
    for (size_t i = 0; i < nal_units.size(); i++) {
        uint8_t nal_type = nal_units[i].first[0] & 0x1F;
        if (nal_type == 5) { // IDR frame
            has_idr_frame = true;
            break;
        }
    }
    
    // Send parameter sets before IDR frames
    if (has_idr_frame) {
        sendParameterSetsBeforeIDR();
    }
    
    // Periodic refresh of parameter sets
    refresh_counter++;
    if (refresh_counter >= REFRESH_INTERVAL) {
        sendParameterSetsPeriodically();
        refresh_counter = 0;
    }
    
    for (size_t i = 0; i < nal_units.size(); i++) {
        const char* nal_start = nal_units[i].first;
        size_t nal_size = nal_units[i].second;
        uint8_t nal_type = nal_start[0] & 0x1F;
        bool is_last_nal = (i == nal_units.size() - 1); // Last NAL unit in frame
        
        // Skip SPS/PPS as individual packets - they are sent via STAP-A before IDR and periodically
        if (nal_type == 7 || nal_type == 8) {
            continue;
        }
        
        // Use original NAL data for non-parameter-set NALs
        const char* nal_data_to_send = nal_start;
        
        if (nal_size <= MAX_SINGLE_PAYLOAD) {
            // Single packet
            char rtp_packet[1500];
            size_t packet_size;
            createRTPPacket(rtp_packet, &packet_size, (const uint8_t*)nal_data_to_send, nal_size, false, false, false);
            
            // Set M-bit for single NAL if it's the last NAL of the AU
            if (packet_size > 0 && is_last_nal) {
                reinterpret_cast<RTPHeader*>(rtp_packet)->byte1 |= 0x80;
            }
            
            sendRTPPacketToAllClients(rtp_packet, packet_size);
        } else {
            // Fragmented packet (FU-A) - RFC 6184 compliant
            const size_t MAX_PAYLOAD = MAX_FRAG_PAYLOAD; // Use conservative MTU limit

            size_t remaining = nal_size - 1;             // without original NAL header
            const uint8_t* full_nal = (const uint8_t*)nal_data_to_send;
            const uint8_t* frag_ptr = full_nal + 1;

            size_t frag_num = 0;
            
            while (remaining > 0) {
                size_t frag_payload = (remaining > MAX_PAYLOAD) ? MAX_PAYLOAD : remaining;

                // Build a temporary buffer consisting of [NAL_HEADER][FRAG_PAYLOAD]
                // so createRTPPacket can copy from +1 and keep F/NRI/type.
                uint8_t tmp[1 + 1400]; // Fixed size buffer for fragment data
                tmp[0] = full_nal[0];
                memcpy(tmp + 1, frag_ptr, frag_payload);

                char rtp_packet[1500];
                size_t pkt_size = 0;

                bool start_frag = (frag_num == 0);
                bool end_frag   = (remaining <= frag_payload);
                bool last_fragment_of_last_nal = (end_frag && is_last_nal);

                createRTPPacket(rtp_packet, &pkt_size,
                                tmp, 1 + frag_payload,
                                true, start_frag, end_frag);

                // Set M-bit here if and only if this is the last fragment of the last NAL of the AU
                if (pkt_size > 0 && last_fragment_of_last_nal) {
                    reinterpret_cast<RTPHeader*>(rtp_packet)->byte1 |= 0x80;
                }

                if (pkt_size > 0) {
                    sendRTPPacketToAllClients(rtp_packet, pkt_size);
                } else {
                    printf("ERROR: Failed to create fragment %zu\n", frag_num);
                    break;
                }

                remaining -= frag_payload;
                frag_ptr  += frag_payload;
                frag_num++;
            }
        }
    }
    
    
    // Update timestamp for next Access Unit (RFC 6184 compliant)
    // All packets in the same AU must share the same timestamp
    // Next AU increments the timestamp by a fixed amount for 30fps
    static const uint32_t TARGET_FPS = 30;
    static const uint32_t ts_increment = 90000 / TARGET_FPS; // 90kHz clock / fps
    
    rtp_timestamp += ts_increment;
    
    return true;
}

/**
 * Cleanup UDP socket and RTP resources
 */
static void cleanupUDP()
{
    // Stop UDP sender thread
    if (UDP_THREAD_RUNNING) {
        UDP_THREAD_STOP = true;
        pthread_cond_signal(&UDP_QUEUE_COND);
        pthread_join(UDP_THREAD, NULL);
        printf("UDP: Sender thread stopped\n");
    }
    
    // Clean up remaining packets in queue
    pthread_mutex_lock(&UDP_QUEUE_MUTEX);
    while (!UDP_PACKET_QUEUE.empty()) {
        UDPPacket* packet = UDP_PACKET_QUEUE.front();
        UDP_PACKET_QUEUE.pop();
        delete packet;
    }
    pthread_mutex_unlock(&UDP_QUEUE_MUTEX);
    
    if (UDP_SOCKET >= 0) {
        close(UDP_SOCKET);
        UDP_SOCKET = -1;
    }
    
    // Cleanup SPS/PPS data
    if (sps_data) {
        delete[] sps_data;
        sps_data = NULL;
        sps_size = 0;
    }
    
    // Cleanup all PPS
    pps_map.clear();
    
    // Cleanup live tracking maps
    latest_sps_map.clear();
    latest_pps_map.clear();
    
    sps_pps_sent = false;
    
    // Reset RTP state
    rtp_sequence_number = 100;
    rtp_timestamp = 0;
    sps_pps_sent = false;
}

/**
 * RTSP server thread function with proper RTP implementation
 */
// Old RTSP server thread function removed - now handled by RTSPServer class


/*
 * Helper class to map NvNativeBuffer to Argus::Buffer and vice versa.
 * This class extends NvBuffer to act as a shared buffer between Argus and V4L2 encoder.
 */
class DmaBuffer : public NvNativeBuffer, public NvBuffer
{
public:
    /* Always use this static method to create DmaBuffer */
    static DmaBuffer* create(const Argus::Size2D<uint32_t>& size,
                             NvBufSurfaceColorFormat colorFormat,
                             NvBufSurfaceLayout layout = NVBUF_LAYOUT_PITCH)
    {
        DmaBuffer* buffer = new DmaBuffer(size);
        if (!buffer)
            return NULL;

        NvBufSurf::NvCommonAllocateParams cParams;
        cParams.memtag = NvBufSurfaceTag_CAMERA;
        cParams.width = size.width();
        cParams.height = size.height();
        cParams.colorFormat = colorFormat;
        cParams.layout = layout;
        cParams.memType = NVBUF_MEM_SURFACE_ARRAY;

        if (NvBufSurf::NvAllocate(&cParams, 1, &buffer->m_fd))
        {
            delete buffer;
            return NULL;
        }

        /* save the DMABUF fd in NvBuffer structure */
        buffer->planes[0].fd = buffer->m_fd;
        /* byteused must be non-zero for a valid buffer */
        buffer->planes[0].bytesused = 1;

        return buffer;
    }

    /* Help function to convert Argus Buffer to DmaBuffer */
    static DmaBuffer* fromArgusBuffer(Buffer *buffer)
    {
        if (!buffer) {
            printf("ERROR: fromArgusBuffer called with NULL buffer\n");
            return NULL;
        }
        
        IBuffer* iBuffer = interface_cast<IBuffer>(buffer);
        if (!iBuffer) {
            printf("ERROR: Failed to get IBuffer interface\n");
            return NULL;
        }
        
        const DmaBuffer *dmabuf = static_cast<const DmaBuffer*>(iBuffer->getClientData());
        if (!dmabuf) {
            printf("ERROR: getClientData returned NULL - buffer not properly initialized\n");
            return NULL;
        }
        
        return const_cast<DmaBuffer*>(dmabuf);
    }

    /* Return DMA buffer handle */
    int getFd() const { return m_fd; }

    /* Get and set reference to Argus buffer */
    void setArgusBuffer(Buffer *buffer) { m_buffer = buffer; }
    Buffer *getArgusBuffer() const { return m_buffer; }

private:
    DmaBuffer(const Argus::Size2D<uint32_t>& size)
        : NvNativeBuffer(size),
          NvBuffer(0, 0),
          m_buffer(NULL)
    {
    }

    Buffer *m_buffer;   /* Reference to Argus::Buffer */
};

/**
 * Consumer thread:
 *   Acquire frames from BufferOutputStream and extract the DMABUF fd from it.
 *   Provide DMABUF to V4L2 for video encoding. The encoder will save the encoded
 *   stream to disk.
 */
class ConsumerThread : public Thread
{
public:
    explicit ConsumerThread(OutputStream* stream);
    ~ConsumerThread();

    bool isInError()
    {
        return m_gotError;
    }

private:
    /** @name Thread methods */
    /**@{*/
    virtual bool threadInitialize();
    virtual bool threadExecute();
    virtual bool threadShutdown();
    /**@}*/

    bool createVideoEncoder();
    void abort();

    static bool encoderCapturePlaneDqCallback(
            struct v4l2_buffer *v4l2_buf,
            NvBuffer *buffer,
            NvBuffer *shared_buffer,
            void *arg);

    OutputStream* m_stream;
    NvVideoEncoder *m_VideoEncoder;
    std::ofstream *m_outputFile;
    bool m_gotError;
};

ConsumerThread::ConsumerThread(OutputStream* stream) :
        m_stream(stream),
        m_VideoEncoder(NULL),
        m_outputFile(NULL),
        m_gotError(false)
{
}

ConsumerThread::~ConsumerThread()
{
    if (m_VideoEncoder)
        delete m_VideoEncoder;

    if (m_outputFile)
        delete m_outputFile;
}

bool ConsumerThread::threadInitialize()
{
    if (RAW_FRAME_MODE) {
        /* Raw frame mode - skip encoder setup */
        printf("Raw frame mode: Skipping encoder initialization\n");
        m_VideoEncoder = nullptr;
        m_outputFile = nullptr;
        return true;
    }

    /* Normal encoding mode */
    /* Create Video Encoder */
    if (!createVideoEncoder())
        ORIGINATE_ERROR("Failed to create video encoder");

    /* Create output file (optional if UDP streaming is enabled) */
    if (!OUTPUT_FILENAME.empty()) {
        m_outputFile = new std::ofstream(OUTPUT_FILENAME.c_str());
        if (!m_outputFile)
            ORIGINATE_ERROR("Failed to open output file.");
    } else {
        m_outputFile = nullptr;
    }

    /* Stream on */
    int e = m_VideoEncoder->output_plane.setStreamStatus(true);
    if (e < 0)
        ORIGINATE_ERROR("Failed to stream on output plane");
    e = m_VideoEncoder->capture_plane.setStreamStatus(true);
    if (e < 0)
        ORIGINATE_ERROR("Failed to stream on capture plane");

    /* Set video encoder callback */
    m_VideoEncoder->capture_plane.setDQThreadCallback(encoderCapturePlaneDqCallback);

    /* startDQThread starts a thread internally which calls the
       encoderCapturePlaneDqCallback whenever a buffer is dequeued
       on the plane */
    m_VideoEncoder->capture_plane.startDQThread(this);

    /* Enqueue all the empty capture plane buffers */
    for (uint32_t i = 0; i < m_VideoEncoder->capture_plane.getNumBuffers(); i++)
    {
        struct v4l2_buffer v4l2_buf;
        struct v4l2_plane planes[MAX_PLANES];

        memset(&v4l2_buf, 0, sizeof(v4l2_buf));
        memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

        v4l2_buf.index = i;
        v4l2_buf.m.planes = planes;

        if (m_VideoEncoder->capture_plane.qBuffer(v4l2_buf, NULL) < 0)
            ORIGINATE_ERROR("Failed to enqueue capture plane buffer");
    }

    return true;
}

bool ConsumerThread::threadExecute()
{
    IBufferOutputStream* stream = interface_cast<IBufferOutputStream>(m_stream);
    if (!stream)
        ORIGINATE_ERROR("Failed to get IBufferOutputStream interface");

    if (RAW_FRAME_MODE) {
        /* Raw frame mode - just count frames without encoding */
        printf("Raw frame mode: Counting frames without encoding\n");
        
        while (!m_gotError) {
            /* Acquire a Buffer from a completed capture request */
            Argus::Status status = STATUS_OK;
            Buffer* buffer = stream->acquireBuffer(TIMEOUT_INFINITE, &status);
            if (status == STATUS_END_OF_STREAM) {
                break;
            }
            
            if (!buffer) {
                printf("ERROR: acquireBuffer returned NULL buffer\n");
                break;
            }

            /* Convert Argus::Buffer to DmaBuffer */
            DmaBuffer *dmabuf = DmaBuffer::fromArgusBuffer(buffer);
            if (!dmabuf) {
                printf("ERROR: Failed to convert Argus buffer to DmaBuffer\n");
                break;
            }
            
            
            /* Release the frame immediately */
            stream->releaseBuffer(dmabuf->getArgusBuffer());
        }
        
        return true;
    }

    /* Normal encoding mode */
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];

    memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));
    v4l2_buf.m.planes = planes;

    for (int bufferIndex = 0; bufferIndex < MAX_ENCODER_FRAMES; bufferIndex++)
    {
        v4l2_buf.index = bufferIndex;
        Buffer* buffer = stream->acquireBuffer();
        if (!buffer) {
            ORIGINATE_ERROR("Failed to acquire buffer from stream");
        }
        
        /* Convert Argus::Buffer to DmaBuffer and queue into v4l2 encoder */
        DmaBuffer *dmabuf = DmaBuffer::fromArgusBuffer(buffer);
        if (!dmabuf) {
            ORIGINATE_ERROR("Failed to convert Argus buffer to DmaBuffer");
        }
        
        if (m_VideoEncoder->output_plane.qBuffer(v4l2_buf, dmabuf) < 0)
            ORIGINATE_ERROR("Failed to queue buffer to encoder");
    }

    /* Keep acquire frames and queue into encoder */
    while (!m_gotError)
    {
        NvBuffer *share_buffer;

        /* Dequeue from encoder first */
        if (m_VideoEncoder->output_plane.dqBuffer(v4l2_buf, NULL,
                                                    &share_buffer, 10) < 0)
            ORIGINATE_ERROR("Failed to dequeue from encoder output plane");
        
        /* Release the frame */
        DmaBuffer *dmabuf = static_cast<DmaBuffer*>(share_buffer);
        stream->releaseBuffer(dmabuf->getArgusBuffer());

        if (VERBOSE_ENABLE)
            printf("Released frame. %d\n", dmabuf->getFd());

        /* Acquire a Buffer from a completed capture request */
        Argus::Status status = STATUS_OK;
        Buffer* buffer = stream->acquireBuffer(TIMEOUT_INFINITE, &status);
        if (status == STATUS_END_OF_STREAM)
        {
            /* Timeout or error happen, exit */
            break;
        }
        
        if (!buffer) {
            printf("ERROR: acquireBuffer returned NULL buffer\n");
            break;
        }

        /* Convert Argus::Buffer to DmaBuffer and get FD */
        dmabuf = DmaBuffer::fromArgusBuffer(buffer);
        if (!dmabuf) {
            printf("ERROR: Failed to convert Argus buffer to DmaBuffer in main loop\n");
            break;
        }
        
        int dmabuf_fd = dmabuf->getFd();

        if (VERBOSE_ENABLE)
            printf("Acquired Frame. %d\n", dmabuf_fd);

        /* Push the frame into V4L2. */
        if (m_VideoEncoder->output_plane.qBuffer(v4l2_buf, dmabuf) < 0)
            ORIGINATE_ERROR("Failed to queue buffer to encoder");
    }

    /* Print profile result before EOS to make FPS more accurate */
    if (DO_STAT)
        m_VideoEncoder->printProfilingStats(std::cout);

    /* Send EOS */
    v4l2_buf.m.planes[0].m.fd = -1;
    v4l2_buf.m.planes[0].bytesused = 0;
    if (m_VideoEncoder->output_plane.qBuffer(v4l2_buf, NULL) < 0)
        ORIGINATE_ERROR("Failed to send EOS to encoder");

    /* Wait till capture plane DQ Thread finishes */
    m_VideoEncoder->capture_plane.waitForDQThread(2000);

    requestShutdown();
    return true;
}

bool ConsumerThread::threadShutdown()
{
    return true;
}

bool ConsumerThread::createVideoEncoder()
{
    int ret = 0;

    m_VideoEncoder = NvVideoEncoder::createVideoEncoder("enc0");
    if (!m_VideoEncoder)
        ORIGINATE_ERROR("Could not create video encoder");

    if (DO_STAT)
        m_VideoEncoder->enableProfiling();

    ret = m_VideoEncoder->setCapturePlaneFormat(ENCODER_PIXFMT, STREAM_SIZE.width(),
                                    STREAM_SIZE.height(), 2 * 1024 * 1024);
    if (ret < 0)
        ORIGINATE_ERROR("Could not set capture plane format");

    ret = m_VideoEncoder->setOutputPlaneFormat(V4L2_PIX_FMT_YUV420M, STREAM_SIZE.width(),
                                    STREAM_SIZE.height());
    if (ret < 0)
        ORIGINATE_ERROR("Could not set output plane format");

    ret = m_VideoEncoder->setBitrate(4 * 1024 * 1024);
    if (ret < 0)
        ORIGINATE_ERROR("Could not set bitrate");

    if (ENCODER_PIXFMT == V4L2_PIX_FMT_H264)
    {
        ret = m_VideoEncoder->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
    }
    else
    {
        ret = m_VideoEncoder->setProfile(V4L2_MPEG_VIDEO_H265_PROFILE_MAIN);
    }
    if (ret < 0)
        ORIGINATE_ERROR("Could not set encoder profile");

    if (ENCODER_PIXFMT == V4L2_PIX_FMT_H264)
    {
        ret = m_VideoEncoder->setLevel(V4L2_MPEG_VIDEO_H264_LEVEL_5_0);
        if (ret < 0)
            ORIGINATE_ERROR("Could not set encoder level");
    }

    ret = m_VideoEncoder->setRateControlMode(V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
    if (ret < 0)
        ORIGINATE_ERROR("Could not set rate control mode");

    ret = m_VideoEncoder->setIFrameInterval(30);
    if (ret < 0)
        ORIGINATE_ERROR("Could not set I-frame interval");

    ret = m_VideoEncoder->setFrameRate(30, 1);
    if (ret < 0)
        ORIGINATE_ERROR("Could not set encoder framerate");

    ret = m_VideoEncoder->setHWPresetType(V4L2_ENC_HW_PRESET_ULTRAFAST);
    if (ret < 0)
        ORIGINATE_ERROR("Could not set encoder HW Preset");

    /* Query, Export and Map the output plane buffers so that we can read
       raw data into the buffers */
    ret = m_VideoEncoder->output_plane.setupPlane(V4L2_MEMORY_DMABUF, 10, true, false);
    if (ret < 0)
        ORIGINATE_ERROR("Could not setup output plane");

    /* Query, Export and Map the output plane buffers so that we can write
       encoded data from the buffers */
    ret = m_VideoEncoder->capture_plane.setupPlane(V4L2_MEMORY_MMAP, 6, true, false);
    if (ret < 0)
        ORIGINATE_ERROR("Could not setup capture plane");

    printf("Video encoder created successfully\n");
    return true;
}

void ConsumerThread::abort()
{
    m_VideoEncoder->abort();
    m_gotError = true;
}

bool ConsumerThread::encoderCapturePlaneDqCallback(struct v4l2_buffer *v4l2_buf,
                                                   NvBuffer * buffer,
                                                   NvBuffer * /*shared_buffer*/,
                                                   void *arg)
{
    ConsumerThread *thiz = (ConsumerThread*)arg;

    if (!v4l2_buf)
    {
        thiz->abort();
        ORIGINATE_ERROR("Failed to dequeue buffer from encoder capture plane");
    }

    // Write to file if output file is open
    if (thiz->m_outputFile && thiz->m_outputFile->is_open()) {
        thiz->m_outputFile->write((char *) buffer->planes[0].data,
                                  buffer->planes[0].bytesused);
    }
    
    // Send RTP data to both UDP and RTSP clients
    if ((UDP_STREAMING || RTSP_SERVER) && buffer->planes[0].bytesused > 0) {
        sendRTPData((char *) buffer->planes[0].data, buffer->planes[0].bytesused);
    }

    if (thiz->m_VideoEncoder->capture_plane.qBuffer(*v4l2_buf, NULL) < 0)
    {
        thiz->abort();
        ORIGINATE_ERROR("Failed to enqueue buffer to encoder capture plane");
        return false;
    }

    /* GOT EOS from encoder. Stop dqthread */
    if (buffer->planes[0].bytesused == 0)
    {
        return false;
    }

    return true;
}

/**
 * Argus Producer thread:
 *   Opens the Argus camera driver, creates an BufferOutputStream to output
 *   frames, then performs repeating capture requests for CAPTURE_TIME
 *   seconds before closing the producer and Argus driver.
 */
static bool execute()
{
    NvBufSurface *surf[NUM_BUFFERS] = {0};

    /* Initialize EGL for headless operation if needed */
    if (HEADLESS_MODE) {
        if (!initializeHeadlessEGL()) {
            ORIGINATE_ERROR("Failed to initialize headless EGL");
        }
    } else {
        /* Initialize EGL for non-headless mode */
        if (!initializeHeadlessEGL()) {
            printf("Failed to initialize EGL in non-headless mode. Try using headless mode (-H)\n");
            ORIGINATE_ERROR("Failed to initialize EGL");
        }
    }

    /* Initialize UDP streaming if enabled */
    if (UDP_STREAMING) {
        if (!initializeUDP()) {
            ORIGINATE_ERROR("Failed to initialize UDP streaming");
        }
    }

    /* Create the CameraProvider object and get the core interface */
    UniqueObj<CameraProvider> cameraProvider = UniqueObj<CameraProvider>(CameraProvider::create());
    ICameraProvider *iCameraProvider = interface_cast<ICameraProvider>(cameraProvider);
    if (!iCameraProvider)
        ORIGINATE_ERROR("Failed to create CameraProvider");

    /* Get the camera devices */
    std::vector<CameraDevice*> cameraDevices;
    iCameraProvider->getCameraDevices(&cameraDevices);
    if (cameraDevices.size() == 0)
        ORIGINATE_ERROR("No cameras available");

    printf("Found %zu camera device(s)\n", cameraDevices.size());
    
    if (CAMERA_INDEX >= cameraDevices.size())
    {
        printf("Camera index %d out of range (max: %zu). Falling back to camera 0\n", 
               CAMERA_INDEX, cameraDevices.size() - 1);
        CAMERA_INDEX = 0;
    }
    
    printf("Using camera %d\n", CAMERA_INDEX);
    
    /* Show selected camera information */
    ICameraProperties *iCameraProperties = interface_cast<ICameraProperties>(cameraDevices[CAMERA_INDEX]);
    if (iCameraProperties) {
        std::vector<SensorMode*> sensorModes;
        iCameraProperties->getBasicSensorModes(&sensorModes);
        printf("Camera %d has %zu sensor modes available\n", CAMERA_INDEX, sensorModes.size());
        
        /* Validate sensor mode index if specified */
        if (SENSOR_MODE_INDEX >= 0) {
            if (SENSOR_MODE_INDEX >= (int)sensorModes.size()) {
                printf("Sensor mode index %d out of range (max: %zu). Using auto-selection.\n", 
                       SENSOR_MODE_INDEX, sensorModes.size() - 1);
                SENSOR_MODE_INDEX = -1;  // Reset to auto-select
            } else {
                ISensorMode *iSensorMode = interface_cast<ISensorMode>(sensorModes[SENSOR_MODE_INDEX]);
                if (iSensorMode) {
                    Size2D<uint32_t> resolution = iSensorMode->getResolution();
                    uint64_t frameDuration = iSensorMode->getFrameDurationRange().min();
                    uint32_t fps = (uint32_t)(1000000000ULL / frameDuration);
                    printf("Using sensor mode %d: %dx%d @ %d fps\n", 
                           SENSOR_MODE_INDEX, resolution.width(), resolution.height(), fps);
                    
                    /* Check if target FPS is compatible with this sensor mode */
                    if (TARGET_FPS > 0) {
                        Range<uint64_t> frameDurationRange = iSensorMode->getFrameDurationRange();
                        uint64_t minFrameDuration = frameDurationRange.min();
                        uint64_t maxFrameDuration = frameDurationRange.max();
                        uint32_t maxFps = (uint32_t)(1000000000ULL / minFrameDuration);
                        uint32_t minFps = (uint32_t)(1000000000ULL / maxFrameDuration);
                        
                        if ((int)TARGET_FPS > (int)maxFps || (int)TARGET_FPS < (int)minFps) {
                            printf("Warning: Target FPS %d is outside sensor mode range (%d-%d fps). Using sensor mode default.\n",
                                   TARGET_FPS, minFps, maxFps);
                            TARGET_FPS = -1;  // Reset to use sensor mode default
                        } else {
                            printf("Target FPS %d is within sensor mode range (%d-%d fps)\n",
                                   TARGET_FPS, minFps, maxFps);
                        }
                    }
                }
            }
        } else {
            printf("Using auto-selection for sensor mode based on resolution %dx%d", 
                   STREAM_SIZE.width(), STREAM_SIZE.height());
            if (TARGET_FPS > 0) {
                printf(" and target FPS %d", TARGET_FPS);
            }
            printf("\n");
        }
    }

    /* Create the capture session using the first device and get the core interface */
    UniqueObj<CaptureSession> captureSession(
            iCameraProvider->createCaptureSession(cameraDevices[CAMERA_INDEX]));
    ICaptureSession *iCaptureSession = interface_cast<ICaptureSession>(captureSession);
    if (!iCaptureSession)
        ORIGINATE_ERROR("Failed to get ICaptureSession interface");

    /* Create the OutputStream */
    UniqueObj<OutputStreamSettings> streamSettings(
        iCaptureSession->createOutputStreamSettings(STREAM_TYPE_BUFFER));
    IBufferOutputStreamSettings *iStreamSettings =
        interface_cast<IBufferOutputStreamSettings>(streamSettings);
    if (!iStreamSettings)
        ORIGINATE_ERROR("Failed to get IBufferOutputStreamSettings interface");

    /* Configure the OutputStream to use the EGLImage BufferType */
    iStreamSettings->setBufferType(BUFFER_TYPE_EGL_IMAGE);

    /* Create the OutputStream */
    UniqueObj<OutputStream> outputStream(iCaptureSession->createOutputStream(streamSettings.get()));
    IBufferOutputStream *iBufferOutputStream = interface_cast<IBufferOutputStream>(outputStream);

    /* Allocate native buffers */
    DmaBuffer* nativeBuffers[NUM_BUFFERS];

    for (uint32_t i = 0; i < NUM_BUFFERS; i++)
    {
        nativeBuffers[i] = DmaBuffer::create(STREAM_SIZE, NVBUF_COLOR_FORMAT_NV12,
                    NVBUF_LAYOUT_BLOCK_LINEAR);
        if (!nativeBuffers[i])
            ORIGINATE_ERROR("Failed to allocate NativeBuffer");
    }

    /* Create EGLImages from the native buffers */
    EGLImageKHR eglImages[NUM_BUFFERS];
    for (uint32_t i = 0; i < NUM_BUFFERS; i++)
    {
        int ret = 0;

        ret = NvBufSurfaceFromFd(nativeBuffers[i]->getFd(), (void**)(&surf[i]));
        if (ret)
            ORIGINATE_ERROR("%s: NvBufSurfaceFromFd failed\n", __func__);

        ret = NvBufSurfaceMapEglImage (surf[i], 0);
        if (ret)
            ORIGINATE_ERROR("%s: NvBufSurfaceMapEglImage failed\n", __func__);

        eglImages[i] = surf[i]->surfaceList[0].mappedAddr.eglImage;
        if (eglImages[i] == EGL_NO_IMAGE_KHR)
            ORIGINATE_ERROR("Failed to create EGLImage");
    }

    /* Create the BufferSettings object to configure Buffer creation */
    UniqueObj<BufferSettings> bufferSettings(iBufferOutputStream->createBufferSettings());
    IEGLImageBufferSettings *iBufferSettings =
        interface_cast<IEGLImageBufferSettings>(bufferSettings);
    if (!iBufferSettings)
        ORIGINATE_ERROR("Failed to create BufferSettings");

    /* Create the Buffers for each EGLImage (and release to
       stream for initial capture use) */
    UniqueObj<Buffer> buffers[NUM_BUFFERS];
    for (uint32_t i = 0; i < NUM_BUFFERS; i++)
    {
        iBufferSettings->setEGLImage(eglImages[i]);
        iBufferSettings->setEGLDisplay(eglDisplay);
        buffers[i].reset(iBufferOutputStream->createBuffer(bufferSettings.get()));
        IBuffer *iBuffer = interface_cast<IBuffer>(buffers[i]);

        if (!iBuffer) {
            ORIGINATE_ERROR("Failed to get IBuffer interface for buffer %d", i);
        }
        
        if (!nativeBuffers[i]) {
            ORIGINATE_ERROR("Native buffer %d is NULL", i);
        }

        /* Reference Argus::Buffer and DmaBuffer each other */
        iBuffer->setClientData(nativeBuffers[i]);
        nativeBuffers[i]->setArgusBuffer(buffers[i].get());

        if (!interface_cast<IEGLImageBuffer>(buffers[i]))
            ORIGINATE_ERROR("Failed to create Buffer");
        if (iBufferOutputStream->releaseBuffer(buffers[i].get()) != STATUS_OK)
            ORIGINATE_ERROR("Failed to release Buffer for capture use");
    }

    /* Launch the FrameConsumer thread to consume frames from the OutputStream */
    ConsumerThread frameConsumerThread(outputStream.get());
    PROPAGATE_ERROR(frameConsumerThread.initialize());

    /* Wait until the consumer is connected to the stream */
    PROPAGATE_ERROR(frameConsumerThread.waitRunning());

    /* Create capture request and enable output stream */
    UniqueObj<Request> request(iCaptureSession->createRequest());
    IRequest *iRequest = interface_cast<IRequest>(request);
    if (!iRequest)
        ORIGINATE_ERROR("Failed to create Request");
    iRequest->enableOutputStream(outputStream.get());

    ISourceSettings *iSourceSettings = interface_cast<ISourceSettings>(iRequest->getSourceSettings());
    if (!iSourceSettings)
        ORIGINATE_ERROR("Failed to get ISourceSettings interface");
    
    /* Set sensor mode if explicitly selected */
    if (SENSOR_MODE_INDEX >= 0) {
        std::vector<SensorMode*> sensorModes;
        iCameraProperties->getBasicSensorModes(&sensorModes);
        if (SENSOR_MODE_INDEX < (int)sensorModes.size()) {
            SensorMode *sensorMode = sensorModes[SENSOR_MODE_INDEX];
            iSourceSettings->setSensorMode(sensorMode);
            printf("Set sensor mode to index %d\n", SENSOR_MODE_INDEX);
        }
    }
    
    /* Set frame duration based on target FPS or use default */
    uint32_t fps = (TARGET_FPS > 0) ? TARGET_FPS : DEFAULT_FPS;
    uint64_t frameDuration = 1000000000ULL / fps;  // Convert FPS to nanoseconds
    iSourceSettings->setFrameDurationRange(Range<uint64_t>(frameDuration));
    
    printf("Setting frame duration to %lu ns (%d fps)\n", frameDuration, fps);

    /* Initialize RTSP server if enabled (after camera is ready) */
    printf("DEBUG: RTSP_SERVER flag = %s\n", RTSP_SERVER ? "true" : "false");
    if (RTSP_SERVER) {
        printf("DEBUG: Initializing RTSP server on port %d\n", RTSP_PORT);
        if (!rtsp_server.start(RTSP_PORT)) {
            printf("ERROR: Failed to initialize RTSP server on port %d\n", RTSP_PORT);
            ORIGINATE_ERROR("Failed to initialize RTSP server");
        }
        printf("DEBUG: RTSP server started successfully\n");
    } else {
        printf("DEBUG: RTSP server disabled\n");
    }

    /* Submit capture requests */
    printf("Submitting capture requests...\n");
    if (iCaptureSession->repeat(request.get()) != STATUS_OK)
        ORIGINATE_ERROR("Failed to start repeat capture request");
    printf("Capture requests submitted successfully\n");
    printf("Frame consumer thread error state: %s\n", frameConsumerThread.isInError() ? "ERROR" : "OK");

    /* Wait for CAPTURE_TIME seconds (0 = continuous) */
    if (CAPTURE_TIME > 0) {
        for (int i = 0; i < CAPTURE_TIME && !frameConsumerThread.isInError(); i++)
            sleep(1);
    } else {
    /* Continuous streaming - wait until error or signal */
    printf("Starting continuous capture loop...\n");
    while (!frameConsumerThread.isInError()) {
        sleep(1);
        static int loop_count = 0;
        if (++loop_count % 5 == 0) {
            printf("Capture loop running... (frameConsumerThread error: %s)\n", 
                   frameConsumerThread.isInError() ? "YES" : "NO");
        }
    }
    printf("Capture loop exited due to error\n");
    }

    /* Stop the repeating request and wait for idle */
    iCaptureSession->stopRepeat();
    iBufferOutputStream->endOfStream();
    iCaptureSession->waitForIdle();

    /* Wait for the consumer thread to complete */
    PROPAGATE_ERROR(frameConsumerThread.shutdown());

    /* Destroy the output stream to end the consumer thread */
    outputStream.reset();

    /* Destroy the EGLImages */
    for (uint32_t i = 0; i < NUM_BUFFERS; i++)
        NvBufSurfaceUnMapEglImage (surf[i], 0);

    /* Destroy the native buffers */
    
    /* Cleanup EGL resources */
    cleanupEGL();
    
    /* Cleanup UDP resources */
    cleanupUDP();
    
    /* Cleanup RTSP resources */
    // RTSP cleanup is handled by RTSPServer destructor
    
    return true;
}

/**
 * Modify PPS ID in the bitstream to always be 0
 * This ensures consistent PPS ID for client compatibility
 */
/**
 * Extract SPS ID from SPS NAL unit using Exp-Golomb decoding
 */
static uint8_t extractSPSId(const char* sps_data, size_t sps_size) {
    if (!sps_data || sps_size < 2) {
        return 0;
    }
    
    // Skip the NAL header (1 byte) and decode the SPS ID
    const uint8_t* data_ptr = (const uint8_t*)sps_data + 1;
    size_t remaining_bytes = sps_size - 1;
    
    // Debug: Print first few bytes of SPS data
    printf("SPS raw data (first 8 bytes): ");
    for (size_t i = 0; i < std::min(remaining_bytes, (size_t)8); i++) {
        printf("%02X ", data_ptr[i]);
    }
    printf("\n");
    
    // Exp-Golomb decoder for SPS ID
    uint32_t bit_pos = 0;
    uint32_t leading_zeros = 0;
    
    // Count leading zeros
    while (bit_pos < remaining_bytes * 8) {
        uint32_t byte_idx = bit_pos / 8;
        uint32_t bit_idx = bit_pos % 8;
        
        if (byte_idx >= remaining_bytes) break;
        
        uint8_t bit = (data_ptr[byte_idx] >> (7 - bit_idx)) & 1;
        if (bit == 0) {
            leading_zeros++;
            bit_pos++;
        } else {
            break;
        }
    }
    
    printf("SPS Leading zeros: %u, bit_pos: %u\n", leading_zeros, bit_pos);
    
    // Read the value (leading_zeros + 1 bits)
    uint32_t value = 0;
    for (uint32_t i = 0; i <= leading_zeros; i++) {
        uint32_t byte_idx = bit_pos / 8;
        uint32_t bit_idx = bit_pos % 8;
        
        if (byte_idx >= remaining_bytes) break;
        
        uint8_t bit = (data_ptr[byte_idx] >> (7 - bit_idx)) & 1;
        value = (value << 1) | bit;
        bit_pos++;
    }
    
    printf("SPS Raw value: %u\n", value);
    uint8_t sps_id = value - 1; // Exp-Golomb values are offset by 1
    printf("Final SPS ID: %u\n", sps_id);
    
    return sps_id;
}

/**
 * Send SPS/PPS parameter sets before IDR frames
 */
static void sendParameterSetsBeforeIDR() {
    if (latest_sps_map.empty() || latest_pps_map.empty()) {
        printf("WARNING: No parameter sets available to send before IDR\n");
        return;
    }

    // Create STAP-A packet with all parameter sets
    char stap_packet[1500];
    size_t packet_size;
    createSTAPAPacket(stap_packet, &packet_size, latest_sps_map, latest_pps_map);

    if (packet_size > 0) {
        sendRTPPacketToAllClients(stap_packet, packet_size);
        //printf("IDR STAP-A packet sent: %zu bytes\n", packet_size);

        // Also send via RTSP server if enabled
        if (RTSP_SERVER) {
            rtsp_server.sendRTPPacket(stap_packet, packet_size);
        }
    }
}

/**
 * Send parameter sets periodically for late joiners and loss recovery
 */
static void sendParameterSetsPeriodically() {
    if (latest_sps_map.empty() || latest_pps_map.empty()) {
        return;
    }

    // Create STAP-A packet with all parameter sets
    char stap_packet[1500];
    size_t packet_size;
    createSTAPAPacket(stap_packet, &packet_size, latest_sps_map, latest_pps_map);

    if (packet_size > 0) {
        sendRTPPacketToAllClients(stap_packet, packet_size);
        //printf("Periodic STAP-A packet sent: %zu bytes\n", packet_size);

        // Also send via RTSP server if enabled
        if (RTSP_SERVER) {
            rtsp_server.sendRTPPacket(stap_packet, packet_size);
        }
    }
}

/**
 * Update RTSP server SDP with latest parameter sets
 */
static void updateRTSPWithLatestParameterSets() {
    if (latest_sps_map.empty() || latest_pps_map.empty()) {
        printf("RTSP: No parameter sets available for SDP update\n");
        return;
    }
    
    // Use the first SPS for SDP (typically there's only one SPS)
    const SPSData& first_sps = latest_sps_map.begin()->second;
    
    if (RTSP_SERVER && first_sps.data && first_sps.size > 0) {
        printf("Updating RTSP server SDP with latest parameter sets\n");
        rtsp_server.updateSPSPPSData(first_sps.data, first_sps.size, latest_pps_map);
    }
}

}; /* namespace ArgusSamples */

static void printHelp()
{
    printf("Usage: csi_camera_h264_encode [OPTIONS]\n"
           "Options:\n"
           "  -r        Set output resolution WxH [Default 1920x1080]\n"
           "  -f        Set output filename [Default output.h264]\n"
           "  -t        Set encoder type H264 or H265 [Default H264]\n"
           "  -d        Set capture duration [Default 10 seconds]\n"
           "  -i        Set camera index [Default 0]\n"
           "  -m        Set sensor mode index [Default auto-select based on resolution]\n"
           "  -F        Set target FPS [Default use sensor mode default FPS]\n"
           "  -H        Enable headless mode (no display required) [Default disabled]\n"
           "  -s        Enable profiling\n"
           "  -v        Enable verbose message\n"
           "  -u        Enable UDP streaming to 192.254.254.99:5000\n"
           "  -U HOST   Set UDP host address [Default 192.254.254.99]\n"
           "  -p PORT   Set UDP port [Default 5000]\n"
           "  -R        Enable RTSP server on port 8554\n"
           "  -P PORT   Set RTSP server port [Default 8554]\n"
           "  -l        List all available cameras and sensor modes\n"
           "  -w        Raw frame mode (no encoding, no network, just capture)\n"
           "  -h        Print this help\n");
}

static bool parseCmdline(int argc, char **argv)
{
    int c, w, h;
    bool haveFilename = false;
    while ((c = getopt(argc, argv, "r:f:t:d:i:m:F:Hs::v::uU:p:RP:lwh")) != -1)
    {
        switch (c)
        {
            case 'r':
                if (sscanf(optarg, "%dx%d", &w, &h) != 2)
                {
                    printf("Invalid resolution format: %s\n", optarg);
                    return false;
                }
                STREAM_SIZE = Size2D<uint32_t>(w, h);
                break;
            case 'f':
                OUTPUT_FILENAME = optarg;
                haveFilename = true;
                break;
            case 't':
                if (strcmp(optarg, "H264") == 0)
                    ENCODER_PIXFMT = V4L2_PIX_FMT_H264;
                else if (strcmp(optarg, "H265") == 0)
                    ENCODER_PIXFMT = V4L2_PIX_FMT_H265;
                else
                {
                    printf("Invalid encoder type: %s\n", optarg);
                    return false;
                }
                break;
            case 'd':
                CAPTURE_TIME = atoi(optarg);
                break;
            case 'i':
                CAMERA_INDEX = atoi(optarg);
                break;
            case 'm':
                SENSOR_MODE_INDEX = atoi(optarg);
                break;
            case 'F':
                TARGET_FPS = atoi(optarg);
                break;
            case 'H':
                HEADLESS_MODE = true;
                break;
            case 's':
                DO_STAT = true;
                break;
            case 'v':
                VERBOSE_ENABLE = true;
                break;
            case 'u':
                UDP_STREAMING = true;
                break;
            case 'U':
                UDP_HOST = optarg;
                UDP_STREAMING = true;
                break;
            case 'p':
                UDP_PORT = atoi(optarg);
                UDP_STREAMING = true;
                break;
            case 'R':
                RTSP_SERVER = true;
                break;
            case 'P':
                RTSP_PORT = atoi(optarg);
                RTSP_SERVER = true;
                break;
            case 'l':
                LIST_CAMERAS = true;
                break;
            case 'w':
                RAW_FRAME_MODE = true;
                break;
            case 'h':
                return false;
                break;
            default:
                return false;
        }
    }
    return true;
}

int main(int argc, char *argv[])
{
    if (!parseCmdline(argc, argv))
    {
        printHelp();
        return EXIT_FAILURE;
    }

    // If camera listing is requested, show cameras and exit
    if (LIST_CAMERAS) {
        if (!ArgusSamples::listAvailableCameras()) {
            printf("Failed to list cameras\n");
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    NvApplicationProfiler &profiler = NvApplicationProfiler::getProfilerInstance();

    printf("Starting CSI Camera H.264 Encoder\n");
    printf("Resolution: %dx%d\n", STREAM_SIZE.width(), STREAM_SIZE.height());
    printf("Duration: %d seconds\n", CAPTURE_TIME);
    printf("Camera index: %d\n", CAMERA_INDEX);
    printf("Headless mode: %s\n", HEADLESS_MODE ? "enabled" : "disabled");
    if (UDP_STREAMING) {
        printf("UDP streaming: enabled to %s:%d\n", UDP_HOST.c_str(), UDP_PORT);
    } else {
        printf("UDP streaming: disabled\n");
    }
    if (RTSP_SERVER) {
        printf("RTSP server: enabled on port %d\n", RTSP_PORT);
    } else {
        printf("RTSP server: disabled\n");
    }

    if (!ArgusSamples::execute())
        return EXIT_FAILURE;

    profiler.stop();
    profiler.printProfilerData(std::cout);

    return EXIT_SUCCESS;
}
