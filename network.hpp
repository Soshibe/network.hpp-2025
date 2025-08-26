#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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
        bool whitelisted = 0; // if set, only allow explicitly whitelisted peers
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
        std::string peerIp; // IP address of the peer who sent the packet

        std::string serialize() const {
            std::string payload = std::to_string(objectId) + "|" + varName + "|";
            payload.append(reinterpret_cast<const char*>(data.data()), data.size());
            uint32_t len = static_cast<uint32_t>(payload.size());
            uint32_t netlen = htonl(len);
            std::string out(reinterpret_cast<const char*>(&netlen), sizeof(netlen));
            out += payload;
            return out;
        }

        // Existing deserialize for std::string (kept for compatibility)
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

        // Optimized deserialize for a raw buffer
        static bool deserialize(const char* raw, size_t size, Packet& out, size_t& consumed) {
            std::string temp(raw, size);
            size_t sep1 = temp.find('|');
            if (sep1 == std::string::npos) return false;

            size_t sep2 = temp.find('|', sep1 + 1);
            if (sep2 == std::string::npos) return false;

            try {
                out.objectId = std::stoull(temp.substr(0, sep1));
                out.varName = temp.substr(sep1 + 1, sep2 - sep1 - 1);
                out.data.assign(temp.begin() + sep2 + 1, temp.end());
                consumed = sep2 + 1 + out.data.size();
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
            if (packetMutexTCP_.try_lock()) {
                packetBufferTCP_.push_back(pkt);
                packetMutexTCP_.unlock();
            }
        }
        else {
            if (packetMutexUDP_.try_lock()) {
                packetBufferUDP_.push_back(pkt);
                packetMutexUDP_.unlock();
            }
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

    void whitelistIp(const std::string& ip) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        whitelist_.insert(ip);
    }
    void blacklistIp(const std::string& ip) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        blacklist_.insert(ip);
    }
    void removeWhitelist(const std::string& ip) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        whitelist_.erase(ip);
    }
    void removeBlacklist(const std::string& ip) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        blacklist_.erase(ip);
    }

protected:
    Config cfg_;
    std::atomic<bool> running_;
    SOCKET udpSocket_, tcpSocket_;
    std::thread udpThread_, tcpThread_, tickThread_;

    struct Peer {
        sockaddr_in addr;
        SOCKET socket = INVALID_SOCKET;
        bool isTCP = false;
    };
    std::mutex peersMutex_;
    std::unordered_map<std::string, Peer> peers_;

    std::mutex packetMutexUDP_, packetMutexTCP_;
    std::vector<Packet> packetBufferUDP_, packetBufferTCP_;

    // Whitelist/Blacklist containers
    std::unordered_set<std::string> whitelist_;
    std::unordered_set<std::string> blacklist_;

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
                    if (!peer.isTCP && peer.socket != INVALID_SOCKET && !pkt.isTCP) {
                        std::string data = pkt.serialize();
                        sendto(peer.socket, data.c_str(), (int)data.size(), 0,
                            (sockaddr*)&peer.addr, sizeof(peer.addr));
                    }
                }
            }
        }

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
                    if (peer.isTCP && peer.socket != INVALID_SOCKET && pkt.isTCP) {
                        std::string data = pkt.serialize();
                        send(peer.socket, data.c_str(), (int)data.size(), 0);
                    }
                }
            }
        }
        // REMOVED: if (!sendTCP.empty()) WhenReceiveTCP(sendTCP);

        onUDPTick(); onTCPTick();
    }

    // --- REFACTORED MEMBERS ---
    // A single, reusable buffer and packet for the UDP listener.
    // This is safe because only one thread uses it.
    char udpBuf[1024];
    Packet udpPkt;

    // A ring buffer implementation to avoid reallocations.
    struct RingBuffer {
        std::vector<char> buffer;
        size_t write_idx = 0;
        size_t read_idx = 0;

        RingBuffer(size_t size) : buffer(size) {}

        size_t write(const char* data, size_t size) {
            size_t available_space = buffer.size() - (write_idx - read_idx);
            if (size > available_space) return 0;

            for (size_t i = 0; i < size; ++i) {
                buffer[(write_idx + i) % buffer.size()] = data[i];
            }
            write_idx += size;
            return size;
        }

        size_t peek(char* out, size_t size) const {
            if (size > write_idx - read_idx) return 0;

            for (size_t i = 0; i < size; ++i) {
                out[i] = buffer[(read_idx + i) % buffer.size()];
            }
            return size;
        }

        void advance(size_t size) {
            if (size <= write_idx - read_idx) {
                read_idx += size;
            }
        }

        size_t size() const {
            return write_idx - read_idx;
        }
    };

    // A struct to hold per-connection data for TCP.
    struct TcpConnectionData {
        char buf[1024]; // Used only for recv, data is then moved to ring buffer
        Packet pkt;
        RingBuffer ringBuffer{ 4096 }; // A sufficiently large ring buffer
    };

    // A map to store a unique buffer and packet for each TCP client.
    // This is crucial for thread safety, as each client thread gets its own data.
    std::unordered_map<SOCKET, TcpConnectionData> tcpConnections;
    std::mutex tcpConnectionsMutex_;
    // -------------------------
};

// ---------------------- startUDP ----------------------
inline void TwentyFiveNetwork::startUDP() {
    if (cfg_.udpPort <= 0) return;
    udpSocket_ = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket_ == INVALID_SOCKET) throw std::runtime_error("UDP socket failed");

    int off = 0;
    setsockopt(udpSocket_, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&off, sizeof(off));

    sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(cfg_.udpPort);
    addr6.sin6_addr = in6addr_any;

    if (bind(udpSocket_, (sockaddr*)&addr6, sizeof(addr6)) == SOCKET_ERROR)
        throw std::runtime_error("UDP bind failed");

    makeNonBlocking(udpSocket_);
    running_ = true;
    udpThread_ = std::thread([this] {
        sockaddr_storage from_storage; int len = sizeof(from_storage);
        while (running_) {
            int bytes = recvfrom(udpSocket_, udpBuf, sizeof(udpBuf), 0, (sockaddr*)&from_storage, &len);
            if (bytes > 4) { // Must be at least 4 bytes for length
                uint32_t netlen = 0;
                std::memcpy(&netlen, udpBuf, 4);
                uint32_t pktLen = ntohl(netlen);
                if (bytes - 4 < (int)pktLen) continue; // Incomplete packet
                std::string payload(udpBuf + 4, udpBuf + 4 + pktLen);
                size_t consumed = 0;
                if (Packet::deserialize(payload, udpPkt)) {
                    udpPkt.isTCP = false;
                    char buf[INET6_ADDRSTRLEN] = {};
                    std::string ipStr;
                    if (from_storage.ss_family == AF_INET) {
                        sockaddr_in* from = (sockaddr_in*)&from_storage;
                        inet_ntop(AF_INET, &from->sin_addr, buf, sizeof(buf));
                        ipStr = buf;
                    } else if (from_storage.ss_family == AF_INET6) {
                        sockaddr_in6* from6 = (sockaddr_in6*)&from_storage;
                        inet_ntop(AF_INET6, &from6->sin6_addr, buf, sizeof(buf));
                        ipStr = buf;
                    }
                    udpPkt.peerIp = ipStr;
                    // Whitelist/Blacklist check
                    bool allowed = true;
                    {
                        std::lock_guard<std::mutex> lock(this->peersMutex_);
                        if (this->cfg_.whitelisted) {
                            allowed = this->whitelist_.count(ipStr) > 0 && this->blacklist_.count(ipStr) == 0;
                        } else {
                            allowed = this->blacklist_.count(ipStr) == 0;
                        }
                    }
                    if (!allowed) continue;
                    {
                        std::lock_guard<std::mutex> lock(this->packetMutexUDP_);
                        this->packetBufferUDP_.push_back(udpPkt);
                    }
                    std::vector<Packet> singlePacket{ udpPkt };
                    this->WhenReceiveUDP(singlePacket);

                    static std::string key = this->addrToKey(*(sockaddr_in*)&from_storage); // fallback for peer map
                    std::lock_guard<std::mutex> lockPeers(this->peersMutex_);
                    if (this->peers_.find(key) == this->peers_.end()) {
                        Peer p; p.addr = *(sockaddr_in*)&from_storage; p.socket = this->udpSocket_;
                        this->peers_[key] = p;
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
    tcpSocket_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (tcpSocket_ == INVALID_SOCKET) throw std::runtime_error("TCP socket failed");

    int off = 0;
    setsockopt(tcpSocket_, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&off, sizeof(off));

    sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(cfg_.tcpPort);
    addr6.sin6_addr = in6addr_any;

    if (bind(tcpSocket_, (sockaddr*)&addr6, sizeof(addr6)) == SOCKET_ERROR)
        throw std::runtime_error("TCP bind failed");

    if (listen(tcpSocket_, SOMAXCONN) == SOCKET_ERROR)
        throw std::runtime_error("TCP listen failed");

    makeNonBlocking(tcpSocket_);
    running_ = true;

    tcpThread_ = std::thread([this] {
        while (running_) {
            sockaddr_storage peer_storage; int len = sizeof(peer_storage);
            SOCKET clientSock = accept(tcpSocket_, (sockaddr*)&peer_storage, &len);
            if (clientSock != INVALID_SOCKET) {
                makeNonBlocking(clientSock);
                std::string key;
                char buf[INET6_ADDRSTRLEN] = {};
                if (peer_storage.ss_family == AF_INET) {
                    sockaddr_in* peerAddr = (sockaddr_in*)&peer_storage;
                    key = addrToKey(*peerAddr);
                } else if (peer_storage.ss_family == AF_INET6) {
                    sockaddr_in6* peerAddr6 = (sockaddr_in6*)&peer_storage;
                    inet_ntop(AF_INET6, &peerAddr6->sin6_addr, buf, sizeof(buf));
                    key = std::string(buf) + ":" + std::to_string(ntohs(peerAddr6->sin6_port));
                }

                {
                    std::lock_guard<std::mutex> lock(tcpConnectionsMutex_);
                    tcpConnections[clientSock] = TcpConnectionData();
                }

                std::lock_guard<std::mutex> peersLock(peersMutex_);
                if (peer_storage.ss_family == AF_INET) {
                    peers_[key] = Peer{ *(sockaddr_in*)&peer_storage, clientSock, true };
                } else if (peer_storage.ss_family == AF_INET6) {
                    sockaddr_in6* peerAddr6 = (sockaddr_in6*)&peer_storage;
                    sockaddr_in peerAddr4{};
                    peerAddr4.sin_family = AF_INET;
                    peerAddr4.sin_addr.s_addr = 0;
                    peerAddr4.sin_port = peerAddr6->sin6_port;
                    peers_[key] = Peer{ peerAddr4, clientSock, true };
                }

                std::thread([this, clientSock, key, peer_storage] {
                    TcpConnectionData* connData = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(this->tcpConnectionsMutex_);
                        auto it = this->tcpConnections.find(clientSock);
                        if (it != this->tcpConnections.end()) {
                            connData = &it->second;
                        }
                    }

                    std::vector<char> tcpBuffer;
                    while (this->running_ && connData) {
                        char tempBuf[1024];
                        int bytes = recv(clientSock, tempBuf, sizeof(tempBuf), 0);
                        if (bytes > 0) {
                            tcpBuffer.insert(tcpBuffer.end(), tempBuf, tempBuf + bytes);
                            // Process all complete packets in the buffer
                            while (tcpBuffer.size() >= 4) {
                                uint32_t netlen = 0;
                                std::memcpy(&netlen, tcpBuffer.data(), 4);
                                uint32_t pktLen = ntohl(netlen);
                                if (tcpBuffer.size() < 4 + pktLen) break; // Wait for full packet
                                std::string payload(tcpBuffer.begin() + 4, tcpBuffer.begin() + 4 + pktLen);
                                Packet tempPkt;
                                size_t consumed = 0;
                                if (Packet::deserialize(payload, tempPkt)) {
                                    tempPkt.isTCP = true;
                                    char buf[INET6_ADDRSTRLEN] = {};
                                    std::string ipStr;
                                    if (peer_storage.ss_family == AF_INET) {
                                        sockaddr_in* peerAddr = (sockaddr_in*)&peer_storage;
                                        inet_ntop(AF_INET, &peerAddr->sin_addr, buf, sizeof(buf));
                                        ipStr = buf;
                                    } else if (peer_storage.ss_family == AF_INET6) {
                                        sockaddr_in6* peerAddr6 = (sockaddr_in6*)&peer_storage;
                                        inet_ntop(AF_INET6, &peerAddr6->sin6_addr, buf, sizeof(buf));
                                        ipStr = buf;
                                    }
                                    tempPkt.peerIp = ipStr;
                                    // Whitelist/Blacklist check
                                    bool allowed = true;
                                    {
                                        std::lock_guard<std::mutex> lock(this->peersMutex_);
                                        if (this->cfg_.whitelisted) {
                                            allowed = this->whitelist_.count(ipStr) > 0 && this->blacklist_.count(ipStr) == 0;
                                        } else {
                                            allowed = this->blacklist_.count(ipStr) == 0;
                                        }
                                    }
                                    if (!allowed) {
                                        tcpBuffer.erase(tcpBuffer.begin(), tcpBuffer.begin() + 4 + pktLen);
                                        continue;
                                    }
                                    {
                                        std::lock_guard<std::mutex> lock(this->packetMutexTCP_);
                                        this->packetBufferTCP_.push_back(tempPkt);
                                    }
                                    std::vector<Packet> singlePacket{ tempPkt };
                                    this->WhenReceiveTCP(singlePacket);
                                }
                                tcpBuffer.erase(tcpBuffer.begin(), tcpBuffer.begin() + 4 + pktLen);
                            }
                        }
                        else if (bytes == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }

                    closesocket(clientSock);
                    std::lock_guard<std::mutex> peersLock(this->peersMutex_);
                    this->peers_.erase(key);
                    std::lock_guard<std::mutex> dataLock(this->tcpConnectionsMutex_);
                    this->tcpConnections.erase(clientSock);
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

    Peer p; p.addr = server; p.socket = sock; p.isTCP = true;
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