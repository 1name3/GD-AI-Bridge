#include <Geode/Geode.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <thread>
#include <string>

#pragma comment(lib, "Ws2_32.lib")

using namespace geode::prelude;

namespace GDAIBridge {

    constexpr const char* HOST = "127.0.0.1";
    constexpr int PORT = 8765;

    SOCKET g_socket = INVALID_SOCKET;
    bool g_connected = false;

    void bridgeThread() {
        WSADATA wsaData;

        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            log::error("[GD AI Bridge] WSAStartup failed");
            return;
        }

        g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (g_socket == INVALID_SOCKET) {
            log::error("[GD AI Bridge] Could not create socket");
            WSACleanup();
            return;
        }

        sockaddr_in serverAddress{};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(PORT);

        if (inet_pton(AF_INET, HOST, &serverAddress.sin_addr) != 1) {
            log::error("[GD AI Bridge] Invalid server address");
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            WSACleanup();
            return;
        }

        log::info("[GD AI Bridge] Connecting to {}:{}...", HOST, PORT);

        if (connect(
            g_socket,
            reinterpret_cast<sockaddr*>(&serverAddress),
            sizeof(serverAddress)
        ) == SOCKET_ERROR) {

            log::warn(
                "[GD AI Bridge] Could not connect to {}:{}",
                HOST,
                PORT
            );

            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            WSACleanup();
            return;
        }

        g_connected = true;

        log::info("[GD AI Bridge] Connected to Python AI Trainer!");

        std::string message =
            R"({"type":"hello","game":"Geometry Dash","mod":"GD AI Bridge","gd_version":"2.2081"})"
            "\n";

        int result = send(
            g_socket,
            message.c_str(),
            static_cast<int>(message.size()),
            0
        );

        if (result == SOCKET_ERROR) {
            log::error("[GD AI Bridge] Failed to send hello message");
        }
        else {
            log::info("[GD AI Bridge] Hello message sent!");
        }

        // Schritt 1 ist absichtlich nur ein Verbindungstest.
        // Die Verbindung bleibt offen, damit wir sie später
        // für Game-State-Daten verwenden können.
    }

    void start() {
        std::thread(bridgeThread).detach();
    }

}

$on_mod(Loaded) {
    log::info("========================================");
    log::info("GD AI Bridge loaded successfully!");
    log::info("Starting TCP bridge...");
    log::info("Target: 127.0.0.1:8765");
    log::info("========================================");

    GDAIBridge::start();
}
