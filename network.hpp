#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdexcept>
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

class TwentyFiveNetwork {

public:
    struct Config {
        int udpPort = 0; // UDP port (mandatory)
        int tcpPort = 0; // TCP port (optional)
        int tickRate = 20; // ticks/sec
        int timeoutSeconds = 10; // disconnect if silent too long
        int keepAliveInterval = 3; // seconds between keepalive/timeout notifications
    };

    TwentyFiveNetwork(const Config& cfg)
        : cfg_(cfg), udpSocket_(INVALID_SOCKET), tcpSocket_(INVALID_SOCKET),
        running_(false)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    virtual ~TwentyFiveNetwork() {
        stop();
        if (udpSocket_ != INVALID_SOCKET) closesocket(udpSocket_);
        if (tcpSocket_ != INVALID_SOCKET) closesocket(tcpSocket_);
        WSACleanup();
    }

    void start() {
        running_ = true;
        udpThread_ = std::thread(&TwentyFiveNetwork::udpLoop, this);
        if (cfg_.tcpPort > 0) tcpThread_ = std::thread(&TwentyFiveNetwork::tcpLoop, this);
        pollThread_ = std::thread(&TwentyFiveNetwork::pollConnections, this);
    }

    void stop() {
        running_ = false;
        if (udpThread_.joinable()) udpThread_.join();
        if (tcpThread_.joinable()) tcpThread_.join();
        if (pollThread_.joinable()) pollThread_.join();
    }

    int getTickRate() const { return cfg_.tickRate; }
    int getUDPPort() const { return cfg_.udpPort; }
    int getTCPPort() const { return cfg_.tcpPort; }

protected:
    struct Peer {
        std::chrono::steady_clock::time_point establishedAt;
        std::chrono::steady_clock::time_point lastSeen;
        sockaddr_in addr;
        SOCKET socket = INVALID_SOCKET; // optional for TCP
        bool timedOut = false;
        std::chrono::steady_clock::time_point lastTimeoutSent;
    };

    Config cfg_;
    SOCKET udpSocket_;
    SOCKET tcpSocket_;
    std::atomic<bool> running_;
    std::thread udpThread_;
    std::thread tcpThread_;
    std::thread pollThread_;

    std::mutex peersMutex_;
    std::unordered_map<std::string, Peer> peers_;

    // Utility
    void makeNonBlocking(SOCKET s) {
        u_long mode = 1;
        ioctlsocket(s, FIONBIO, &mode);
    }

    std::string addrToKey(const sockaddr_in& addr) {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
    }

    // --- UDP Loop ---
    virtual void udpLoop() {
        udpSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udpSocket_ == INVALID_SOCKET) throw std::runtime_error("UDP socket failed");
        makeNonBlocking(udpSocket_);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(cfg_.udpPort);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(udpSocket_, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR)
            throw std::runtime_error("UDP bind failed");

        std::cout << "[UDP] Listening on port " << cfg_.udpPort << "\n";

        using clock = std::chrono::steady_clock;
        auto lastTick = clock::now();
        auto tickInterval = std::chrono::duration<double>(1.0 / cfg_.tickRate);

        char buffer[512];
        sockaddr_in from{};

        while (running_) {
            int fromLen = sizeof(from); // reset every loop
            int bytes = recvfrom(udpSocket_, buffer, sizeof(buffer), 0,
                (SOCKADDR*)&from, &fromLen);
            if (bytes > 0) {
                handleIncomingMessage(buffer, bytes, from);
            }

            auto now = clock::now();
            if (now - lastTick >= tickInterval) {
                lastTick = now;
                onUDPTick();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // --- TCP Loop (optional) ---
    virtual void tcpLoop() {
        if (cfg_.tcpPort <= 0) return;

        tcpSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcpSocket_ == INVALID_SOCKET) throw std::runtime_error("TCP socket failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(cfg_.tcpPort);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(tcpSocket_, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR)
            throw std::runtime_error("TCP bind failed");

        if (listen(tcpSocket_, SOMAXCONN) == SOCKET_ERROR)
            throw std::runtime_error("TCP listen failed");

        makeNonBlocking(tcpSocket_);
        std::cout << "[TCP] Listening on port " << cfg_.tcpPort << "\n";

        while (running_) {
            SOCKET clientSock = accept(tcpSocket_, nullptr, nullptr);
            if (clientSock != INVALID_SOCKET) {
                makeNonBlocking(clientSock);
                std::thread([this, clientSock] {
                    char buffer[512];
                    sockaddr_in addr{};
                    int len = sizeof(addr);
                    getpeername(clientSock, (sockaddr*)&addr, &len);
                    std::string key = addrToKey(addr);

                    auto now = std::chrono::steady_clock::now();
                    {
                        std::lock_guard<std::mutex> lock(peersMutex_);
                        peers_[key] = Peer{ now, now, addr, clientSock };
                    }

                    while (running_) {
                        int bytes = recv(clientSock, buffer, sizeof(buffer), 0);
                        if (bytes > 0) {
                            handleIncomingMessage(buffer, bytes, addr);
                        }
                        else if (bytes == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }

                    shutdown(clientSock, SD_BOTH);
                    closesocket(clientSock);

                    std::lock_guard<std::mutex> lock(peersMutex_);
                    peers_.erase(key);
                    }).detach();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // --- Poll Connections and Timeout Notifications ---
    void pollConnections() {
        using clock = std::chrono::steady_clock;
        auto lastKeepAlive = clock::now();
        auto keepAliveInterval = std::chrono::seconds(cfg_.keepAliveInterval);

        while (running_) {
            auto now = clock::now();
            std::vector<std::string> toRemove;

            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                for (auto& [key, peer] : peers_) {
                    auto elapsed = now - peer.lastSeen;
                    if (elapsed > std::chrono::seconds(cfg_.timeoutSeconds)) {
                        if (!peer.timedOut ||
                            now - peer.lastTimeoutSent >= keepAliveInterval) {
                            sendTimeoutNotification(key);
                        }
                        if (elapsed > std::chrono::seconds(cfg_.timeoutSeconds + cfg_.keepAliveInterval)) {
                            toRemove.push_back(key);
                        }
                    }
                }
                for (auto& key : toRemove) peers_.erase(key);
            }

            if (now - lastKeepAlive >= keepAliveInterval) {
                lastKeepAlive = now;
                onKeepAlive();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // --- Send Timeout Notification ---
    void sendTimeoutNotification(const std::string& peerKey) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.find(peerKey);
        if (it == peers_.end()) return;

        auto& peer = it->second;
        std::string msg = "TIMEOUT|" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - peer.establishedAt).count());

        sendto(udpSocket_, msg.c_str(), (int)msg.size(), 0,
            (SOCKADDR*)&peer.addr, sizeof(peer.addr));
        peer.timedOut = true;
        peer.lastTimeoutSent = std::chrono::steady_clock::now();
        std::cout << "[Timeout] Notification sent to " << peerKey << "\n";
    }

    // --- Handle incoming messages with timestamps ---
    void handleIncomingMessage(const char* rawMsg, int size, const sockaddr_in& from) {
        std::string raw(rawMsg, size);
        auto sep = raw.find('|');
        if (sep == std::string::npos) return;

        uint64_t relMs = std::stoull(raw.substr(0, sep));
        std::string payload = raw.substr(sep + 1);

        std::string key = addrToKey(from);
        auto now = std::chrono::steady_clock::now();

        Peer* peer = nullptr;
        {
            std::lock_guard<std::mutex> lock(peersMutex_);
            auto it = peers_.find(key);
            if (it == peers_.end()) {
                peers_[key] = Peer{ now, now, from };
                peer = &peers_[key];
                std::cout << "[Connect] Established with " << key << "\n";
            }
            else {
                peer = &it->second;
            }
        }

        if (peer) {
            auto expectedTime = peer->establishedAt + std::chrono::milliseconds(relMs);
            auto drift = std::chrono::duration_cast<std::chrono::milliseconds>(now - expectedTime);
            if (drift < std::chrono::seconds(cfg_.timeoutSeconds)) {
                peer->lastSeen = now;
                handleUDPMessage(payload, from, drift.count());
            }
            else {
                std::cout << "[Warning] Dropped late packet from " << key << "\n";
            }
        }
    }

    // --- Send timed UDP message ---
    void sendUDPMessage(const std::string& payload, const std::string& peerKey) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.find(peerKey);
        if (it == peers_.end()) return;

        auto now = std::chrono::steady_clock::now();
        uint64_t relMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.establishedAt).count();
        std::string msg = std::to_string(relMs) + "|" + payload;

        sendto(udpSocket_, msg.c_str(), (int)msg.size(), 0, (SOCKADDR*)&it->second.addr, sizeof(it->second.addr));
    }

    // --- Virtual hooks ---
    virtual void handleUDPMessage(const std::string& msg, const sockaddr_in& from, long driftMs) {
        std::cout << "[UDP] " << addrToKey(from) << " -> " << msg << " (drift=" << driftMs << "ms)\n";
    }

    virtual void onUDPTick() {}
    virtual void onTCPTick() {}
    virtual void onKeepAlive() {
        std::cout << "[Poll] Sending keepalive to all peers\n";
    }
};
