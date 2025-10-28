# Enhanced Camera Client-Server System

This project provides a robust client-server system for streaming camera data through named pipes. The system consists of three main components:

1. **Camera Server** (`camera_server`) - Captures camera data and streams it through named pipes
2. **Camera Client** (`camera_client`) - Receives camera data from named pipes
3. **Main Camera Application** (`camera_main`) - Full-featured camera application with encoding

## Features

- **Raw Image Streaming**: Stream raw YUV420 camera frames through named pipes
- **H.264 Streaming**: Stream encoded H.264 video through named pipes
- **Network Streaming**: Optional TCP network streaming support
- **Robust Error Handling**: Proper client disconnection detection and recovery
- **Thread-Safe Communication**: Multi-threaded architecture with proper synchronization
- **FPS Monitoring**: Real-time frame rate statistics

## Building

### Prerequisites

- NVIDIA Jetson platform with Argus SDK
- GCC/G++ compiler
- pthread library
- EGL/OpenGL ES libraries

### Build Commands

```bash
# Build all executables
make all

# Build specific components
make camera_server    # Camera server only
make camera_client    # Camera client only
make camera_main      # Main camera application

# Clean build artifacts
make clean
```

## Usage

### 0. Rtsp server
./csi_camera_h264_encode -i 0 -r 1280x720 -f 30 -H -d 0

### 1. Start the Camera Server

```bash
# Start server with default settings (1280x720 @ 30fps)
./camera_server

# Start server with custom settings
./camera_server -r 1920x1080 -f 60 -i 0

# Start server with network streaming enabled
./camera_server -n -p 8080
```

**Server Options:**
- `-i <index>` - Camera index (default: 0)
- `-r <WxH>` - Resolution (default: 1280x720)
- `-f <fps>` - Frame rate (default: 30)
- `-m <mode>` - Sensor mode index (-1 = auto-select)
- `-p <port>` - Network port for TCP streaming (default: 8080)
- `-n` - Enable network streaming (TCP)
- `-H` - Enable headless mode
- `-h` - Print help

### 2. Start the Camera Client

```bash
# Connect to raw image stream
./camera_client raw

# Connect to H.264 stream
./camera_client h264

# Connect via network
./camera_client network -h localhost -p 8080
```

**Client Options:**
- `raw` - Read raw YUV420 frames from named pipe
- `h264` - Read H.264 encoded frames from named pipe
- `network` - Connect to camera server via network
- `-h <host>` - Server host (default: localhost)
- `-p <port>` - Server port (default: 8080)

### 3. Named Pipes

The system uses the following named pipes:
- `/tmp/camera_raw_pipe` - Raw YUV420 frame data
- `/tmp/camera_h264_pipe` - H.264 encoded frame data
- `/tmp/camera_control_pipe` - Control commands (future use)

Create pipes manually:
```bash
make pipes
```

Remove pipes:
```bash
make clean-pipes
```

## Frame Protocol

### Raw Frame Format
```
Header (16 bytes):
- frameNumber (4 bytes, network byte order)
- width (4 bytes, network byte order)
- height (4 bytes, network byte order)
- timestamp (4 bytes, network byte order)

Data:
- YUV420 data (width * height * 3/2 bytes)
```

### H.264 Frame Format
```
Header (12 bytes):
- frameNumber (4 bytes, network byte order)
- size (4 bytes, network byte order)
- timestamp (4 bytes, network byte order)

Data:
- H.264 encoded frame data (size bytes)
```

## Testing

Run a complete test:
```bash
make test
```

This will:
1. Start the camera server
2. Start a camera client in raw mode
3. Run for 10 seconds
4. Stop both processes

## Architecture

### Server Architecture
- **Capture Thread**: Generates camera frames (currently dummy data)
- **Raw Writer Thread**: Writes raw frames to `/tmp/camera_raw_pipe`
- **H.264 Writer Thread**: Writes H.264 frames to `/tmp/camera_h264_pipe`
- **Network Thread**: Handles TCP network clients (if enabled)
- **Control Thread**: Handles control commands (future)

### Client Architecture
- **Raw Reader**: Reads raw frames from `/tmp/camera_raw_pipe`
- **H.264 Reader**: Reads H.264 frames from `/tmp/camera_h264_pipe`
- **Network Client**: Connects to server via TCP (if enabled)

## Error Handling

The system includes robust error handling:

- **Client Disconnection**: Server detects broken pipes and reconnects when clients reconnect
- **Partial Reads/Writes**: Proper handling of partial data transfers
- **Pipe Creation**: Automatic pipe creation and cleanup
- **Signal Handling**: Graceful shutdown on SIGINT/SIGTERM

## Performance

- **Low Latency**: Direct pipe communication minimizes latency
- **High Throughput**: Optimized for high frame rates (up to 60fps)
- **Memory Efficient**: Streaming architecture with minimal buffering
- **Thread Safe**: Proper synchronization prevents data corruption

## Troubleshooting

### Common Issues

1. **"Failed to open pipe"**
   - Ensure server is running before starting client
   - Check pipe permissions (should be 666)

2. **"No cameras available"**
   - Verify camera hardware is connected
   - Check camera permissions
   - Try different camera index (-i option)

3. **"Failed to create socket"**
   - Check if port is already in use
   - Verify network permissions

### Debug Mode

Enable verbose output:
```bash
./camera_server -v
./camera_client raw
```

## Future Enhancements

- Real camera integration (currently uses dummy data)
- Control protocol for camera settings
- Multiple client support
- Frame compression options
- Web-based client interface
- Recording functionality

## License

This project is provided as-is for educational and development purposes.
