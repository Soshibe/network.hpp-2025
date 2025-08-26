#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <type_traits>

#pragma comment(lib, "Ws2_32.lib")

class TwentyFiveNetwork {
public:
    struct Config {
        int udpPort = 0; // local port for receiving UDP
        int tcpPort = 0; // local port for receiving TCP
        int tickRate = 20;
    };

    TwentyFiveNetwork(const Config& cfg)
        : cfg_(cfg), running_(false),
        udpSocket_(INVALID_SOCKET), tcpSocket_(INVALID_SOCKET) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            throw std::runtime_error("WSAStartup failed");
    }

    virtual ~TwentyFiveNetwork() { stopNetwork(); WSACleanup(); }

    // ---------------- Packet ----------------
    struct Packet {
        uint64_t objectId = 0;
        std::string varName;
        std::vector<uint8_t> data;
        bool isTCP = false;

        std::string serialize() const {
            std::string out = std::to_string(objectId) + "|" + varName + "|";
            out.append(reinterpret_cast<const char*>(data.data()), data.size());
            return out;
        }

        static bool deserialize(const std::string& raw, Packet& out) {
            size_t sep1 = raw.find('|');
            size_t sep2 = raw.find('|', sep1 + 1);
            if (sep1 == std::string::npos || sep2 == std::string::npos) return false;
            try {
                out.objectId = std::stoull(raw.substr(0, sep1));
                out.varName = raw.substr(sep1 + 1, sep2 - sep1 - 1);
                out.data.assign(raw.begin() + sep2 + 1, raw.end());
                return true;
            }
            catch (...) { return false; }
        }
    };

    template<typename T>
    Packet packetize(uint64_t objectId, const std::string& varName, const T& value, bool isTCP = false) {
        static_assert(std::is_trivially_copyable_v<T>, "POD required");
        Packet pkt; pkt.objectId = objectId; pkt.varName = varName; pkt.isTCP = isTCP;
        pkt.data.resize(sizeof(T));
        std::memcpy(pkt.data.data(), &value, sizeof(T));
        if (isTCP) {
            std::lock_guard<std::mutex> lock(packetMutexTCP_);
            packetBufferTCP_.push_back(pkt);
        }
        else {
            std::lock_guard<std::mutex> lock(packetMutexUDP_);
            packetBufferUDP_.push_back(pkt);
        }
        return pkt;
    }

    template<typename T>
    bool unpack(const Packet& pkt, T& out) {
        if (pkt.data.size() != sizeof(T)) return false;
        std::memcpy(&out, pkt.data.data(), sizeof(T));
        return true;
    }

    // ---------------- Control ----------------
    void startTCP();
    void startUDP();
    void stopNetwork();

    bool TCPConnect(const std::string& ipAddress, int serverPort, int clientPort);
    bool UDPConnect(const std::string& ipAddress, int serverPort, int clientPort);

    void setUDPPort(int p) { cfg_.udpPort = p; }
    void setTCPPort(int p) { cfg_.tcpPort = p; }

protected:
    Config cfg_;
    std::atomic<bool> running_;
    SOCKET udpSocket_, tcpSocket_;
    std::thread udpThread_, tcpThread_, tickThread_;

    struct Peer { sockaddr_in addr; SOCKET socket = INVALID_SOCKET; };
    std::mutex peersMutex_;
    std::unordered_map<std::string, Peer> peers_;

    std::mutex packetMutexUDP_, packetMutexTCP_;
    std::vector<Packet> packetBufferUDP_, packetBufferTCP_;

    virtual void WhenReceiveUDP(std::vector<Packet>& packets) {}
    virtual void WhenReceiveTCP(std::vector<Packet>& packets) {}
    virtual void onUDPTick() {}
    virtual void onTCPTick() {}

private:
    void makeNonBlocking(SOCKET s) { u_long mode = 1; ioctlsocket(s, FIONBIO, &mode); }
    std::string addrToKey(const sockaddr_in& addr) {
        char buf[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
    }

    void tickLoop() {
        using clock = std::chrono::steady_clock;
        auto tickInterval = std::chrono::milliseconds(1000 / cfg_.tickRate);
        while (running_) {
            auto start = clock::now();
            privatetic();
            auto elapsed = clock::now() - start;
            if (elapsed < tickInterval)
                std::this_thread::sleep_for(tickInterval - elapsed);
        }
    }

    void privatetic() {
        // send UDP
        std::vector<Packet> sendUDP;
        {
            std::lock_guard<std::mutex> lock(packetMutexUDP_);
            sendUDP = std::move(packetBufferUDP_);
            packetBufferUDP_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(peersMutex_);
            for (auto& pkt : sendUDP) {
                for (auto& [key, peer] : peers_) {
                    if (peer.socket != INVALID_SOCKET && !pkt.isTCP) {
                        sendto(peer.socket, pkt.serialize().c_str(), (int)pkt.serialize().size(), 0,
                            (sockaddr*)&peer.addr, sizeof(peer.addr));
                    }
                }
            }
        }
        if (!sendUDP.empty()) WhenReceiveUDP(sendUDP);

        // send TCP
        std::vector<Packet> sendTCP;
        {
            std::lock_guard<std::mutex> lock(packetMutexTCP_);
            sendTCP = std::move(packetBufferTCP_);
            packetBufferTCP_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(peersMutex_);
            for (auto& pkt : sendTCP) {
                for (auto& [key, peer] : peers_) {
                    if (peer.socket != INVALID_SOCKET && pkt.isTCP) {
                        send(peer.socket, pkt.serialize().c_str(), (int)pkt.serialize().size(), 0);
                    }
                }
            }
        }
        if (!sendTCP.empty()) WhenReceiveTCP(sendTCP);

        onUDPTick(); onTCPTick();
    }
};

// ---------------------- startUDP ----------------------
inline void TwentyFiveNetwork::startUDP() {
    if (cfg_.udpPort <= 0) return;
    udpSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket_ == INVALID_SOCKET) throw std::runtime_error("UDP socket failed");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.udpPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udpSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
        throw std::runtime_error("UDP bind failed");

    makeNonBlocking(udpSocket_);
    running_ = true;

    udpThread_ = std::thread([this] {
        char buf[1024];
        sockaddr_in from; int len = sizeof(from);
        while (running_) {
            int bytes = recvfrom(udpSocket_, buf, sizeof(buf), 0, (sockaddr*)&from, &len);
            if (bytes > 0) {
                Packet pkt;
                if (Packet::deserialize(std::string(buf, bytes), pkt)) {
                    std::lock_guard<std::mutex> lock(packetMutexUDP_);
                    packetBufferUDP_.push_back(pkt);

                    // add unknown peer
                    std::string key = addrToKey(from);
                    std::lock_guard<std::mutex> lockPeers(peersMutex_);
                    if (peers_.find(key) == peers_.end()) {
                        Peer p; p.addr = from; p.socket = udpSocket_;
                        peers_[key] = p;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        });

    if (!tickThread_.joinable())
        tickThread_ = std::thread(&TwentyFiveNetwork::tickLoop, this);

    std::cout << "[UDP] Listening on port " << cfg_.udpPort << "\n";
}

// ---------------------- startTCP ----------------------
inline void TwentyFiveNetwork::startTCP() {
    if (cfg_.tcpPort <= 0) return;
    tcpSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcpSocket_ == INVALID_SOCKET) throw std::runtime_error("TCP socket failed");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.tcpPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(tcpSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
        throw std::runtime_error("TCP bind failed");

    if (listen(tcpSocket_, SOMAXCONN) == SOCKET_ERROR)
        throw std::runtime_error("TCP listen failed");

    makeNonBlocking(tcpSocket_);
    running_ = true;

    tcpThread_ = std::thread([this] {
        while (running_) {
            SOCKET clientSock = accept(tcpSocket_, nullptr, nullptr);
            if (clientSock != INVALID_SOCKET) {
                makeNonBlocking(clientSock);
                sockaddr_in addr{}; int len = sizeof(addr);
                getpeername(clientSock, (sockaddr*)&addr, &len);
                std::string key = addrToKey(addr);
                std::lock_guard<std::mutex> lock(peersMutex_);
                peers_[key] = Peer{ addr, clientSock };

                std::thread([this, clientSock, key] {
                    char buf[1024];
                    while (running_) {
                        int bytes = recv(clientSock, buf, sizeof(buf), 0);
                        if (bytes > 0) {
                            Packet pkt;
                            if (Packet::deserialize(std::string(buf, bytes), pkt)) {
                                std::lock_guard<std::mutex> lock(packetMutexTCP_);
                                packetBufferTCP_.push_back(pkt);
                            }
                        }
                        else if (bytes == 0 || WSAGetLastError() != WSAEWOULDBLOCK) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    closesocket(clientSock);
                    std::lock_guard<std::mutex> lock(peersMutex_);
                    peers_.erase(key);
                    }).detach();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        });

    if (!tickThread_.joinable())
        tickThread_ = std::thread(&TwentyFiveNetwork::tickLoop, this);

    std::cout << "[TCP] Listening on port " << cfg_.tcpPort << "\n";
}

// ---------------------- stopNetwork ----------------------
inline void TwentyFiveNetwork::stopNetwork() {
    running_ = false;
    if (udpThread_.joinable()) udpThread_.join();
    if (tcpThread_.joinable()) tcpThread_.join();
    if (tickThread_.joinable()) tickThread_.join();

    if (udpSocket_ != INVALID_SOCKET) { closesocket(udpSocket_); udpSocket_ = INVALID_SOCKET; }
    if (tcpSocket_ != INVALID_SOCKET) { closesocket(tcpSocket_); tcpSocket_ = INVALID_SOCKET; }

    std::lock_guard<std::mutex> lock(peersMutex_);
    for (auto& [key, peer] : peers_)
        if (peer.socket != INVALID_SOCKET) closesocket(peer.socket);
    peers_.clear();

    std::cout << "[Network] Stopped all sockets and threads\n";
}

// ---------------------- TCPConnect ----------------------
inline bool TwentyFiveNetwork::TCPConnect(const std::string& ipAddress, int serverPort, int clientPort) {
    cfg_.tcpPort = clientPort;
    startTCP();

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(serverPort);
    inet_pton(AF_INET, ipAddress.c_str(), &server.sin_addr);

    if (connect(sock, (sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
        return false;

    Peer p; p.addr = server; p.socket = sock;
    std::lock_guard<std::mutex> lock(peersMutex_);
    peers_[ipAddress + ":" + std::to_string(serverPort)] = p;
    return true;
}

// ---------------------- UDPConnect ----------------------
inline bool TwentyFiveNetwork::UDPConnect(const std::string& ipAddress, int serverPort, int clientPort) {
    cfg_.udpPort = clientPort;
    startUDP();

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(serverPort);
    inet_pton(AF_INET, ipAddress.c_str(), &server.sin_addr);

    connect(sock, (sockaddr*)&server, sizeof(server));

    Peer p; p.addr = server; p.socket = sock;
    std::lock_guard<std::mutex> lock(peersMutex_);
    peers_[ipAddress + ":" + std::to_string(serverPort)] = p;
    return true;
}
