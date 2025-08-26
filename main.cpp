#include "network.hpp"
#include <thread>
#include <chrono>
#include <iostream>

// ---------------- SERVER ----------------
class ServerNetwork : public TwentyFiveNetwork {
public:
    using TwentyFiveNetwork::TwentyFiveNetwork;

protected:
    void handleUDPMessage(const std::string& msg, const sockaddr_in& from, long driftMs) override {
        std::cout << "[Server] Got message: " << msg
            << " (drift=" << driftMs << "ms)\n";

        // Echo back
        std::string key = addrToKey(from);
        sendUDPMessage("Echo: " + msg, key);
    }

    void onUDPTick() override {
        // Server tick logic
    }
};

// ---------------- CLIENT ----------------
class ClientNetwork : public TwentyFiveNetwork {
public:
    using TwentyFiveNetwork::TwentyFiveNetwork;

    void connectToServer(const std::string& host, int port) {
        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &server.sin_addr);

        serverKey_ = addrToKey(server);
        {
            std::lock_guard<std::mutex> lock(peersMutex_);
            peers_[serverKey_] = Peer{
                std::chrono::steady_clock::now(),
                std::chrono::steady_clock::now(),
                server
            };
        }
    }

    void sendTestMessage(const std::string& text) {
        if (!serverKey_.empty()) {
            sendUDPMessage(text, serverKey_);
        }
    }

protected:
    void handleUDPMessage(const std::string& msg, const sockaddr_in& from, long driftMs) override {
        std::cout << "[Client] Got reply: " << msg
            << " (drift=" << driftMs << "ms)\n";
    }

private:
    std::string serverKey_;
};

// ---------------- MAIN ----------------
int main() {
    // Config for server and client
    TwentyFiveNetwork::Config serverCfg;
    serverCfg.udpPort = 5000; // server listens on 5000
    serverCfg.tickRate = 10;

    TwentyFiveNetwork::Config clientCfg;
    clientCfg.udpPort = 5001; // client listens on 5001
    clientCfg.tickRate = 10;

    ServerNetwork server(serverCfg);
    ClientNetwork client(clientCfg);

    // Start both
    server.start();
    client.start();

    // Delay before connecting
    std::this_thread::sleep_for(std::chrono::seconds(1));
    client.connectToServer("127.0.0.1", serverCfg.udpPort);

    // Send test message
    std::this_thread::sleep_for(std::chrono::seconds(1));
    client.sendTestMessage("Hello from Client!");

    // Let them talk a bit
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Stop everything
    client.stop();
    server.stop();

    return 0;
}
