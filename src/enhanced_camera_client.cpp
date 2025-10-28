/*
 * Enhanced Camera Client - Inspired by V4L2 patterns
 * 
 * This client can connect to the enhanced camera server via network
 * and receive frames with proper deserialization.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static bool g_doExit = false;

/* FPS monitoring for client */
static uint64_t g_framesReceived = 0;
static time_t g_clientStartTime = 0;
static time_t g_clientLastFpsReport = 0;
static time_t g_lastFrameTime = 0;

/* Print client FPS statistics */
static void printClientFpsStats(const char* frameType)
{
    time_t currentTime = time(NULL);
    
    if (g_clientStartTime == 0) {
        g_clientStartTime = currentTime;
        g_clientLastFpsReport = currentTime;
        return;
    }
    
    if (currentTime - g_clientLastFpsReport >= 5) { // Report every 5 seconds
        double elapsed = difftime(currentTime, g_clientStartTime);
        double fps = (elapsed > 0) ? g_framesReceived / elapsed : 0;
        time_t timeSinceLastFrame = g_lastFrameTime > 0 ? currentTime - g_lastFrameTime : -1;
        
        // Calculate bandwidth if we have frames
        double bandwidthMBps = 0;
        if (g_framesReceived > 0 && elapsed > 0) {
            // Estimate frame size based on frame type
            size_t estimatedFrameSize = 0;
            if (strcmp(frameType, "RAW") == 0) {
                // Assume 640x480 for now, could be made configurable
                estimatedFrameSize = 640 * 480 * 3 / 2; // YUV420
            } else {
                // H.264 frames are typically smaller
                estimatedFrameSize = 5000; // ~5KB average
            }
            size_t totalBytes = g_framesReceived * estimatedFrameSize;
            bandwidthMBps = (totalBytes / (1024.0 * 1024.0)) / elapsed;
        }
        
        printf("\n=== CAMERA CLIENT STATISTICS (%s) (%.1fs elapsed) ===\n", frameType, elapsed);
        printf("📥 Reception Statistics:\n");
        printf("   Frames received: %lu\n", g_framesReceived);
        printf("   Reception rate: %.1f fps\n", fps);
        printf("   Estimated bandwidth: %.2f MB/s\n", bandwidthMBps);
        
        printf("\n⏱️  Timing Information:\n");
        printf("   Client status: %s\n", g_doExit ? "🔄 EXITING" : "✅ RUNNING");
        if (timeSinceLastFrame >= 0) {
            printf("   Time since last frame: %ld seconds\n", timeSinceLastFrame);
            if (timeSinceLastFrame > 2) {
                printf("   ⚠️  WARNING: No frames received recently!\n");
            }
        } else {
            printf("   No frames received yet\n");
        }
        
        printf("\n📊 Performance Metrics:\n");
        if (fps > 0) {
            double avgFrameInterval = elapsed / g_framesReceived;
            printf("   Average frame interval: %.3f seconds\n", avgFrameInterval);
            if (fps < 25) {
                printf("   ⚠️  WARNING: Low frame rate detected!\n");
            } else if (fps > 35) {
                printf("   ⚡ High frame rate - excellent performance!\n");
            }
        }
        printf("====================================================\n\n");
        
        g_clientLastFpsReport = currentTime;
    }
}

void signalHandler(int signal)
{
    printf("\nReceived signal %d, exiting...\n", signal);
    g_doExit = true;
}

void printUsage(const char* program)
{
    printf("Usage: %s <mode> [options]\n", program);
    printf("Enhanced Camera Client\n\n");
    printf("Modes:\n");
    printf("  raw          Read raw frames from named pipe\n");
    printf("  h264         Read H.264 encoded frames from named pipe\n");
    printf("  network      Connect to camera server via network\n");
    printf("\nGeneral options:\n");
    printf("  -f <file>    Save frames to file (optional)\n");
    printf("  -o           Overwrite existing file (default: append)\n");
    printf("\nNetwork options:\n");
    printf("  -h <host>    Server host (default: localhost)\n");
    printf("  -p <port>    Server port (default: 8080)\n");
    printf("\nExamples:\n");
    printf("  %s raw\n", program);
    printf("  %s raw -f raw_frames.yuv\n", program);
    printf("  %s h264 -f video.h264\n", program);
    printf("  %s network -h 192.168.1.100 -p 8080 -f network_stream.h264\n", program);
}

void readRawFrames(const char* outputFile = nullptr, bool overwrite = false)
{
    printf("=== CLIENT RAW FRAME READER STARTING ===\n");
    printf("Attempting to connect to /tmp/camera_raw_pipe...\n");
    
    int pipeFd = open("/tmp/camera_raw_pipe", O_RDONLY);
    if (pipeFd < 0) {
        printf("ERROR: Failed to open raw pipe: %s\n", strerror(errno));
        printf("Make sure the camera server is running!\n");
        return;
    }
    
    printf("Connected to raw pipe\n");
    printf("Waiting for frames...\n");
    
    // Open output file if specified
    FILE* fileHandle = nullptr;
    if (outputFile) {
        const char* mode = overwrite ? "wb" : "ab";
        fileHandle = fopen(outputFile, mode);
        if (!fileHandle) {
            printf("ERROR: Failed to open output file '%s': %s\n", outputFile, strerror(errno));
            close(pipeFd);
            return;
        }
        printf("Saving raw frames to: %s (%s)\n", outputFile, overwrite ? "overwrite" : "append");
    }
    
    uint32_t frameCount = 0;
    while (!g_doExit) {
        // Read frame header (frameNumber, width, height, timestamp)
        uint32_t header[4];
        ssize_t bytes = read(pipeFd, header, sizeof(header));
        if (bytes != sizeof(header)) {
            if (bytes == 0) {
                printf("WARNING: Pipe closed by server (EOF received)\n");
                break;
            }
            printf("ERROR: Failed to read frame header: %ld/%zu bytes, errno: %s\n", 
                   bytes, sizeof(header), strerror(errno));
            break;
        }
        
        uint32_t frameNumber = ntohl(header[0]);
        uint32_t width = ntohl(header[1]);
        uint32_t height = ntohl(header[2]);
        uint32_t timestamp = ntohl(header[3]);
        
        // Check if there's additional data (YUV) after header
        // The server may send header-only frames for now
        uint8_t* yuvData = nullptr;
        uint32_t yuvDataSize = 0;
        
        // Try to peek at what's available
        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(pipeFd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000; // 1ms timeout
        
        int ready = select(pipeFd + 1, &readfds, NULL, NULL, &timeout);
        if (ready > 0) {
            // There's more data available
            yuvDataSize = width * height * 3 / 2; // YUV420 size
            yuvData = (uint8_t*)malloc(yuvDataSize);
            size_t remaining = yuvDataSize;
            uint8_t* data_ptr = yuvData;
            bool read_success = true;
            
            while (remaining > 0 && read_success) {
                size_t chunk_size = (remaining > 65536) ? 65536 : remaining; // 64KB chunks
                ssize_t bytes = read(pipeFd, data_ptr, chunk_size);
                
                if (bytes < 0) {
                    printf("ERROR: Failed to read YUV data chunk: %s\n", strerror(errno));
                    read_success = false;
                    break;
                }
                
                if (bytes == 0) {
                    // No more data, probably header-only frame
                    yuvDataSize -= remaining;
                    break;
                }
                
                remaining -= bytes;
                data_ptr += bytes;
            }
            
            if (remaining > 0 && remaining != yuvDataSize) {
                printf("WARNING: Received partial YUV data (%zu bytes remaining)\n", remaining);
            }
        }
        
        // If no YUV data, treat as header-only frame
        
        frameCount++;
        g_framesReceived++;
        g_lastFrameTime = time(NULL);
        
        // Print FPS stats every 5 seconds
        printClientFpsStats("RAW");
        
        // Save frame data to file if specified
        if (fileHandle && yuvData && yuvDataSize > 0) {
            // Write frame header
            fwrite(header, sizeof(header), 1, fileHandle);
            // Write YUV data
            fwrite(yuvData, yuvDataSize, 1, fileHandle);
            fflush(fileHandle);
        }
        
        printf("CLIENT: Received frame %u - %dx%d, timestamp %u%s\n", 
               frameNumber, width, height, timestamp,
               yuvDataSize > 0 ? " [with YUV data]" : " [header only]");
        
        if (yuvData) {
            free(yuvData);
        }
        
        // Continue receiving frames indefinitely
    }
    
    close(pipeFd);
    if (fileHandle) {
        fclose(fileHandle);
        printf("Raw frames saved to: %s\n", outputFile);
    }
    printf("CLIENT: Raw frame reader stopped\n");
}

void readH264Frames(const char* outputFile = nullptr, bool overwrite = false)
{
    printf("=== CLIENT H.264 FRAME READER STARTING ===\n");
    printf("Attempting to connect to /tmp/camera_h264_pipe...\n");
    
    int pipeFd = open("/tmp/camera_h264_pipe", O_RDONLY);
    if (pipeFd < 0) {
        printf("ERROR: Failed to open H.264 pipe: %s\n", strerror(errno));
        printf("Make sure the camera server is running!\n");
        return;
    }
    
    printf("Connected to H.264 pipe\n");
    printf("Waiting for frames...\n");
    
    // Open output file if specified
    FILE* fileHandle = nullptr;
    if (outputFile) {
        const char* mode = overwrite ? "wb" : "ab";
        fileHandle = fopen(outputFile, mode);
        if (!fileHandle) {
            printf("ERROR: Failed to open output file '%s': %s\n", outputFile, strerror(errno));
            close(pipeFd);
            return;
        }
        printf("Saving H.264 frames to: %s (%s)\n", outputFile, overwrite ? "overwrite" : "append");
    }
    
    uint32_t frameCount = 0;
    
    while (!g_doExit) {
        // Read frame header (frameNumber, size, timestamp)
        uint32_t header[3];
        ssize_t bytes = read(pipeFd, header, sizeof(header));
        if (bytes != sizeof(header)) {
            if (bytes == 0) {
                printf("WARNING: Pipe closed by server (EOF received)\n");
                break;
            }
            printf("ERROR: Failed to read frame header: %ld/%zu bytes, errno: %s\n", 
                   bytes, sizeof(header), strerror(errno));
            break;
        }
        
        uint32_t frameNumber = ntohl(header[0]);
        uint32_t size = ntohl(header[1]);
        uint32_t timestamp = ntohl(header[2]);
        
        // Read frame data
        uint8_t* frameData = (uint8_t*)malloc(size);
        bytes = read(pipeFd, frameData, size);
        if (bytes != (ssize_t)size) {
            printf("ERROR: Failed to read frame data: %ld/%u bytes, errno: %s\n", 
                   bytes, size, strerror(errno));
            free(frameData);
            break;
        }
        
        frameCount++;
        g_framesReceived++;
        g_lastFrameTime = time(NULL);
        
        // Print FPS stats every 5 seconds
        printClientFpsStats("H.264");
        
        // Write to file if specified
        if (fileHandle) {
            fwrite(frameData, 1, size, fileHandle);
            fflush(fileHandle);
        }
        
        printf("CLIENT: Received H.264 frame %u - size %u, timestamp %u%s\n", 
               frameNumber, size, timestamp, fileHandle ? " [saved]" : "");
        
        free(frameData);
        
        // Continue receiving frames indefinitely
    }
    
    close(pipeFd);
    if (fileHandle) {
        fclose(fileHandle);
        printf("H.264 data saved to: %s\n", outputFile);
    }
}

void connectToNetwork(const char* host, int port, const char* outputFile = nullptr, bool overwrite = false)
{
    printf("=== CLIENT NETWORK CONNECTOR STARTING ===\n");
    printf("Attempting to connect to %s:%d...\n", host, port);
    
    // Create socket
    int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) {
        printf("ERROR: Failed to create socket: %s\n", strerror(errno));
        return;
    }
    
    // Set up server address
    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    
    // Handle localhost
    if (strcmp(host, "localhost") == 0) {
        host = "127.0.0.1";
    }
    
    if (inet_pton(AF_INET, host, &serverAddr.sin_addr) <= 0) {
        printf("ERROR: Invalid address: %s\n", host);
        close(socketFd);
        return;
    }
    
    // Connect to server
    if (connect(socketFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        printf("ERROR: Failed to connect to %s:%d: %s\n", host, port, strerror(errno));
        close(socketFd);
        return;
    }
    
    printf("Connected to server %s:%d\n", host, port);
    printf("Waiting for frames...\n");
    
    // Open output file if specified
    FILE* fileHandle = nullptr;
    if (outputFile) {
        const char* mode = overwrite ? "wb" : "ab";
        fileHandle = fopen(outputFile, mode);
        if (!fileHandle) {
            printf("ERROR: Failed to open output file '%s': %s\n", outputFile, strerror(errno));
            close(socketFd);
            return;
        }
        printf("Saving network frames to: %s (%s)\n", outputFile, overwrite ? "overwrite" : "append");
    }
    
    uint32_t frameCount = 0;
    
    while (!g_doExit) {
        // Read frame header (frameNumber, size, timestamp)
        uint32_t header[3];
        ssize_t bytes = recv(socketFd, header, sizeof(header), 0);
        if (bytes != sizeof(header)) {
            if (bytes == 0) {
                printf("WARNING: Server disconnected\n");
                break;
            }
            printf("ERROR: Failed to read frame header: %ld/%zu bytes, errno: %s\n", 
                   bytes, sizeof(header), strerror(errno));
            break;
        }
        
        uint32_t frameNumber = ntohl(header[0]);
        uint32_t h264DataSize = ntohl(header[1]);
        uint32_t timestamp = ntohl(header[2]);
        
        printf("CLIENT: Received header - frame %u, H.264 size %u, timestamp %u\n", 
               frameNumber, h264DataSize, timestamp);
        
        // Read H.264 frame data
        uint8_t* frameData = (uint8_t*)malloc(h264DataSize);
        bytes = recv(socketFd, frameData, h264DataSize, 0);
        if (bytes != (ssize_t)h264DataSize) {
            printf("ERROR: Failed to read frame data: %ld/%u bytes, errno: %s\n", 
                   bytes, h264DataSize, strerror(errno));
            free(frameData);
            break;
        }
        
        frameCount++;
        g_framesReceived++;
        g_lastFrameTime = time(NULL);
        
        // Print FPS stats every 5 seconds
        printClientFpsStats("NETWORK");
        
        // Write to file if specified
        if (fileHandle) {
            fwrite(frameData, 1, h264DataSize, fileHandle);
            fflush(fileHandle);
        }
        
        free(frameData);
        
        // Continue receiving frames indefinitely
        // Remove frame limit for continuous operation
    }
    
    close(socketFd);
    if (fileHandle) {
        fclose(fileHandle);
        printf("Network data saved to: %s\n", outputFile);
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Parse common options
    const char* outputFile = nullptr;
    bool overwrite = false;
    const char* host = "localhost";
    int port = 8080;
    
    // Parse all options
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 < argc) {
                outputFile = argv[i + 1];
                i++; // Skip next argument
            } else {
                printf("ERROR: -f requires a filename\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0) {
            overwrite = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            if (i + 1 < argc) {
                host = argv[i + 1];
                i++; // Skip next argument
            } else {
                printf("ERROR: -h requires a hostname\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-p") == 0) {
            if (i + 1 < argc) {
                port = atoi(argv[i + 1]);
                i++; // Skip next argument
            } else {
                printf("ERROR: -p requires a port number\n");
                return 1;
            }
        }
    }
    
    if (strcmp(argv[1], "raw") == 0) {
        readRawFrames(outputFile, overwrite);
    } else if (strcmp(argv[1], "h264") == 0) {
        readH264Frames(outputFile, overwrite);
    } else if (strcmp(argv[1], "network") == 0) {
        connectToNetwork(host, port, outputFile, overwrite);
    } else {
        printUsage(argv[0]);
        return 1;
    }
    
    return 0;
}
