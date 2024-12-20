#include <iostream>
#include <string>
#include <WS2tcpip.h>
#include <winsock2.h>
#include <vector>
#include <signal.h>
#include <unordered_map>
#include <sstream>
#include <fstream>
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




std::ofstream commandLog("commands.log", std::ios::app); // Append mode
std::ofstream publicMessageLog("public_messages.log", std::ios::app);

class Server {
private:

    SOCKET listenSocket;
    fd_set masterSet;    // Master file descriptor set
    fd_set readySet;     // Ready file descriptor set
    int maxClients;      // Maximum number of clients
    char commandChar;    // Command character
    std::vector<SOCKET> clientSockets;

    static const int MAX_BUFFER_SIZE = 2056 ;
    struct ClientBuffer {
        SOCKET socket;
        char buffer[MAX_BUFFER_SIZE];
        int bytesReceived;
        uint8_t expectedLength;
        bool headerReceived;

        ClientBuffer(SOCKET s) : socket(s), bytesReceived(0), expectedLength(0), headerReceived(false) {
            memset(buffer, 0, MAX_BUFFER_SIZE);
        }
        void reset() {
            memset(buffer, 0, MAX_BUFFER_SIZE);  // Clear the entire buffer
            bytesReceived = 0;
            expectedLength = 0;
            headerReceived = false;
        }
    };
    std::vector<ClientBuffer> clientBuffers;
    struct Command {
        std::string name;
        std::string description;
    };

    struct User {
        std::string username;
        std::string password;
        bool isLoggedIn;
        SOCKET socket;

        User(const std::string& uname = "", const std::string& pwd = "")
            : username(uname), password(pwd), isLoggedIn(false), socket(INVALID_SOCKET) {}
    };

    std::unordered_map<std::string, User> users;  // Username -> User mapping
    std::unordered_map<SOCKET, std::string> socketToUsername;  // Socket -> Username mapping

    std::vector<Command> commands = {
        {"help", "Display all available commands"},
        {"register", "Register a new user account (usage: ~register username password)"},
        {"login", "Log in with registered credentials (usage: ~login username password)"}
    };
public:
    Server() : listenSocket(INVALID_SOCKET), maxClients(0), commandChar('~') {
        FD_ZERO(&masterSet);
        FD_ZERO(&readySet);
    }

    ~Server() {
        stop();
    }

    int init() {
        uint16_t port;
        std::cout << "Enter TCP port number: ";
        std::cin >> port;

        std::cout << "Enter maximum chat capacity: ";
        std::cin >> maxClients;

        std::cout << "Enter command character (default is ~): ";
        std::cin.ignore();
        char input = std::cin.get();
        commandChar = (input != '\n') ? input : '~';

        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return SETUP_ERROR;
        }

        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET) {
            WSACleanup();
            return SETUP_ERROR;
        }
        int reuseAddr = 1;
        if (setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
            (const char*)&reuseAddr, sizeof(reuseAddr)) == SOCKET_ERROR) {
            std::cerr << "Failed to set SO_REUSEADDR option. Error: "
                << WSAGetLastError() << std::endl;
            stop();
            return SETUP_ERROR;
        }


        char hostName[256];
        if (gethostname(hostName, sizeof(hostName)) == 0) {
            displayHostInfo(hostName, port);
        }

        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(port);

        if (bind(listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            stop();
            return BIND_ERROR;
        }

        if (listen(listenSocket, maxClients) == SOCKET_ERROR) {
            stop();
            return SETUP_ERROR;
        }

        FD_SET(listenSocket, &masterSet);

        std::cout << "Server initialized successfully\n";
        std::cout << "Command character is: " << commandChar << "\n";
        std::cout << "Maximum clients: " << maxClients << "\n";

        return SUCCESS;
    }

    int sendBroadcast(const char* message, size_t length) {
        sockaddr_in broadcastAddr;
        broadcastAddr.sin_family = AF_INET;
        broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
        broadcastAddr.sin_port = htons(9999); // Use appropriate broadcast port

        int result = sendto(listenSocket, message, static_cast<int>(length), 0,
            (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

        if (result == SOCKET_ERROR) {
            std::cerr << "Broadcast failed with error: " << WSAGetLastError() << std::endl;
            return SOCKET_ERROR;
        }

        return result;
    }
    int processNetworkEvents() {
        readySet = masterSet;  // Copy the master set

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
        std::string welcomeMsg = "Welcome to the chat server!\n";
        welcomeMsg += "Command character is: ";
        welcomeMsg += commandChar;


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

        if (!clientBuf.headerReceived) {
            char lengthByte;
            int result = recv(clientBuf.socket, &lengthByte, 1, 0);

            if (result <= 0) {
                return (result == 0) ? SHUTDOWN : DISCONNECT;
            }

            clientBuf.expectedLength = static_cast<uint8_t>(lengthByte);
            clientBuf.headerReceived = true;
            clientBuf.bytesReceived = 0;

            if (clientBuf.expectedLength > MAX_BUFFER_SIZE - 1) {
                return PARAMETER_ERROR;
            }
        }

        int remainingBytes = clientBuf.expectedLength - clientBuf.bytesReceived;
        int result = recv(clientBuf.socket,
            clientBuf.buffer + clientBuf.bytesReceived,
            remainingBytes, 0);

        if (result <= 0) {
            return (result == 0) ? SHUTDOWN : DISCONNECT;
        }

        clientBuf.bytesReceived += result;

        if (clientBuf.bytesReceived == clientBuf.expectedLength) {
            clientBuf.buffer[clientBuf.bytesReceived] = '\0';

            //	for(int i = 0 ; i < clientBuf.bytesReceived ; i++)
                    //cout << clientBuf.buffer[i];


            processMessage(clientIndex, clientBuf.buffer, clientBuf.bytesReceived);

            clientBuf.reset();
        }

        return SUCCESS;
    }
    int sendMessage(SOCKET clientSocket, const char* data, int32_t length) {
        if (length <= 0 || length > MAX_BUFFER_SIZE - 1) {  return PARAMETER_ERROR; }
        //check 
        if (data[length] != '\0') {
            char sanitizedData[MAX_BUFFER_SIZE];
            memset(sanitizedData, 0, MAX_BUFFER_SIZE);
            strncpy_s(sanitizedData, data, length);  
            data = sanitizedData;                  
        }
        uint8_t msgSize = static_cast<uint8_t>(length);
        int result = send(clientSocket, (char*)&msgSize, 1, 0);
        if (result <= 0) return (result == 0) ? SHUTDOWN : DISCONNECT;

        result = send(clientSocket, data, msgSize, 0);
        return (result <= 0) ? ((result == 0) ? SHUTDOWN : DISCONNECT) : SUCCESS;
    }


    void processMessage(size_t sourceClientIndex, const char* message, int length) {
        SOCKET clientSocket = clientSockets[sourceClientIndex];
        auto usernameIt = socketToUsername.find(clientSocket);

        std::string username = (usernameIt != socketToUsername.end()) ? usernameIt->second : "UnknownUser";

        if (length > 0 && message[0] == commandChar) {
            std::string command(message + 1, length - 1);
            std::string helpCommand(message + 1, 4);
            std::string logoutCmd(message + 1, 6);
			string getList(message + 1, 7);
			string getLog(message + 1, 6);
            std::istringstream iss(command);
            std::string cmd;
            iss >> cmd;
			
            if (getLog == "getlog") {
                std::ifstream file("public_messages.log");
				std::string line;
				std::string log;
				while (std::getline(file, line))
				{
                    sendMessage(clientSockets[sourceClientIndex], line.c_str(), static_cast<int32_t>(line.length()));
				}
				file.close();


                return; 
            }
            if (commandLog.is_open()) {
                commandLog << "User: " << username << ", Command: " << command << std::endl;
            }

            if (cmd == "register") {
                handleRegistration(sourceClientIndex, command);
                return;
            }
            else if (cmd == "login") {
                handleLogin(sourceClientIndex, command);
                return;
            }

            if (logoutCmd == "logout") {
                removeClient(sourceClientIndex);
                return;
            }
            if (helpCommand == "help") {
                std::string ss;
                for (auto word : commands) {
                    ss += word.name + " - " + word.description + "\n";
                }
                sendMessage(clientSocket, ss.c_str(), ss.length());
                return;
            }

            if (usernameIt == socketToUsername.end()) {
                std::string errorMsg = "You must be logged in to use this command.\n";
                sendMessage(clientSocket, errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
                return;
            }

            string sendCmd(message + 1, 4);
             sendCmd = cmd;

            if (sendCmd == "send")
            {
                if (usernameIt == socketToUsername.end()) {
                    std::string errorMsg = "You must be logged in to send messages.\n";
                    sendMessage(clientSocket, errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
                    return;
                }

                std::string targetUsername;
                iss >> targetUsername;

                if (targetUsername.empty()) {
                    std::string errorMsg = "Usage: ~send <username> <message>\n";
                    sendMessage(clientSocket, errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
                    return;
                }

                std::string privateMessage;
                std::getline(iss, privateMessage);
                if (privateMessage.empty() || privateMessage == " ") {
                    std::string errorMsg = "Message cannot be empty\n";
                    sendMessage(clientSocket, errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
                    return;
                }

                privateMessage = privateMessage.substr(privateMessage.find_first_not_of(" "));

                SOCKET targetSocket = INVALID_SOCKET;
                for (const auto& entry : socketToUsername) {
                    if (entry.second == targetUsername) {
                        targetSocket = entry.first;
                        break;
                    }
                }

                if (targetSocket == INVALID_SOCKET) {
                    std::string errorMsg = "User '" + targetUsername + "' not found or not online.\n";
                    sendMessage(clientSocket, errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
                    return;
                }

                std::string formattedMsg = "[Private from " + username + "]: " + privateMessage;
                sendMessage(targetSocket, formattedMsg.c_str(), static_cast<int32_t>(formattedMsg.length()));

                std::string confirmMsg = "[Private to " + targetUsername + "]: " + privateMessage;
                sendMessage(clientSocket, confirmMsg.c_str(), static_cast<int32_t>(confirmMsg.length()));

                if (publicMessageLog.is_open()) {
                    publicMessageLog << "[Private] " << username << " to " << targetUsername << ": " << privateMessage << std::endl;
                }

                return;
            }
			if (getList == "getlist")
			{

                SOCKET clientSocket = clientSockets[sourceClientIndex];

                auto usernameIt = socketToUsername.find(clientSocket);
                if (usernameIt == socketToUsername.end()) {
                    std::string errorMsg = "You must be logged in to use the ~getlist command.";
                    sendMessage(clientSocket, errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
                    return;
                }

                std::string activeUsersList = "Active clients:\n";
                for (const auto& entry : socketToUsername) {
                    activeUsersList += "- " + entry.second + "\n";
                }

                if (activeUsersList == "Active clients:\n") {
                    activeUsersList += "No clients are currently logged in.";
                }

                sendMessage(clientSocket, activeUsersList.c_str(), static_cast<int32_t>(activeUsersList.length()));

                return; 
			}


        }
        else {
            if (usernameIt == socketToUsername.end()) {
                std::string errorMsg = "You must be logged in to send messages. Please register and login first.\n";
                sendMessage(clientSocket, errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
                return;
            }

            std::string formattedMsg = username + ": " + std::string(message, length);

            if (publicMessageLog.is_open()) {
                publicMessageLog << formattedMsg << std::endl;
            }

            for (size_t i = 0; i < clientSockets.size(); i++) {
                if (i != sourceClientIndex) {
                    sendMessage(clientSockets[i], formattedMsg.c_str(), static_cast<int32_t>(formattedMsg.length()));
                }
            }
        }
    }



    void handleRegistration(size_t clientIndex, const std::string& command) {
        std::istringstream iss(command);
        std::string cmd, username, password;

        iss >> cmd >> username >> password;

        if (username.empty() || password.empty()) {
            std::string errorMsg = "Usage: ~register username password\n";
            sendMessage(clientSockets[clientIndex], errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
            return;
        }

        if (users.find(username) != users.end()) {
            std::string errorMsg = "Username already exists. Please choose another.\n";
            sendMessage(clientSockets[clientIndex], errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
            return;
        }

        if (users.size() >= maxClients) {
            std::string errorMsg = "Server capacity reached. Registration declined.\n";
            sendMessage(clientSockets[clientIndex], errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
            return;
        }

        users.emplace(username, User(username, password));

        std::string successMsg = "Registration successful! You can now login with ~login username password\n";
        sendMessage(clientSockets[clientIndex], successMsg.c_str(), static_cast<int32_t>(successMsg.length()));
    }

    void handleLogin(size_t clientIndex, const std::string& command) {
        std::istringstream iss(command);
        std::string cmd, username, password;

        iss >> cmd >> username >> password;

        if (username.empty() || password.empty())
        {
            std::string errorMsg = "Usage: ~login username password";
            sendMessage(clientSockets[clientIndex], errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
            return;
        }

        auto userIt = users.find(username);
        if (userIt == users.end())
        {
            std::string errorMsg = "Username not found. Please register first.";
            sendMessage(clientSockets[clientIndex], errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
            return;
        }

        if (userIt->second.password != password)
        {
            std::string errorMsg = "Invalid password.";
            sendMessage(clientSockets[clientIndex], errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
            return;
        }

        if (userIt->second.isLoggedIn)
        {
            std::string errorMsg = "User already logged in from another location.";
            sendMessage(clientSockets[clientIndex], errorMsg.c_str(), static_cast<int32_t>(errorMsg.length()));
			removeClient(clientIndex);
            return;
        }

        userIt->second.isLoggedIn = true;
        userIt->second.socket = clientSockets[clientIndex];
        socketToUsername[clientSockets[clientIndex]] = username;

        std::string successMsg2 = "Login successful! Welcome to the chat, " + username + "!\n";

        sendMessage(clientSockets[clientIndex], successMsg2.c_str(),
            successMsg2.length());
    }

    void removeClient(size_t clientIndex) {
    SOCKET clientSocket = clientSockets[clientIndex];

    auto usernameIt = socketToUsername.find(clientSocket);
    if (usernameIt != socketToUsername.end()) {
        std::string username = usernameIt->second;

        if (commandLog.is_open()) {
            commandLog << "User: " << username << " has logged out." << std::endl;
        }

        auto userIt = users.find(username);
        if (userIt != users.end()) {
            userIt->second.isLoggedIn = false;
            userIt->second.socket = INVALID_SOCKET;
        }

        socketToUsername.erase(usernameIt);
    }

    closesocket(clientSocket);
    clientSockets.erase(clientSockets.begin() + clientIndex);

    clientBuffers.erase(clientBuffers.begin() + clientIndex);

    FD_CLR(clientSocket, &masterSet);
	cout << "Disconnecting client . Remaining clients: " << clientSockets.size() << endl;
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
        readySet = masterSet;

        if (select(0, &readySet, nullptr, nullptr, nullptr) == SOCKET_ERROR) {
            return SELECT_ERROR;
        }

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
        for (SOCKET clientSocket : clientSockets) {
            shutdown(clientSocket, SD_BOTH);
            closesocket(clientSocket);
        }
        clientSockets.clear();

        if (listenSocket != INVALID_SOCKET) {
            shutdown(listenSocket, SD_BOTH);
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
        }

        WSACleanup();
    }
};
Server* g_server = nullptr;

void signalHandler(int signum) {
    std::cout << "\nShutdown signal received.\n";
    if (g_server) {
        g_server->stop();
    }
    exit(signum);
}

int main() {
    try {
        signal(SIGINT, signalHandler);  // Handle Ctrl+C
        signal(SIGTERM, signalHandler); // Handle termination request

        // Create server instance
        Server server;
        g_server = &server;

        std::cout << "=== TCP Chat Server ===\n\n";

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

            Sleep(1);  // 1ms sleep prevent CPU hogging
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