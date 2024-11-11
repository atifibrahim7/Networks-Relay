#include <iostream>
#include <string>
#include <WS2tcpip.h>
#include <winsock2.h>
#include <vector>
#include <signal.h>
#pragma comment(lib, "ws2_32.lib")


#define SUCCESS 0
#define BIND_ERROR -1
#define SETUP_ERROR -2
#define CONNECT_ERROR -3
#define SHUTDOWN -4
#define DISCONNECT -5
#define PARAMETER_ERROR -6
#define SELECT_ERROR -7
#define CAPACITY_ERROR -8
using namespace std; 


struct ClientInfo {
    SOCKET socket;
    std::vector<uint8_t> receiveBuffer;
    size_t bytesReceived;
    bool hasFullMessage;

    ClientInfo(SOCKET s) : socket(s), bytesReceived(0), hasFullMessage(false) {}
};


class Server {
private:
    SOCKET listenSocket;
    fd_set masterSet;    // Master file descriptor set
    fd_set readySet;     // Ready file descriptor set
    int maxClients;      // Maximum number of clients
    char commandChar;    // Command character
    std::vector<SOCKET> clientSockets;

    // Buffer for message handling
    static const int MAX_BUFFER_SIZE = 256;
    struct ClientBuffer {
        SOCKET socket;
        char buffer[MAX_BUFFER_SIZE];
        int bytesReceived;
        uint8_t expectedLength;
        bool headerReceived;

        ClientBuffer(SOCKET s) : socket(s), bytesReceived(0), expectedLength(0), headerReceived(false) {
            memset(buffer, 0, MAX_BUFFER_SIZE);
        }
    };
    std::vector<ClientBuffer> clientBuffers;

public:
    Server() : listenSocket(INVALID_SOCKET), maxClients(0), commandChar('~') {
        FD_ZERO(&masterSet);
        FD_ZERO(&readySet);
    }

    ~Server() {
        stop();
    }

    int init() {
        // Prompt for configuration
        uint16_t port;
        std::cout << "Enter TCP port number: ";
        std::cin >> port;

        std::cout << "Enter maximum chat capacity: ";
        std::cin >> maxClients;

        std::cout << "Enter command character (default is ~): ";
        std::cin.ignore();
        char input = std::cin.get();
        commandChar = (input != '\n') ? input : '~';

        // Initialize Winsock
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return SETUP_ERROR;
        }

        // Create listening socket
        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET) {
            WSACleanup();
            return SETUP_ERROR;
        }

        // Get host information
        char hostName[256];
        if (gethostname(hostName, sizeof(hostName)) == 0) {
            displayHostInfo(hostName, port);
        }

        // Setup server address
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(port);

        // Bind socket
        if (bind(listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            stop();
            return BIND_ERROR;
        }

        // Listen for connections
        if (listen(listenSocket, maxClients) == SOCKET_ERROR) {
            stop();
            return SETUP_ERROR;
        }

        // Add listening socket to master set
        FD_SET(listenSocket, &masterSet);

        std::cout << "Server initialized successfully\n";
        std::cout << "Command character is: " << commandChar << "\n";
        std::cout << "Maximum clients: " << maxClients << "\n";

        return SUCCESS;
    }


    int processNetworkEvents() {
        readySet = masterSet;  // Copy the master set

        // Set timeout for select (NULL means blocking)
        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000; // 50ms timeout

        // Wait for activity on sockets
        int selectResult = select(0, &readySet, nullptr, nullptr, &timeout);

        if (selectResult == SOCKET_ERROR) {
            return SELECT_ERROR;
        }

        // Check for new connections if listening socket is ready
        if (FD_ISSET(listenSocket, &readySet)) {
            handleNewConnection();
        }

        // Check for activity on client sockets
        for (size_t i = 0; i < clientSockets.size(); /* no increment */) {
            SOCKET currentSocket = clientSockets[i];

            if (FD_ISSET(currentSocket, &readySet)) {
                int result = handleClientMessage(i);
                if (result != SUCCESS) {
                    // Handle disconnection or error
                    removeClient(i);
                    continue;
                }
            }
            ++i;
        }

        return SUCCESS;
    }

private:


     void sendWelcomeMessage(SOCKET clientSocket) {
        // Format welcome message with command character
         std::string welcomeMsg = "Welcome to the chat server!\n"; 
		 welcomeMsg += "Command character is: ";
		 welcomeMsg += commandChar;

        
        // Send using our framed message protocol
        sendMessage(clientSocket, welcomeMsg.c_str(), static_cast<int32_t>(welcomeMsg.length()));
    }
    int handleNewConnection() {
        SOCKET newClient = accept(listenSocket, nullptr, nullptr);
        if (newClient != INVALID_SOCKET) {
            if (clientSockets.size() < maxClients) {
                FD_SET(newClient, &masterSet);
                clientSockets.push_back(newClient);
                clientBuffers.emplace_back(newClient);
                std::cout << "New client connected. Total clients: " << clientSockets.size() << "\n";
                sendWelcomeMessage(newClient);
                return SUCCESS;
            }
            else {
                closesocket(newClient);
                std::cout << "Connection rejected: maximum clients reached\n";
                return CAPACITY_ERROR;
            }
        }
        return CONNECT_ERROR;
    }

    int handleClientMessage(size_t clientIndex) {
        ClientBuffer& clientBuf = clientBuffers[clientIndex];

        // First, receive the message length if we haven't already
        if (!clientBuf.headerReceived) {
            char lengthByte;
            int result = recv(clientBuf.socket, &lengthByte, 1, 0);

            if (result <= 0) {
                return (result == 0) ? SHUTDOWN : DISCONNECT;
            }

            clientBuf.expectedLength = static_cast<uint8_t>(lengthByte);
            clientBuf.headerReceived = true;
            clientBuf.bytesReceived = 0;

            // Validate message length
            if (clientBuf.expectedLength > MAX_BUFFER_SIZE - 1) { // -1 for null terminator
                return PARAMETER_ERROR;
            }
        }

        // Receive the actual message data
        int remainingBytes = clientBuf.expectedLength - clientBuf.bytesReceived;
        int result = recv(clientBuf.socket,
            clientBuf.buffer + clientBuf.bytesReceived,
            remainingBytes, 0);

        if (result <= 0) {
            return (result == 0) ? SHUTDOWN : DISCONNECT;
        }

        clientBuf.bytesReceived += result;

        // Check if we've received the complete message
        if (clientBuf.bytesReceived == clientBuf.expectedLength) {
            // Null terminate the message
            clientBuf.buffer[clientBuf.bytesReceived] = '\0';

            // Process the complete message
            processMessage(clientIndex, clientBuf.buffer, clientBuf.bytesReceived);

            // Reset for next message
            clientBuf.headerReceived = false;
            clientBuf.bytesReceived = 0;
            clientBuf.expectedLength = 0;
        }

        return SUCCESS;
    }

    void processMessage(size_t sourceClientIndex, const char* message, int length) {
        // Example: Broadcast message to all other clients
        for (size_t i = 0; i < clientSockets.size(); i++) {
            if (i != sourceClientIndex) {
                sendMessage(clientSockets[i], message, length);
            }
        }
    }
    int sendMessage(SOCKET clientSocket, const char* data, int32_t length) {
        if (length <= 0 || length > MAX_BUFFER_SIZE - 1) return PARAMETER_ERROR;

        // Send length byte
        uint8_t msgSize = static_cast<uint8_t>(length);
        int result = send(clientSocket, (char*)&msgSize, 1, 0);
        if (result <= 0) return (result == 0) ? SHUTDOWN : DISCONNECT;

        // Send message data
        result = send(clientSocket, data, msgSize, 0);
        return (result <= 0) ? ((result == 0) ? SHUTDOWN : DISCONNECT) : SUCCESS;
    }

    void removeClient(size_t index) {
        SOCKET clientSocket = clientSockets[index];
        FD_CLR(clientSocket, &masterSet);
        closesocket(clientSocket);

        // Remove from our vectors
        clientSockets.erase(clientSockets.begin() + index);
        clientBuffers.erase(clientBuffers.begin() + index);

        std::cout << "Client disconnected. Remaining clients: " << clientSockets.size() << "\n";
    }

    void displayHostInfo(const char* hostName, uint16_t port) {
        std::cout << "\nServer Host Information:\n";
        std::cout << "Hostname: " << hostName << "\n";

        addrinfo hints = {}, * result = nullptr;
        hints.ai_family = AF_UNSPEC;    // Allow IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        char portStr[6];
        sprintf_s(portStr, sizeof(portStr), "%d", port);

        if (getaddrinfo(hostName, portStr, &hints, &result) == 0) {
            for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
                void* addr;
                char ipStr[INET6_ADDRSTRLEN];

                if (ptr->ai_family == AF_INET) {  // IPv4
                    sockaddr_in* ipv4 = (sockaddr_in*)ptr->ai_addr;
                    addr = &(ipv4->sin_addr);
                    inet_ntop(AF_INET, addr, ipStr, sizeof(ipStr));
                    std::cout << "IPv4: " << ipStr << "\n";
                }
                else if (ptr->ai_family == AF_INET6) {  // IPv6
                    sockaddr_in6* ipv6 = (sockaddr_in6*)ptr->ai_addr;
                    addr = &(ipv6->sin6_addr);
                    inet_ntop(AF_INET6, addr, ipStr, sizeof(ipStr));
                    std::cout << "IPv6: " << ipStr << "\n";
                }
            }
            freeaddrinfo(result);
        }
        std::cout << "Port: " << port << "\n\n";
    }

    int handleConnections() {
        readySet = masterSet;  // Copy the master set for select()

        // Wait for activity on any socket
        if (select(0, &readySet, nullptr, nullptr, nullptr) == SOCKET_ERROR) {
            return SELECT_ERROR;
        }

        // Check for new connections
        if (FD_ISSET(listenSocket, &readySet)) {
            SOCKET newClient = accept(listenSocket, nullptr, nullptr);
            if (newClient != INVALID_SOCKET) {
                if (clientSockets.size() < maxClients) {
                    FD_SET(newClient, &masterSet);
                    clientSockets.push_back(newClient);
                    std::cout << "New client connected\n";
                }
                else {
                    closesocket(newClient);
                    std::cout << "Connection rejected: maximum clients reached\n";
                }
            }
        }

        return SUCCESS;
    }
    public:
    void stop() {
        // Clean up client sockets
        for (SOCKET clientSocket : clientSockets) {
            shutdown(clientSocket, SD_BOTH);
            closesocket(clientSocket);
        }
        clientSockets.clear();

        // Clean up listening socket
        if (listenSocket != INVALID_SOCKET) {
            shutdown(listenSocket, SD_BOTH);
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
        }

        WSACleanup();
    }
};
Server* g_server = nullptr;

// Signal handler for graceful shutdown
void signalHandler(int signum) {
    std::cout << "\nShutdown signal received.\n";
    if (g_server) {
        g_server->stop();
    }
    exit(signum);
}

int main() {
    try {
        // Set up signal handling
        signal(SIGINT, signalHandler);  // Handle Ctrl+C
        signal(SIGTERM, signalHandler); // Handle termination request

        // Create server instance
        Server server;
        g_server = &server;

        std::cout << "=== TCP Chat Server ===\n\n";

        // Initialize the server
        int result = server.init();
        if (result != SUCCESS) {
            std::cerr << "Failed to initialize server. Error code: " << result << "\n";
            return 1;
        }

        std::cout << "Server is running. Press Ctrl+C to stop.\n";
        std::cout << "Waiting for connections...\n\n";

        // Main server loop
        bool running = true;
        while (running) {
            result = server.processNetworkEvents();

            if (result != SUCCESS) {
                switch (result) {
                case SELECT_ERROR:
                    std::cerr << "Select error occurred.\n";
                    break;
                case SHUTDOWN:
                    std::cout << "Server shutdown requested.\n";
                    running = false;
                    break;
                default:
                    std::cerr << "Error occurred: " << result << "\n";
                    break;
                }
            }

            // Small sleep to prevent CPU hogging
            Sleep(1);  // 1ms sleep
        }

        // Cleanup
        server.stop();
        g_server = nullptr;

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception occurred: " << e.what() << "\n";
        return 1;
    }
    catch (...) {
        std::cerr << "Unknown exception occurred\n";
        return 1;
    }
}