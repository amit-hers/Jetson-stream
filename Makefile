# Makefile for Enhanced Camera Client-Server System
# Builds separate executables for client and server

CC = g++
CFLAGS = -Wall -Wextra -std=c++11 -O2
LDFLAGS = -lpthread

# NVIDIA libraries for server (camera capture)
NVIDIA_CFLAGS = -I/usr/include/argus -I/usr/include/nvidia
NVIDIA_LDFLAGS = -largus -lnvargus -lnvbuf_utils -lEGL -lGLESv2

# Source files
SERVER_SOURCES = src/enhanced_camera_server.cpp
CLIENT_SOURCES = src/enhanced_camera_client.cpp
MAIN_SOURCES = src/main.cpp

# Object files
SERVER_OBJECTS = $(SERVER_SOURCES:.cpp=.o)
CLIENT_OBJECTS = $(CLIENT_SOURCES:.cpp=.o)
MAIN_OBJECTS = $(MAIN_SOURCES:.cpp=.o)

# Executables
SERVER_EXEC = camera_server
CLIENT_EXEC = camera_client
MAIN_EXEC = camera_main

# Default target
all: $(SERVER_EXEC) $(CLIENT_EXEC)

# Build camera server (with NVIDIA libraries)
$(SERVER_EXEC): $(SERVER_OBJECTS)
	$(CC) $(SERVER_OBJECTS) -o $@ $(LDFLAGS) $(NVIDIA_LDFLAGS)
	@echo "Built camera server: $@"

# Build camera client (standalone)
$(CLIENT_EXEC): $(CLIENT_OBJECTS)
	$(CC) $(CLIENT_OBJECTS) -o $@ $(LDFLAGS)
	@echo "Built camera client: $@"

# Build main camera application (with NVIDIA libraries)
$(MAIN_EXEC): $(MAIN_OBJECTS)
	$(CC) $(MAIN_OBJECTS) -o $@ $(LDFLAGS) $(NVIDIA_LDFLAGS)
	@echo "Built main camera application: $@"

# Compile server sources with NVIDIA includes
src/enhanced_camera_server.o: src/enhanced_camera_server.cpp
	$(CC) $(CFLAGS) $(NVIDIA_CFLAGS) -c $< -o $@

# Compile client sources (no NVIDIA libraries needed)
src/enhanced_camera_client.o: src/enhanced_camera_client.cpp
	$(CC) $(CFLAGS) -c $< -o $@

# Compile main sources with NVIDIA includes
src/main.o: src/main.cpp
	$(CC) $(CFLAGS) $(NVIDIA_CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(SERVER_OBJECTS) $(CLIENT_OBJECTS) $(MAIN_OBJECTS)
	rm -f $(SERVER_EXEC) $(CLIENT_EXEC) $(MAIN_EXEC)
	@echo "Cleaned build artifacts"

# Install executables to /usr/local/bin
install: all
	sudo cp $(SERVER_EXEC) /usr/local/bin/
	sudo cp $(CLIENT_EXEC) /usr/local/bin/
	sudo cp $(MAIN_EXEC) /usr/local/bin/
	@echo "Installed executables to /usr/local/bin"

# Uninstall executables
uninstall:
	sudo rm -f /usr/local/bin/$(SERVER_EXEC)
	sudo rm -f /usr/local/bin/$(CLIENT_EXEC)
	sudo rm -f /usr/local/bin/$(MAIN_EXEC)
	@echo "Uninstalled executables"

# Create named pipes for testing
pipes:
	mkfifo -m 666 /tmp/camera_raw_pipe
	mkfifo -m 666 /tmp/camera_h264_pipe
	mkfifo -m 666 /tmp/camera_control_pipe
	@echo "Created named pipes"

# Remove named pipes
clean-pipes:
	rm -f /tmp/camera_raw_pipe /tmp/camera_h264_pipe /tmp/camera_control_pipe
	@echo "Removed named pipes"

# Test the client-server communication
test: all pipes
	@echo "Starting camera server..."
	./$(SERVER_EXEC) &
	SERVER_PID=$$!; \
	sleep 2; \
	echo "Starting camera client (raw mode)..."; \
	./$(CLIENT_EXEC) raw & \
	CLIENT_PID=$$!; \
	sleep 10; \
	echo "Stopping test..."; \
	kill $$CLIENT_PID $$SERVER_PID 2>/dev/null; \
	wait

# Help target
help:
	@echo "Available targets:"
	@echo "  all        - Build both client and server executables"
	@echo "  $(SERVER_EXEC) - Build camera server executable"
	@echo "  $(CLIENT_EXEC) - Build camera client executable"
	@echo "  $(MAIN_EXEC)   - Build main camera application"
	@echo "  clean      - Remove build artifacts"
	@echo "  install    - Install executables to /usr/local/bin"
	@echo "  uninstall  - Remove executables from /usr/local/bin"
	@echo "  pipes      - Create named pipes for communication"
	@echo "  clean-pipes- Remove named pipes"
	@echo "  test       - Run a test of client-server communication"
	@echo "  help       - Show this help message"

.PHONY: all clean install uninstall pipes clean-pipes test help

