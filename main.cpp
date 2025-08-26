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
                std::cout << "  Object " << pkt.objectId << " " << pkt.varName << "=" << val << "from: " << pkt.peerIp << "\n";
				blacklistIp(pkt.peerIp); // Example: blacklist any UDP sender
				std::cout << "  Blacklisted IP: " << pkt.peerIp << " for pressing J. (Banned)\n";
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
				whitelistIp(pkt.peerIp); // Example: whitelist any TCP sender
				std::cout << "  Whitelisted IP: " << pkt.peerIp << " for pressing K.\n";
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
	serverCfg.tickRate = 20;
    //Whitelist is intended to be used clientside,
	// i.e. a metaserver that holds a list of allowed servers.
    // blacklist serverside for known bad clients.
    serverCfg.whitelisted = false;
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
    int lastTick = 0;
    while (running) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { // ESC to quit
            running = false;
            break;
        }

        // Only send once per new tick
        int currentTick = 0;
        {
            // Use the server's tickCount as a reference (simulate tick sync)
            // In a real client, you would sync with the server or have a tick callback
            static int fakeTick = 0;
            static auto last = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() > 50) {
                fakeTick++;
                last = now;
            }
            currentTick = fakeTick;
        }
        if (currentTick != lastTick) {
            for (auto& [vk, state] : keys) {
                bool currentlyDown = GetAsyncKeyState(vk) & 0x8000;
                uint64_t objId = (vk == 'K') ? 1 : 2;
                bool useTcp = (vk == 'K');

                if (currentlyDown && !state.lastDown) {
                    // Keydown event
                    client.sendKey(objId, "keydown", 1, useTcp);
                    std::cout << "[Client] Sent keydown for " << char(vk) << (useTcp ? " (TCP)" : " (UDP)") << "\n";
                }
                else if (!currentlyDown && state.lastDown) {
                    // Keyup event
                    client.sendKey(objId, "keyup", 1, useTcp);
                    std::cout << "[Client] Sent keyup for " << char(vk) << (useTcp ? " (TCP)" : " (UDP)") << "\n";
                }

                state.lastDown = currentlyDown;
            }
            lastTick = currentTick;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    client.stopNetwork();
    server.stopNetwork();
    return 0;
}
