#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string>
#include <map>
#include <vector>

// Base64 encoding function
std::string base64_encode(const char* data, size_t size) {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((size + 2) / 3) * 4);
    
    for (size_t i = 0; i < size; i += 3) {
        uint32_t value = 0;
        int padding = 0;
        
        for (int j = 0; j < 3; j++) {
            value <<= 8;
            if (i + j < size) {
                value |= (unsigned char)data[i + j];
            } else {
                padding++;
            }
        }
        
        for (int j = 0; j < 4; j++) {
            if (j < 4 - padding) {
                result += chars[(value >> (6 * (3 - j))) & 0x3F];
            } else {
                result += '=';
            }
        }
    }
    
    return result;
}

// RTSP Server implementation
#include "rtsp_server.h"

// ClientHandler class implementation
class ClientHandler {
private:
    RTSPServer* server;
    int client_socket;
    struct sockaddr_in client_addr;
    RTSPServer::ClientSession session;
    
public:
    ClientHandler(RTSPServer* srv, int socket, struct sockaddr_in addr) 
        : server(srv), client_socket(socket), client_addr(addr) {
        session.rtsp_socket = socket;
        session.client_addr = addr;
        session.playing = false;
        session.session_id = generateSessionId();
        session.rtp_port = 0;  // Will be set during SETUP
        session.rtcp_port = 0;  // Will be set during SETUP
        session.rtp_socket = -1;
    }
    
    void run();
    std::string generateDescribeResponse(const std::string& cseq);
    std::string generateSetupResponse(const std::string& cseq, const std::string& url, const std::string& request);
    std::string generatePlayResponse(const std::string& cseq);
    std::string generateTeardownResponse(const std::string& cseq);
    
private:
    std::string processRequest(const std::string& request);
    std::string extractMethod(const std::string& request);
    std::string extractCSeq(const std::string& request);
    std::string generateSessionId();
    std::string getCurrentTimeString();
};

// RTSPServer class implementation
RTSPServer::RTSPServer(int port) : server_port(port), running(false) {
    pthread_mutex_init(&clients_mutex, NULL);
    generateSDP();
    last_keepalive_check = time(NULL);
}

RTSPServer::~RTSPServer() {
    pthread_mutex_destroy(&clients_mutex);
}

void RTSPServer::generateSDP() {
    // Use the known external IP address for the server
    char server_ip[INET_ADDRSTRLEN] = "192.0.50.30";
    
    printf("RTSP: Generated SDP with server IP: %s\n", server_ip);
    
    // Generate RFC 4566 compliant SDP content for H.264 stream
    // Session ID and version (using timestamp for uniqueness)
    time_t now = time(NULL);
    std::string session_id = std::to_string(now);
    std::string session_version = std::to_string(now);
    
    // For now, use default sprop-parameter-sets
    // The actual SPS/PPS will be sent via RTP packets
    std::string sprop_parameter_sets = "Z0IAHpWoKA9puAgICBA=,aM48gA==";
    printf("RTSP: Using default sprop-parameter-sets\n");
    
    sdp_content = 
        // Protocol Version (required)
        "v=0\r\n"
        // Origin (required): username, session-id, session-version, nettype, addrtype, unicast-address
        "o=- " + session_id + " " + session_version + " IN IP4 " + std::string(server_ip) + "\r\n"
        // Session Name (required)
        "s=CSI Camera H.264 Stream\r\n"
        // Session Information (optional but recommended)
        "i=Live H.264 video stream from CSI camera\r\n"
        // URI (optional)
        "u=http://" + std::string(server_ip) + ":8554/stream\r\n"
        // Email Address (optional)
        "e=admin@" + std::string(server_ip) + "\r\n"
        // Phone Number (optional)
        "p=+1-555-000-0000\r\n"
        // Connection Information (required if not in media description)
        "c=IN IP4 " + std::string(server_ip) + "\r\n"
        // Bandwidth Information (optional)
        "b=AS:2000\r\n"
        "b=TIAS:2000000\r\n"
        // Time Description (required)
        "t=0 0\r\n"
        // Repeat Times (optional)
        "r=604800 3600 0 90000\r\n"
        // Timezone (optional)
        "z=2882844526 -1h 2898848070 0\r\n"
        // Encryption Keys (optional)
        "k=clear:password\r\n"
        // Attributes (optional)
        "a=recvonly\r\n"
        "a=type:broadcast\r\n"
        "a=charset:UTF-8\r\n"
        "a=sdplang:en\r\n"
        "a=lang:en\r\n"
        "a=framerate:30\r\n"
        "a=quality:10\r\n"
        // Media Description (required)
        "m=video 0 RTP/AVP 96\r\n"
        // Media Title (optional)
        "i=Video Track\r\n"
        // Media Connection Information (optional, inherits from session)
        "c=IN IP4 " + std::string(server_ip) + "\r\n"
        // Media Bandwidth (optional)
        "b=AS:1500\r\n"
        "b=TIAS:1500000\r\n"
        // Media Attributes (required for RTP)
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42001f;sprop-parameter-sets=" + sprop_parameter_sets + "\r\n"
        "a=control:track0\r\n"
        // Additional H.264 specific attributes
        "a=framerate:30\r\n"
        "a=framesize:96 1920-1080\r\n"
        "a=cliprect:0,0,1080,1920\r\n"
        "a=orient:landscape\r\n"
        "a=type:broadcast\r\n"
        "a=tool:GStreamer\r\n"
        "a=recvonly\r\n";
}

bool RTSPServer::start() {
    printf("DEBUG: Creating RTSP socket\n");
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Failed to create RTSP socket");
        return false;
    }
    
    printf("DEBUG: Setting socket options\n");
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);
    
    printf("DEBUG: Binding to port %d\n", server_port);
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to bind RTSP socket");
        close(server_socket);
        return false;
    }
    
    printf("DEBUG: Starting to listen\n");
    if (listen(server_socket, 5) < 0) {
        perror("Failed to listen on RTSP socket");
        close(server_socket);
        return false;
    }
    
    running = true;
    printf("DEBUG: Creating server thread\n");
    if (pthread_create(&server_thread, NULL, serverThread, this) != 0) {
        perror("Failed to create server thread");
        close(server_socket);
        running = false;
        return false;
    }
    
    printf("RTSP Server started on port %d\n", server_port);
    return true;
}

bool RTSPServer::start(int port) {
    server_port = port;
    return start();
}

void RTSPServer::stop() {
    running = false;
    close(server_socket);
    pthread_join(server_thread, NULL);
    
    pthread_mutex_lock(&clients_mutex);
    for (auto& client : clients) {
        close(client.second.rtsp_socket);
        if (client.second.rtp_socket >= 0) {
            close(client.second.rtp_socket);
        }
    }
    clients.clear();
    pthread_mutex_unlock(&clients_mutex);
    
    printf("RTSP Server stopped\n");
}

bool RTSPServer::isRunning() const {
    return running;
}

void RTSPServer::sendRTPPacket(const char* rtp_data, size_t rtp_size) {
    // Forward pre-packetized RTP data directly to RTSP clients
    pthread_mutex_lock(&clients_mutex);
    //printf("RTSP: sendRTPPacket called with size=%zu, clients=%zu\n", rtp_size, clients.size());
    std::vector<int> dead_clients;  // Track clients to remove
    
    // Only log if there are clients connected
    if (!clients.empty()) {
        for (auto& client : clients) {
            if (client.second.playing) {
                if (client.second.rtp_socket >= 0) {
                    // UDP transport
                    // Check if RTP socket is still valid
                    int error = 0;
                    socklen_t len = sizeof(error);
                    if (getsockopt(client.second.rtp_socket, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                        dead_clients.push_back(client.first);
                        continue;
                    }
                    
                    // Send RTP packet directly via UDP
                    ssize_t sent = sendto(client.second.rtp_socket, rtp_data, rtp_size, MSG_NOSIGNAL,
                                        (struct sockaddr*)&client.second.rtp_addr, sizeof(client.second.rtp_addr));
                    if (sent < 0 || sent != (ssize_t)rtp_size) {
                        printf("RTSP: Failed to send RTP packet to client %s:%d (sent=%zd, expected=%zu)\n",
                               inet_ntoa(client.second.rtp_addr.sin_addr), ntohs(client.second.rtp_addr.sin_port), sent, rtp_size);
                        dead_clients.push_back(client.first);
                    } else {
                        //printf("RTSP: Sent RTP packet to client %s:%d (size=%zu)\n",
                               //inet_ntoa(client.second.rtp_addr.sin_addr), ntohs(client.second.rtp_addr.sin_port), rtp_size);
                    }
                } else if (client.second.rtp_socket == -1) {
                    // TCP transport (interleaved)
                    // Send interleaved binary data: '$' + channel + length + data
                    char interleaved_header[4];
                    interleaved_header[0] = '$';  // Magic byte
                    interleaved_header[1] = 0;    // Channel (0 for RTP)
                    interleaved_header[2] = (rtp_size >> 8) & 0xFF;  // Length high byte
                    interleaved_header[3] = rtp_size & 0xFF;          // Length low byte
                    
                    // printf("RTSP: Sending TCP interleaved RTP packet: header=$%d, size=%zu\n", 
                    //        interleaved_header[1], rtp_size);
                    
                    // Send header
                    ssize_t sent_header = send(client.first, interleaved_header, 4, MSG_NOSIGNAL);
                    if (sent_header != 4) {
                        printf("RTSP: Failed to send interleaved header (sent=%zd, expected=4)\n", sent_header);
                        dead_clients.push_back(client.first);
                        continue;
                    }
                    
                    // Send RTP data
                    ssize_t sent_data = send(client.first, rtp_data, rtp_size, MSG_NOSIGNAL);
                    if (sent_data < 0 || sent_data != (ssize_t)rtp_size) {
                        printf("RTSP: Failed to send interleaved data (sent=%zd, expected=%zu)\n", sent_data, rtp_size);
                        dead_clients.push_back(client.first);
                    } else {
                        // printf("RTSP: Successfully sent TCP interleaved RTP packet (size=%zu)\n", rtp_size);
                    }
                } else {
                    dead_clients.push_back(client.first);
                }
            }
        }
    }
    
    // Remove dead clients
    for (int dead_socket : dead_clients) {
        // Close sockets
        close(dead_socket);
        if (clients[dead_socket].rtp_socket >= 0) {
            close(clients[dead_socket].rtp_socket);
        }
        
        // Remove from clients map
        clients.erase(dead_socket);
    }
    
    pthread_mutex_unlock(&clients_mutex);
}

/**
 * Send SPS/PPS to all connected clients
 */
void RTSPServer::sendSPSPPSToAllClients() {
    pthread_mutex_lock(&clients_mutex);
    printf("RTSP: sendSPSPPSToAllClients called, clients=%zu\n", clients.size());
    
    if (clients.empty()) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }
    
    // This function will be called from main.cpp when SPS/PPS are available
    // The actual SPS/PPS data will be sent via sendRTPPacket
    pthread_mutex_unlock(&clients_mutex);
}

void RTSPServer::regenerateSDP() {
    printf("RTSP: Regenerating SDP with updated SPS/PPS data\n");
    generateSDP();
}

void RTSPServer::updateSPSPPSData(const char* sps_data, size_t sps_size, const std::map<uint8_t, PPSData>& pps_map) {
    if (!sps_data || sps_size == 0 || pps_map.empty()) {
        printf("RTSP: No valid SPS/PPS data to update\n");
        return;
    }
    
    printf("RTSP: Updating SPS/PPS data - SPS: %zu bytes, PPS count: %zu\n", sps_size, pps_map.size());
    
    // Encode SPS
    std::string sps_base64 = base64_encode(sps_data, sps_size);
    
    // Encode all PPS
    std::string pps_base64_list;
    for (const auto& pps_pair : pps_map) {
        const PPSData& pps = pps_pair.second;
        if (pps.data && pps.size > 0) {
            if (!pps_base64_list.empty()) {
                pps_base64_list += ",";
            }
            pps_base64_list += base64_encode(pps.data, pps.size);
        }
    }
    
    // Create sprop-parameter-sets
    std::string sprop_parameter_sets = sps_base64 + "," + pps_base64_list;
    printf("RTSP: Generated sprop-parameter-sets: %s\n", sprop_parameter_sets.c_str());
    
    // Regenerate SDP with the new parameter sets
    generateSDPWithParameterSets(sprop_parameter_sets);
}

void RTSPServer::generateSDPWithParameterSets(const std::string& sprop_parameter_sets) {
    // Use the known external IP address for the server
    char server_ip[INET_ADDRSTRLEN] = "192.0.50.30";
    
    printf("RTSP: Generated SDP with server IP: %s and sprop-parameter-sets\n", server_ip);
    
    // Generate RFC 4566 compliant SDP content for H.264 stream
    // Session ID and version (using timestamp for uniqueness)
    time_t now = time(NULL);
    std::string session_id = std::to_string(now);
    std::string session_version = std::to_string(now);
    
    sdp_content = 
        // Protocol Version (required)
        "v=0\r\n"
        // Origin (required): username, session-id, session-version, nettype, addrtype, unicast-address
        "o=- " + session_id + " " + session_version + " IN IP4 " + std::string(server_ip) + "\r\n"
        // Session Name (required)
        "s=CSI Camera H.264 Stream\r\n"
        // Session Information (optional but recommended)
        "i=Live H.264 video stream from CSI camera\r\n"
        // URI (optional)
        "u=http://" + std::string(server_ip) + ":8522/high\r\n"
        // Email Address (optional)
        "e=admin@" + std::string(server_ip) + "\r\n"
        // Phone Number (optional)
        "p=+1-555-000-0000\r\n"
        // Connection Information (required if not in media description)
        "c=IN IP4 " + std::string(server_ip) + "\r\n"
        // Bandwidth Information (optional)
        "b=AS:2000\r\n"
        "b=TIAS:2000000\r\n"
        // Time Description (required)
        "t=0 0\r\n"
        // Repeat Times (optional)
        "r=604800 3600 0 90000\r\n"
        // Timezone (optional)
        "z=2882844526 -1h 2898848070 0\r\n"
        // Encryption Keys (optional)
        "k=clear:password\r\n"
        // Attributes (optional)
        "a=recvonly\r\n"
        "a=type:broadcast\r\n"
        "a=charset:UTF-8\r\n"
        "a=sdplang:en\r\n"
        "a=lang:en\r\n"
        "a=framerate:30\r\n"
        "a=quality:10\r\n"
        // Media Description (required)
        "m=video 0 RTP/AVP 96\r\n"
        // Media Title (optional)
        "i=Video Track\r\n"
        // Media Connection Information (optional, inherits from session)
        "c=IN IP4 " + std::string(server_ip) + "\r\n"
        // Media Bandwidth (optional)
        "b=AS:1500\r\n"
        "b=TIAS:1500000\r\n"
        // Media Attributes (required for RTP)
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42001f;sprop-parameter-sets=" + sprop_parameter_sets + "\r\n"
        "a=control:track0\r\n"
        // Additional H.264 specific attributes
        "a=framerate:30\r\n"
        "a=framesize:96 1920-1080\r\n"
        "a=cliprect:0,0,1080,1920\r\n"
        "a=orient:landscape\r\n"
        "a=type:broadcast\r\n"
        "a=tool:GStreamer\r\n"
        "a=recvonly\r\n";
}

void* RTSPServer::serverThread(void* arg) {
    RTSPServer* server = (RTSPServer*)arg;
    server->runServer();
    return NULL;
}

void RTSPServer::runServer() {
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (running) {
                perror("Failed to accept client connection");
            }
            continue;
        }
        
        printf("RTSP: New client connected from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        // Create client handler and start thread
        pthread_t client_thread;
        ClientHandler* handler = new ClientHandler(this, client_socket, client_addr);
        pthread_create(&client_thread, NULL, handleClient, handler);
        pthread_detach(client_thread);
    }
}

void* RTSPServer::handleClient(void* arg) {
    ClientHandler* handler = (ClientHandler*)arg;
    handler->run();
    delete handler;
    return NULL;
}

// ClientHandler method implementations
void ClientHandler::run() {
    char buffer[4096];
    
    // Add client to server's client map
    pthread_mutex_lock(&server->clients_mutex);
    server->clients[client_socket] = session;
    printf("RTSP: Initial client added to map - Total clients: %zu\n", server->clients.size());
    pthread_mutex_unlock(&server->clients_mutex);
    
    while (server->running) {
        ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                printf("RTSP: Client %s:%d disconnected gracefully\n",
                       inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            } else {
                printf("RTSP: Client %s:%d disconnected with error: %s\n",
                       inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), strerror(errno));
            }
            break;
        }
        
        buffer[bytes_received] = '\0';
        
        // Check if this is interleaved binary data (starts with '$')
        if (buffer[0] == '$') {
            // This is interleaved binary data, not an RTSP request
            // Skip processing and continue reading
            continue;
        }
        
        printf("RTSP Request from %s:%d:\n%s\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), buffer);
        
        std::string response = processRequest(std::string(buffer));
        ssize_t sent = send(client_socket, response.c_str(), response.length(), MSG_NOSIGNAL);
        if (sent < 0) {
            printf("RTSP: Failed to send response to client %s:%d: %s\n",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), strerror(errno));
            break;
        }
        
        printf("RTSP Response sent to %s:%d:\n%s\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), response.c_str());
    }
    
    // Cleanup client from server's client map
    pthread_mutex_lock(&server->clients_mutex);
    printf("RTSP: Cleanup client from map - Before: size=%zu\n", server->clients.size());
    if (server->clients.find(client_socket) != server->clients.end()) {
        // Close RTP socket if it exists
        if (server->clients[client_socket].rtp_socket >= 0) {
            close(server->clients[client_socket].rtp_socket);
        }
        server->clients.erase(client_socket);
    }
    printf("RTSP: Client %s:%d cleaned up - After: size=%zu\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), server->clients.size());
    pthread_mutex_unlock(&server->clients_mutex);
    
    close(client_socket);
}

std::string ClientHandler::processRequest(const std::string& request) {
    std::string method = extractMethod(request);
    std::string cseq = extractCSeq(request);
    
    if (method == "DESCRIBE") {
        return generateDescribeResponse(cseq);
    } else if (method == "SETUP") {
        return generateSetupResponse(cseq, "", request);
    } else if (method == "PLAY") {
        return generatePlayResponse(cseq);
    } else if (method == "TEARDOWN") {
        return generateTeardownResponse(cseq);
    } else if (method == "OPTIONS") {
        return "RTSP/1.0 200 OK\r\n"
               "CSeq: " + cseq + "\r\n"
               "Public: DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n"
               "Server: CSI Camera RTSP Server\r\n"
               "Date: " + getCurrentTimeString() + "\r\n"
               "\r\n";
    } else {
        return "RTSP/1.0 501 Not Implemented\r\n"
               "CSeq: " + cseq + "\r\n"
               "Server: CSI Camera RTSP Server\r\n"
               "\r\n";
    }
}

std::string ClientHandler::extractMethod(const std::string& request) {
    size_t space_pos = request.find(' ');
    if (space_pos != std::string::npos) {
        return request.substr(0, space_pos);
    }
    return "";
}

std::string ClientHandler::extractCSeq(const std::string& request) {
    size_t cseq_pos = request.find("CSeq:");
    if (cseq_pos != std::string::npos) {
        size_t start = cseq_pos + 5;
        while (start < request.length() && request[start] == ' ') start++;
        size_t end = request.find('\r', start);
        if (end == std::string::npos) end = request.find('\n', start);
        if (end != std::string::npos) {
            return request.substr(start, end - start);
        }
    }
    return "1";
}

std::string ClientHandler::generateSessionId() {
    static int session_counter = 1;
    return "CSI_CAMERA_" + std::to_string(session_counter++);
}

std::string ClientHandler::getCurrentTimeString() {
    time_t now = time(NULL);
    struct tm* tm_info = gmtime(&now);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", tm_info);
    return std::string(buffer);
}

std::string ClientHandler::generateDescribeResponse(const std::string& cseq) {
    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " + cseq + "\r\n"
           "Content-Type: application/sdp\r\n"
           "Content-Length: " + std::to_string(server->sdp_content.length()) + "\r\n"
           "Server: CSI Camera RTSP Server\r\n"
           "Date: " + getCurrentTimeString() + "\r\n"
           "Cache-Control: no-cache\r\n"
           "Expires: " + getCurrentTimeString() + "\r\n"
           "\r\n"
           + server->sdp_content;
}

std::string ClientHandler::generateSetupResponse(const std::string& cseq, const std::string& /*url*/, const std::string& request) {
    // Extract transport information from request
    size_t transport_pos = request.find("Transport:");
    if (transport_pos != std::string::npos) {
        std::string transport_line = request.substr(transport_pos);
        size_t line_end = transport_line.find('\r');
        if (line_end == std::string::npos) line_end = transport_line.find('\n');
        if (line_end != std::string::npos) {
            transport_line = transport_line.substr(0, line_end);
        }
        
        printf("RTSP: Client transport request: %s\n", transport_line.c_str());
        
        // Check if client wants TCP transport
        bool wants_tcp = transport_line.find("RTP/AVP/TCP") != std::string::npos;
        bool wants_interleaved = transport_line.find("interleaved=") != std::string::npos;
        
        if (wants_tcp || wants_interleaved) {
            // Handle TCP transport (interleaved)
            printf("RTSP: Client requested TCP transport, using interleaved mode\n");
            
            // For TCP transport, we don't need UDP sockets
            session.rtp_socket = -1; // Mark as TCP transport
            session.rtp_port = 0;
            session.rtcp_port = 0;
            
            // Update server's client map
            pthread_mutex_lock(&server->clients_mutex);
            server->clients[client_socket] = session;
            printf("RTSP: SETUP TCP - Updated client in map - Total clients: %zu\n", server->clients.size());
            pthread_mutex_unlock(&server->clients_mutex);
            
            return "RTSP/1.0 200 OK\r\n"
                   "CSeq: " + cseq + "\r\n"
                   "Session: " + session.session_id + "\r\n"
                   "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                   "Server: CSI Camera RTSP Server\r\n"
                   "\r\n";
        } else {
            // Handle UDP transport
            printf("RTSP: Client requested UDP transport\n");
            
            // Parse client RTP port
            size_t client_port_pos = transport_line.find("client_port=");
            if (client_port_pos != std::string::npos) {
                size_t start = client_port_pos + 12;
                size_t end = transport_line.find('-', start);
                if (end != std::string::npos) {
                    std::string client_port_str = transport_line.substr(start, end - start);
                    session.rtp_port = std::stoi(client_port_str);
                    
                    // Parse RTCP port
                    size_t rtcp_start = end + 1;
                    size_t rtcp_end = transport_line.find(';', rtcp_start);
                    if (rtcp_end == std::string::npos) rtcp_end = transport_line.find('\r', rtcp_start);
                    if (rtcp_end == std::string::npos) rtcp_end = transport_line.find('\n', rtcp_start);
                    if (rtcp_end == std::string::npos) rtcp_end = transport_line.length(); // End of string
                    if (rtcp_end > rtcp_start) {
                        std::string rtcp_port_str = transport_line.substr(rtcp_start, rtcp_end - rtcp_start);
                        session.rtcp_port = std::stoi(rtcp_port_str);
                    }
                    printf("RTSP: Parsed client ports - RTP: %d, RTCP: %d\n", session.rtp_port, session.rtcp_port);
                }
            }
            
            // Create RTP socket for this client
            session.rtp_socket = socket(AF_INET, SOCK_DGRAM, 0);
            if (session.rtp_socket >= 0) {
                session.rtp_addr.sin_family = AF_INET;
                session.rtp_addr.sin_addr = client_addr.sin_addr;
                session.rtp_addr.sin_port = htons(session.rtp_port);
                
                printf("RTSP: Created RTP socket %d for client %s:%d (RTP port %d)\n",
                       session.rtp_socket, inet_ntoa(client_addr.sin_addr), 
                       ntohs(client_addr.sin_port), session.rtp_port);
                printf("RTSP: RTP address configured: %s:%d\n",
                       inet_ntoa(session.rtp_addr.sin_addr), ntohs(session.rtp_addr.sin_port));
            } else {
                printf("RTSP: Failed to create RTP socket for client %s:%d\n",
                       inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            }
            
            // Update server's client map
            pthread_mutex_lock(&server->clients_mutex);
            server->clients[client_socket] = session;
            printf("RTSP: SETUP UDP - Updated client in map - Total clients: %zu\n", server->clients.size());
            pthread_mutex_unlock(&server->clients_mutex);
            
            return "RTSP/1.0 200 OK\r\n"
                   "CSeq: " + cseq + "\r\n"
                   "Session: " + session.session_id + "\r\n"
                   "Transport: RTP/AVP;unicast;client_port=" + std::to_string(session.rtp_port) + 
                   "-" + std::to_string(session.rtcp_port) + "\r\n"
                   "Server: CSI Camera RTSP Server\r\n"
                   "\r\n";
        }
    }
    
    return "RTSP/1.0 400 Bad Request\r\n"
           "CSeq: " + cseq + "\r\n"
           "Server: CSI Camera RTSP Server\r\n"
           "\r\n";
}

std::string ClientHandler::generatePlayResponse(const std::string& cseq) {
    session.playing = true;
    
    // Update server's client map
    pthread_mutex_lock(&server->clients_mutex);
    server->clients[client_socket] = session;
    printf("RTSP: Added client to map - Total clients: %zu\n", server->clients.size());
    printf("RTSP: Client map after adding: size=%zu\n", server->clients.size());
    pthread_mutex_unlock(&server->clients_mutex);
    
    // Use the known external IP address for the server
    char server_ip[INET_ADDRSTRLEN] = "192.0.50.30";
    
    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " + cseq + "\r\n"
           "Session: " + session.session_id + "\r\n"
           "RTP-Info: url=rtsp://" + std::string(server_ip) + ":8522/high;seq=1;rtptime=0\r\n"
           "Server: CSI Camera RTSP Server\r\n"
           "Date: " + getCurrentTimeString() + "\r\n"
           "Range: npt=0.000-\r\n"
           "Scale: 1.000\r\n"
           "Speed: 1.000\r\n"
           "\r\n";
}

std::string ClientHandler::generateTeardownResponse(const std::string& cseq) {
    session.playing = false;
    
    // Remove from server's client map
    pthread_mutex_lock(&server->clients_mutex);
    printf("RTSP: Removing client from map - Before: size=%zu\n", server->clients.size());
    server->clients.erase(client_socket);
    printf("RTSP: Removed client from map - After: size=%zu\n", server->clients.size());
    pthread_mutex_unlock(&server->clients_mutex);
    
    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " + cseq + "\r\n"
           "Session: " + session.session_id + "\r\n"
           "Server: CSI Camera RTSP Server\r\n"
           "\r\n";
}

// Global RTSP server instance
static RTSPServer* g_rtsp_server = nullptr;

// Functions to integrate with existing camera application
extern "C" {
    bool startRTSPServer(int port) {
        if (g_rtsp_server != nullptr) {
            printf("RTSP Server already running\n");
            return true;
        }
        
        g_rtsp_server = new RTSPServer(port);
        return g_rtsp_server->start();
    }
    
    void stopRTSPServer() {
        if (g_rtsp_server != nullptr) {
            g_rtsp_server->stop();
            delete g_rtsp_server;
            g_rtsp_server = nullptr;
        }
    }
    
    bool isRTSPServerRunning() {
        return g_rtsp_server != nullptr && g_rtsp_server->isRunning();
    }
    
    void sendRTPPacketToClients(const char* rtp_data, size_t rtp_size) {
        if (g_rtsp_server != nullptr) {
            g_rtsp_server->sendRTPPacket(rtp_data, rtp_size);
        }
    }
}