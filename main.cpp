#include "network.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <windows.h> // for GetAsyncKeyState

class ServerNetwork : public TwentyFiveNetwork {
public:
    ServerNetwork(const Config& cfg) : TwentyFiveNetwork(cfg), tickCount(0) {}

protected:
    int tickCount;

    void WhenReceiveUDP(std::vector<Packet>& packets) override {
        if (!packets.empty()) {
            std::cout << "[Server] Tick " << tickCount << " received UDP packets:\n";
            for (auto& pkt : packets) {
                int val;
                unpack(pkt, val);
                std::cout << "  Object " << pkt.objectId << " " << pkt.varName << "=" << val << "\n";
            }
        }
    }

    void WhenReceiveTCP(std::vector<Packet>& packets) override {
        if (!packets.empty()) {
            std::cout << "[Server] Tick " << tickCount << " received TCP packets:\n";
            for (auto& pkt : packets) {
                int val;
                unpack(pkt, val);
                std::cout << "  Object " << pkt.objectId << " " << pkt.varName << "=" << val << "\n";
            }
        }
    }

    void onUDPTick() override { tickCount++; }
};

class ClientNetwork : public TwentyFiveNetwork {
public:
    ClientNetwork(const Config& cfg) : TwentyFiveNetwork(cfg) {}

    void sendKey(uint64_t objId, const std::string& keyVar, int value, bool tcp = false) {
        packetize(objId, keyVar, value, tcp);
    }
};

int main() {
    // Server setup
    ServerNetwork::Config serverCfg;
    serverCfg.udpPort = 5000;
    serverCfg.tcpPort = 5001;
    ServerNetwork server(serverCfg);
    server.startUDP();
    server.startTCP();
    std::cout << "Server started.\n";

    // Client setup
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ClientNetwork::Config clientCfg;
    ClientNetwork client(clientCfg);
    client.TCPConnect("127.0.0.1", 5001, 6001);
    client.UDPConnect("127.0.0.1", 5000, 6000);
    std::cout << "Client connected.\n";

    struct KeyState {
        bool lastDown = false; // previous tick state
    };

    std::unordered_map<int, KeyState> keys = {
        { 'K', KeyState{} }, // Object 1 key
        { 'J', KeyState{} }  // Object 2 key
    };

    bool running = true;
    while (running) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { // ESC to quit
            running = false;
            break;
        }

        for (auto& [vk, state] : keys) {
            bool currentlyDown = GetAsyncKeyState(vk) & 0x8000;
            uint64_t objId = (vk == 'K') ? 1 : 2;

            if (currentlyDown && !state.lastDown) {
                // Keydown event
                client.sendKey(objId, "keydown", 1);
                std::cout << "[Client] Sent keydown for " << char(vk) << "\n";
            }
            else if (!currentlyDown && state.lastDown) {
                // Keyup event
                client.sendKey(objId, "keyup", 1);
                std::cout << "[Client] Sent keyup for " << char(vk) << "\n";
            }

            state.lastDown = currentlyDown;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    client.stopNetwork();
    server.stopNetwork();
    return 0;
}
