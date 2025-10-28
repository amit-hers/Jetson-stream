#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <string>
#include <map>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

// PPS Data structure (must match main.cpp)
struct PPSData {
    char* data;
    size_t size;
    uint8_t pps_id;
    
    PPSData() : data(NULL), size(0), pps_id(0) {}
    ~PPSData() { if (data) delete[] data; }
};

// Forward declaration for ClientHandler
class ClientHandler;

// RTSP Server class declaration
class RTSPServer {
    friend class ClientHandler;
private:
    int server_socket;
    int server_port;
    bool running;
    pthread_t server_thread;
    
    // Client session management
    struct ClientSession {
        int rtsp_socket;
        int rtp_port;
        int rtcp_port;
        std::string session_id;
        bool playing;
        struct sockaddr_in client_addr;
        int rtp_socket;
        struct sockaddr_in rtp_addr;
    };
    
    std::map<int, ClientSession> clients;
    pthread_mutex_t clients_mutex;
    
    // SDP content
    std::string sdp_content;
    
    // Keepalive mechanism
    time_t last_keepalive_check;
    
public:
    RTSPServer(int port = 8554);
    ~RTSPServer();
    
    bool start();
    bool start(int port);
    void stop();
    bool isRunning() const;
    void sendRTPPacket(const char* rtp_data, size_t rtp_size);
    void sendSPSPPSToAllClients();
    void regenerateSDP();
    void updateSPSPPSData(const char* sps_data, size_t sps_size, const std::map<uint8_t, struct PPSData>& pps_map);
    
private:
    void generateSDP();
    void generateSDPWithParameterSets(const std::string& sprop_parameter_sets);
    static void* serverThread(void* arg);
    void runServer();
    static void* handleClient(void* arg);
    void handleDESCRIBE(int client_socket, const std::string& request);
    void handleSETUP(int client_socket, const std::string& request);
    void handlePLAY(int client_socket, const std::string& request);
    void handleTEARDOWN(int client_socket, const std::string& request);
    void cleanupClient(int client_socket);
    void checkKeepalive();
};

#ifdef __cplusplus
extern "C" {
#endif

// C-style function wrappers
bool startRTSPServer(int port);
void stopRTSPServer();
bool isRTSPServerRunning();
void sendRTPPacketToClients(const char* rtp_data, size_t rtp_size);

#ifdef __cplusplus
}
#endif

#endif // RTSP_SERVER_H