#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

using namespace geode::prelude;

namespace GDABridge {

static constexpr const char* HOST = "127.0.0.1";
static constexpr int PORT = 8765;
static constexpr int PROTOCOL = 4;

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_pythonResponsive{false};
static std::atomic<bool> g_debugBoxes{false};
static std::atomic<bool> g_noclip{false};
static std::atomic<bool> g_speedhack{false};
static std::atomic<bool> g_practice{false};
static std::atomic<bool> g_inLevel{false};
static std::atomic<bool> g_needLevelReport{false};
static std::atomic<float> g_speed{1.0f};
static std::atomic<int> g_stateHz{30};
static std::atomic<unsigned long> g_connectionCount{0};
static std::atomic<unsigned long> g_disconnectCount{0};
static std::atomic<int> g_debugFPS{30};
static std::atomic<int> g_debugDrawnObjects{0};
static std::atomic<int> g_debugVisibleObjects{0};
static constexpr int OBJECT_HZ = 5;
static constexpr float OBJECT_LOOK_BEHIND = 280.f;
static constexpr float OBJECT_LOOK_AHEAD = 2200.f;
static constexpr int OBJECT_MAX_SENT = 120;
static std::atomic<int> g_objectPackets{0};
static int g_lastReportedLevelID = -1;
static std::string g_lastReportedLevelName;
static std::atomic<bool> g_lastDebugActive{false};

static SOCKET g_socket = INVALID_SOCKET;
static std::mutex g_socketMutex;
static std::mutex g_commandMutex;
static std::vector<std::string> g_commands;

static std::mutex g_outgoingMutex;
static std::condition_variable g_outgoingCv;
static std::deque<std::string> g_outgoing;
static constexpr size_t MAX_OUTGOING = 128;
static std::thread g_networkThread;
static std::thread g_senderThread;

static CCNode* g_debugNode = nullptr;

static float g_lastP1X = 0.f;
static float g_lastP1Y = 0.f;
static float g_lastP2X = 0.f;
static float g_lastP2Y = 0.f;
static bool g_haveP1 = false;
static bool g_haveP2 = false;

static float g_naturalTimeWarp = 1.0f;
static std::chrono::steady_clock::time_point g_levelStart = std::chrono::steady_clock::now();
static std::chrono::steady_clock::time_point g_lastState = std::chrono::steady_clock::now();
static std::chrono::steady_clock::time_point g_lastObjects = std::chrono::steady_clock::now();
static std::vector<GameObject*> g_sortedObjects;
static int g_cachedLevelID = -1;

static void clearOutgoing();

static thread_local bool g_injectedInput = false;

static std::string escapeJson(const std::string& value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '"': out += "\""; break;
            case '\\': out += "\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string fjson(float value) {
    std::ostringstream s;
    s.setf(std::ios::fixed);
    s.precision(4);
    s << value;
    return s.str();
}

static std::string bjson(bool value) {
    return value ? "true" : "false";
}

static void closeSocket() {
    std::lock_guard<std::mutex> lock(g_socketMutex);
    const bool wasConnected = g_connected.load();
    if (g_socket != INVALID_SOCKET) {
        shutdown(g_socket, SD_BOTH);
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    g_connected = false;
    g_pythonResponsive = false;
    clearOutgoing();
    if (wasConnected) {
        g_disconnectCount.fetch_add(1);
    }
}

static bool sendRaw(const std::string& payload) {
    std::lock_guard<std::mutex> lock(g_socketMutex);
    if (g_socket == INVALID_SOCKET) return false;

    size_t total = 0;
    while (total < payload.size()) {
        int sent = send(
            g_socket,
            payload.data() + total,
            static_cast<int>(payload.size() - total),
            0
        );
        if (sent <= 0) return false;
        total += static_cast<size_t>(sent);
    }
    return true;
}

static bool isTelemetryJson(const std::string& payload) {
    return payload.find("{\"type\":\"state\"") == 0 ||
        payload.find("{\"type\":\"objects\"") == 0 ||
        payload.find("{\"type\":\"debug_status\"") == 0;
}

static void dropOldTelemetryLocked() {
    for (auto it = g_outgoing.begin(); it != g_outgoing.end(); ++it) {
        if (isTelemetryJson(*it)) {
            g_outgoing.erase(it);
            return;
        }
    }
}

static bool queueJson(std::string payload) {
    payload.push_back('\n');
    {
        std::lock_guard<std::mutex> lock(g_outgoingMutex);

        // State and object telemetry are snapshots. Never allow stale snapshots
        // to build up when Python is busy or temporarily disconnected.
        if (payload.rfind("{\"type\":\"state\"", 0) == 0 ||
            payload.rfind("{\"type\":\"objects\"", 0) == 0) {
            for (auto it = g_outgoing.begin(); it != g_outgoing.end();) {
                const bool sameType =
                    (payload.rfind("{\"type\":\"state\"", 0) == 0 && it->rfind("{\"type\":\"state\"", 0) == 0) ||
                    (payload.rfind("{\"type\":\"objects\"", 0) == 0 && it->rfind("{\"type\":\"objects\"", 0) == 0);
                if (sameType) {
                    it = g_outgoing.erase(it);
                } else {
                    ++it;
                }
            }
        }

        while (g_outgoing.size() >= MAX_OUTGOING) {
            dropOldTelemetryLocked();
            if (g_outgoing.size() >= MAX_OUTGOING) {
                g_outgoing.pop_front();
            }
        }

        g_outgoing.emplace_back(std::move(payload));
    }
    g_outgoingCv.notify_one();
    return true;
}

static bool sendJson(const std::string& json) {
    if (!g_connected.load()) return false;
    return queueJson(json);
}

static void clearOutgoing() {
    std::lock_guard<std::mutex> lock(g_outgoingMutex);
    g_outgoing.clear();
}

static void senderThread() {
    while (g_running) {
        std::string payload;
        {
            std::unique_lock<std::mutex> lock(g_outgoingMutex);
            g_outgoingCv.wait_for(lock, std::chrono::milliseconds(100), [] {
                return !g_running || !g_outgoing.empty();
            });
            if (!g_running) break;
            if (g_outgoing.empty()) continue;
            payload = std::move(g_outgoing.front());
            g_outgoing.pop_front();
        }

        if (!sendRaw(payload)) {
            clearOutgoing();
            closeSocket();
        }
    }
}

static void queueCommand(const std::string& command) {
    std::lock_guard<std::mutex> lock(g_commandMutex);
    g_commands.push_back(command);
}

static std::vector<std::string> takeCommands() {
    std::lock_guard<std::mutex> lock(g_commandMutex);
    auto result = g_commands;
    g_commands.clear();
    return result;
}

static bool contains(const std::string& text, const std::string& value) {
    return text.find(value) != std::string::npos;
}

static float getCommandFloat(const std::string& text, const char* key, float fallback) {
    const auto pos = text.find(key);
    if (pos == std::string::npos) return fallback;
    const auto start = pos + std::string(key).size();
    return std::strtof(text.c_str() + start, nullptr);
}

static int getCommandInt(const std::string& text, const char* key, int fallback) {
    const auto pos = text.find(key);
    if (pos == std::string::npos) return fallback;
    const auto start = pos + std::string(key).size();
    return std::atoi(text.c_str() + start);
}

static void sendCheatStatus() {
    sendJson(
        std::string("{\"type\":\"cheat_status\",\"protocol\":4,\"noclip\":") +
        bjson(g_noclip.load()) +
        ",\"speedhack\":" +
        bjson(g_speedhack.load()) +
        ",\"speed\":" +
        fjson(g_speed.load()) +
        ",\"practice\":" +
        bjson(g_practice.load()) +
        ",\"debug\":" +
        bjson(g_debugBoxes.load()) +
        "}"
    );
}

static void sendDebugStatus(bool force = false) {
    const bool active = g_debugBoxes.load() && g_inLevel.load();
    if (!force && active == g_lastDebugActive) return;
    g_lastDebugActive = active;

    sendJson(
        std::string("{\"type\":\"debug_status\",\"protocol\":4,\"enabled\":") +
        bjson(g_debugBoxes.load()) +
        ",\"active\":" + bjson(active) +
        ",\"drawn_objects\":" + std::to_string(g_debugDrawnObjects.load()) +
        ",\"visible_objects\":" + std::to_string(g_debugVisibleObjects.load()) +
        ",\"fps\":" + std::to_string(g_debugFPS.load()) +
        "}"
    );
}

static void sendCommandResult(const char* action, bool success, const char* reason = nullptr) {
    std::string packet =
        std::string("{\"type\":\"command_result\",\"action\":\"") +
        action +
        "\",\"success\":" +
        bjson(success);
    if (reason) {
        packet += std::string(",\"reason\":\"") + escapeJson(reason) + "\"";
    }
    packet += "}";
    sendJson(packet);
}

static void processCommand(const std::string& command) {
    log::info("[GD AI Bridge] RX command: {}", command);

    if (contains(command, "\"action\":\"ping\"")) {
        sendJson("{\"type\":\"pong\",\"protocol\":4}");
        g_pythonResponsive = true;
        return;
    }

    if (contains(command, "\"action\":\"debug_boxes\"")) {
        if (contains(command, "\"enabled\":true")) g_debugBoxes = true;
        if (contains(command, "\"enabled\":false")) g_debugBoxes = false;
        const int fps = std::max(5, std::min(30, getCommandInt(command, "\"fps\":", g_debugFPS.load())));
        g_debugFPS = fps;
        sendCommandResult("debug_boxes", true);
        sendDebugStatus(true);
        return;
    }

    if (contains(command, "\"action\":\"state_hz\"")) {
        int hz = getCommandInt(command, "\"value\":", g_stateHz.load());
        hz = std::max(1, std::min(120, hz));
        g_stateHz = hz;
        sendCommandResult("state_hz", true);
        return;
    }

    if (contains(command, "\"action\":\"noclip\"")) {
        g_noclip = contains(command, "\"enabled\":true");
        sendCommandResult("noclip", true);
        sendCheatStatus();
        log::info("[GD AI Bridge] Noclip {}", g_noclip.load() ? "ON" : "OFF");
        return;
    }

    if (contains(command, "\"action\":\"speedhack\"")) {
        g_speedhack = contains(command, "\"enabled\":true");
        float speed = getCommandFloat(command, "\"speed\":", 1.0f);
        speed = std::max(0.1f, std::min(4.0f, speed));
        g_speed = speed;
        sendCommandResult("speedhack", true);
        sendCheatStatus();
        log::info("[GD AI Bridge] Speedhack {} ({:.2f}x)", g_speedhack.load() ? "ON" : "OFF", g_speed.load());
        return;
    }

    if (contains(command, "\"action\":\"practice\"")) {
        g_practice = contains(command, "\"enabled\":true");
        if (auto* pl = PlayLayer::get()) {
            pl->m_isPracticeMode = g_practice.load();
        }
        sendCommandResult("practice", true);
        sendCheatStatus();
        log::info("[GD AI Bridge] Practice {}", g_practice.load() ? "ON" : "OFF");
        return;
    }

    if (contains(command, "\"action\":\"reset_cheats\"")) {
        g_noclip = false;
        g_speedhack = false;
        g_speed = 1.0f;
        g_practice = false;
        if (auto* pl = PlayLayer::get()) {
            pl->m_isPracticeMode = false;
            pl->m_gameState.m_timeWarp = g_naturalTimeWarp;
        }
        sendCommandResult("reset_cheats", true);
        sendCheatStatus();
        return;
    }

    if (contains(command, "\"action\":\"jump\"")) {
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) {
            sendCommandResult("jump", false, "no_player");
            return;
        }
        g_injectedInput = true;
        pl->handleButton(true, static_cast<int>(PlayerButton::Jump), true);
        g_injectedInput = false;
        sendCommandResult("jump", true);
        return;
    }

    if (contains(command, "\"action\":\"release\"")) {
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) {
            sendCommandResult("release", false, "no_player");
            return;
        }
        g_injectedInput = true;
        pl->handleButton(false, static_cast<int>(PlayerButton::Jump), true);
        g_injectedInput = false;
        sendCommandResult("release", true);
        return;
    }

    sendCommandResult("unknown", false, "unknown_command");
}

static void processPendingCommands() {
    for (const auto& command : takeCommands()) {
        processCommand(command);
    }
}

static void receiveLoop() {
    std::string buffer;
    char temp[8192];

    while (g_running) {
        SOCKET socketCopy;
        {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            socketCopy = g_socket;
        }
        if (socketCopy == INVALID_SOCKET) break;

        int received = recv(socketCopy, temp, sizeof(temp), 0);
        if (received <= 0) break;

        buffer.append(temp, received);

        while (true) {
            const auto newline = buffer.find('\n');
            if (newline == std::string::npos) break;

            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);

            if (line.empty()) continue;

            if (contains(line, "\"type\":\"pong\"")) {
                g_pythonResponsive = true;
                continue;
            }

            queueCommand(line);
        }
    }

    closeSocket();
}

static bool connectToServer() {
    SOCKET newSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (newSocket == INVALID_SOCKET) {
        log::error("[GD AI Bridge] socket() failed");
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);

    if (inet_pton(AF_INET, HOST, &address.sin_addr) != 1) {
        closesocket(newSocket);
        return false;
    }

    if (connect(
        newSocket,
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address)
    ) != 0) {
        closesocket(newSocket);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_socketMutex);
        if (g_socket != INVALID_SOCKET) {
            closesocket(newSocket);
            return false;
        }
        g_socket = newSocket;
        g_connected = true;
        g_pythonResponsive = false;
        g_connectionCount.fetch_add(1);
    }

    log::info("[GD AI Bridge] TCP CONNECTED {}:{}", HOST, PORT);

    sendJson(
        "{\"type\":\"hello\",\"protocol\":4,\"game\":\"Geometry Dash\",\"mod\":\"GD AI Bridge\",\"gd_version\":\"2.2081\",\"features\":[\"state\",\"objects\",\"input\",\"macro\",\"noclip\",\"speedhack\",\"practice\",\"debug\"]}"
    );
    sendJson("{\"type\":\"ping\",\"source\":\"gd_mod\"}");
    sendCheatStatus();
    sendDebugStatus(true);

    return true;
}

static void networkThread() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        log::error("[GD AI Bridge] WSAStartup failed");
        return;
    }

    log::info("[GD AI Bridge] Network thread started");

    while (g_running) {
        if (!g_connected) {
            if (connectToServer()) {
                receiveLoop();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    closeSocket();
    WSACleanup();
}

static double levelTimeSeconds() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_levelStart
    ).count();
}

static std::string playerJson(PlayerObject* player, float dt, int index) {
    if (!player) return "null";

    const float x = player->getPositionX();
    const float y = player->getPositionY();
    float vx = 0.f;
    float vy = 0.f;

    if (index == 1) {
        if (g_haveP1 && dt > 0.f) {
            vx = (x - g_lastP1X) / dt;
            vy = (y - g_lastP1Y) / dt;
        }
        g_lastP1X = x;
        g_lastP1Y = y;
        g_haveP1 = true;
    } else {
        if (g_haveP2 && dt > 0.f) {
            vx = (x - g_lastP2X) / dt;
            vy = (y - g_lastP2Y) / dt;
        }
        g_lastP2X = x;
        g_lastP2Y = y;
        g_haveP2 = true;
    }

    return
        "{\"x\":" + fjson(x) +
        ",\"y\":" + fjson(y) +
        ",\"vx\":" + fjson(vx) +
        ",\"vy\":" + fjson(vy) +
        ",\"rotation\":" + fjson(player->getRotation()) +
        ",\"scale\":" + fjson(player->getScale()) +
        ",\"dead\":" + bjson(player->m_isDead) +
        ",\"gravityFlipped\":" + bjson(player->m_isUpsideDown) +
        ",\"onGround\":" + bjson(player->m_isOnGround) +
        "}";
}

static bool isOrbID(int id) {
    switch (id) {
        case 36:   // Yellow Jump Orb
        case 84:   // Blue Gravity Orb
        case 141:  // Pink Jump Orb
        case 1330: // Black Drop Orb
        case 1333: // Red Jump Orb
        case 1704: // Green Dash Orb
        case 1751: // Pink Gravity Dash Orb
            return true;
        default:
            return false;
    }
}

static const char* objectKind(int id) {
    switch (id) {
        case 36: return "YELLOW_ORB";
        case 84: return "BLUE_ORB";
        case 141: return "PINK_ORB";
        case 1330: return "BLACK_ORB";
        case 1333: return "RED_ORB";
        case 1704: return "GREEN_DASH_ORB";
        case 1751: return "PINK_GRAVITY_DASH_ORB";
        case 35: return "YELLOW_PAD";
        case 67: return "BLUE_PAD";
        case 140: return "PINK_PAD";
        case 1332: return "RED_PAD";
        case 12: return "CUBE_PORTAL";
        case 13: return "SHIP_PORTAL";
        case 47: return "BALL_PORTAL";
        case 111: return "UFO_PORTAL";
        case 660: return "WAVE_PORTAL";
        case 745: return "ROBOT_PORTAL";
        case 1331: return "SPIDER_PORTAL";
        case 1931: return "SWING_PORTAL";
        default: return "OBJECT";
    }
}

static bool isInteractiveID(int id) {
    if (isOrbID(id)) return true;
    switch (id) {
        case 35: case 67: case 140: case 1332:
        case 12: case 13: case 47: case 111: case 660: case 745: case 1331: case 1931:
            return true;
        default:
            return false;
    }
}

static void rebuildObjectCache(PlayLayer* layer) {
    g_sortedObjects.clear();
    g_cachedLevelID = -1;
    if (!layer || !layer->m_objects || !layer->m_level) return;

    g_sortedObjects.reserve(layer->m_objects->count());
    for (auto object : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (object) g_sortedObjects.push_back(object);
    }

    std::stable_sort(g_sortedObjects.begin(), g_sortedObjects.end(), [](GameObject* a, GameObject* b) {
        return a->getPositionX() < b->getPositionX();
    });
    g_cachedLevelID = layer->m_level->m_levelID;
}

static std::string oneObjectJson(GameObject* object, float playerX) {
    const auto rect = object->getObjectRect();
    const float distance = rect.origin.x + rect.size.width * 0.5f - playerX;
    const int id = object->m_objectID;
    return
        "{\"id\":" + std::to_string(id) +
        ",\"x\":" + fjson(object->getPositionX()) +
        ",\"y\":" + fjson(object->getPositionY()) +
        ",\"w\":" + fjson(rect.size.width) +
        ",\"h\":" + fjson(rect.size.height) +
        ",\"rotation\":" + fjson(object->getRotation()) +
        ",\"dx\":" + fjson(distance) +
        ",\"kind\":\"" + objectKind(id) +
        "\",\"interactive\":" + bjson(isInteractiveID(id)) +
        ",\"orb\":" + bjson(isOrbID(id)) +
        "}";
}

static std::string relevantObjectsJson(PlayLayer* layer, float lookBehind, float lookAhead, int maxCount, bool interactiveOnly = false) {
    if (!layer || !layer->m_player1) return "[]";
    if (!layer->m_level || g_cachedLevelID != layer->m_level->m_levelID || g_sortedObjects.empty()) {
        rebuildObjectCache(layer);
    }

    const float px = layer->m_player1->getPositionX();
    std::vector<GameObject*> candidates;
    candidates.reserve(160);
    for (auto object : g_sortedObjects) {
        if (!object) continue;
        const auto rect = object->getObjectRect();
        const float centerX = rect.origin.x + rect.size.width * 0.5f;
        const float dx = centerX - px;
        if (dx < -lookBehind) continue;
        if (dx > lookAhead) break;
        if (interactiveOnly && !isInteractiveID(object->m_objectID)) continue;
        candidates.push_back(object);
    }

    std::stable_sort(candidates.begin(), candidates.end(), [px](GameObject* a, GameObject* b) {
        const auto ra = a->getObjectRect();
        const auto rb = b->getObjectRect();
        return std::abs((ra.origin.x + ra.size.width * 0.5f) - px) <
               std::abs((rb.origin.x + rb.size.width * 0.5f) - px);
    });

    std::string result = "[";
    const int count = std::min(maxCount, static_cast<int>(candidates.size()));
    for (int i = 0; i < count; ++i) {
        if (i) result += ",";
        result += oneObjectJson(candidates[i], px);
    }
    result += "]";
    return result;
}

static std::string actionableTargetsJson(PlayLayer* layer) {
    return relevantObjectsJson(layer, 250.f, 1100.f, 16, true);
}

static std::string objectsJson(PlayLayer* layer) {
    return relevantObjectsJson(layer, OBJECT_LOOK_BEHIND, OBJECT_LOOK_AHEAD, OBJECT_MAX_SENT, false);
}

static void clearDebugBoxes() {
    if (g_debugNode) {
        g_debugNode->removeFromParent();
        g_debugNode = nullptr;
    }
}

static void drawDebugBoxes(PlayLayer* layer) {
    clearDebugBoxes();
    g_debugDrawnObjects = 0;
    g_debugVisibleObjects = 0;
    if (!layer || !g_debugBoxes || !layer->m_player1) return;

    if (!layer->m_level || g_cachedLevelID != layer->m_level->m_levelID || g_sortedObjects.empty()) {
        rebuildObjectCache(layer);
    }

    auto node = CCDrawNode::create();
    if (!node) return;
    node->setZOrder(9999);

    const auto playerRect = layer->m_player1->getObjectRect();
    node->drawRect(
        playerRect.origin,
        playerRect.origin + playerRect.size,
        ccc4f(1.f, 1.f, 0.f, 1.f),
        2.f,
        ccc4f(1.f, 1.f, 0.f, 0.08f)
    );

    const float px = layer->m_player1->getPositionX();
    constexpr float LOOK_BEHIND = 220.f;
    constexpr float LOOK_AHEAD = 1400.f;
    constexpr int MAX_DRAW = 32;
    constexpr int HIGHLIGHT = 12;

    struct Candidate { GameObject* object; float dx; };
    std::vector<Candidate> candidates;
    candidates.reserve(48);

    for (auto object : g_sortedObjects) {
        if (!object) continue;
        const auto rect = object->getObjectRect();
        const float centerX = rect.origin.x + rect.size.width * 0.5f;
        const float dx = centerX - px;
        if (dx < -LOOK_BEHIND) continue;
        if (dx > LOOK_AHEAD) break;
        candidates.push_back({object, dx});
        if (static_cast<int>(candidates.size()) >= 48) break;
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        const bool aOrb = isOrbID(a.object->m_objectID);
        const bool bOrb = isOrbID(b.object->m_objectID);
        if (aOrb != bOrb) return aOrb > bOrb;
        return std::abs(a.dx) < std::abs(b.dx);
    });

    g_debugVisibleObjects = static_cast<int>(candidates.size());
    const int drawCount = std::min(MAX_DRAW, static_cast<int>(candidates.size()));
    for (int i = 0; i < drawCount; ++i) {
        auto object = candidates[i].object;
        const auto rect = object->getObjectRect();
        const bool orb = isOrbID(object->m_objectID);
        const bool highlighted = i < HIGHLIGHT && candidates[i].dx >= 0.f;
        const auto edge = orb ? ccc4f(0.2f, 1.f, 0.4f, 1.f) : ccc4f(1.f, 0.25f, 0.15f, highlighted ? 1.f : 0.75f);
        const auto fill = orb ? ccc4f(0.2f, 1.f, 0.4f, 0.07f) : ccc4f(1.f, 0.25f, 0.15f, 0.025f);
        node->drawRect(rect.origin, rect.origin + rect.size, edge, orb ? 2.5f : 1.f, fill);
    }
    g_debugDrawnObjects = drawCount;

    const float centerY = playerRect.origin.y + playerRect.size.height * 0.5f;
    node->drawSegment(ccp(px, centerY), ccp(px + LOOK_AHEAD, centerY), 1.f, ccc4f(1.f, 1.f, 1.f, 0.3f));
    layer->addChild(node);
    g_debugNode = node;
}

class $modify(GDABridgeGameLayer, GJBaseGameLayer) {
    int checkCollisions(PlayerObject* player, float dt, bool p2) {
        if (g_noclip) {
            auto* pl = PlayLayer::get();
            if (pl && (player == pl->m_player1 || player == pl->m_player2)) {
                return 0;
            }
        }
        return GJBaseGameLayer::checkCollisions(player, dt, p2);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto* pl = PlayLayer::get();
        const bool inLevel = pl != nullptr;

        if (inLevel && g_connected) {
            const double t = levelTimeSeconds();
            const char* source = g_injectedInput ? "AI" : "GAME";
            const int player = isPlayer1 ? 1 : 2;
            const char* action = down ? "DOWN" : "UP";

            sendJson(
                std::string(
                    "{\"type\":\"input\",\"protocol\":4,\"timestamp\":"
                ) +
                std::to_string(t) +
                ",\"action\":\"" + action +
                "\",\"button\":" + std::to_string(button) +
                ",\"player\":" + std::to_string(player) +
                ",\"source\":\"" + source + "\"}"
            );
        }

        GJBaseGameLayer::handleButton(
            down,
            button,
            isPlayer1
        );
    }
};

static void reportCurrentLevel(PlayLayer* layer, bool force = false) {
    if (!layer) return;

    int levelID = 0;
    std::string levelName;
    if (layer->m_level) {
        levelID = layer->m_level->m_levelID;
        levelName = layer->m_level->m_levelName;
    }

    if (!force && levelID == g_lastReportedLevelID && levelName == g_lastReportedLevelName) {
        return;
    }

    g_lastReportedLevelID = levelID;
    g_lastReportedLevelName = levelName;

    sendJson(
        std::string("{\"type\":\"level_info\",\"protocol\":4,\"level_id\":") +
        std::to_string(levelID) +
        ",\"level_name\":\"" + escapeJson(levelName) +
        "\",\"practice\":" + bjson(layer->m_isPracticeMode) +
        ",\"attempts\":" + std::to_string(layer->m_attempts) +
        "}"
    );

    log::info("[GD AI Bridge] Current level: '{}' (ID {})", levelName, levelID);
}

class $modify(GDABridgePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        const bool result = PlayLayer::init(level, useReplay, dontCreateObjects);
        if (!result) return false;

        g_levelStart = std::chrono::steady_clock::now();
        g_haveP1 = false;
        g_haveP2 = false;
        g_lastReportedLevelID = -1;
        g_lastReportedLevelName.clear();
        g_debugDrawnObjects = 0;
        g_debugVisibleObjects = 0;
        rebuildObjectCache(this);
        g_inLevel = true;

        g_naturalTimeWarp = m_gameState.m_timeWarp;

        if (g_practice) {
            m_isPracticeMode = true;
        }

        std::string levelName;
        int levelID = 0;
        if (m_level) {
            levelName = m_level->m_levelName;
            levelID = m_level->m_levelID;
        }

        sendJson(
            std::string(
                "{\"type\":\"level_loaded\",\"protocol\":4,\"level_id\":"
            ) +
            std::to_string(levelID) +
            ",\"level_name\":\"" + escapeJson(levelName) +
            "\",\"practice\":" + bjson(m_isPracticeMode) +
            ",\"attempts\":" + std::to_string(m_attempts) +
            "}"
        );

        reportCurrentLevel(this, true);
        sendDebugStatus(true);

        return true;
    }

    void postUpdate(float dt) {
        processPendingCommands();

        // Apply training cheats before the original game update so the current
        // frame actually uses the requested timewarp/practice state.
        if (g_speedhack) {
            m_gameState.m_timeWarp = g_speed.load();
        } else if (m_gameState.m_timeWarp != g_naturalTimeWarp) {
            m_gameState.m_timeWarp = g_naturalTimeWarp;
        }

        if (g_practice) {
            m_isPracticeMode = true;
        }

        PlayLayer::postUpdate(dt);

        if (g_needLevelReport.exchange(false)) {
            reportCurrentLevel(this, true);
        } else {
            reportCurrentLevel(this);
        }

        const auto now = std::chrono::steady_clock::now();

        const float interval = 1.f / static_cast<float>(g_stateHz.load());
        const float stateElapsed = std::chrono::duration<float>(now - g_lastState).count();

        if (g_connected && stateElapsed >= interval) {
            g_lastState = now;

            float progress = 0.f;
            if (m_levelLength > 0.f && m_player1) {
                progress = (m_player1->getPositionX() / m_levelLength) * 100.f;
                progress = std::max(0.f, std::min(100.f, progress));
            }

            std::string levelName;
            int levelID = 0;
            if (m_level) {
                levelName = m_level->m_levelName;
                levelID = m_level->m_levelID;
            }

            sendJson(
                std::string(
                    "{\"type\":\"state\",\"protocol\":4,\"frame_dt\":"
                ) + fjson(dt) +
                ",\"progress\":" + fjson(progress) +
                ",\"paused\":" + bjson(m_isPaused) +
                ",\"level_id\":" + std::to_string(levelID) +
                ",\"level_name\":\"" + escapeJson(levelName) +
                "\",\"attempts\":" + std::to_string(m_attempts) +
                ",\"clicks\":" + std::to_string(m_clicks) +
                ",\"practice\":" + bjson(m_isPracticeMode) +
                ",\"timewarp\":" + fjson(m_gameState.m_timeWarp) +
                ",\"targets\":" + actionableTargetsJson(this) +
                ",\"player1\":" + playerJson(m_player1, dt, 1) +
                ",\"player2\":" + playerJson(m_player2, dt, 2) +
                ",\"cheats\":{\"noclip\":" + bjson(g_noclip.load()) +
                ",\"speedhack\":" + bjson(g_speedhack.load()) +
                ",\"speed\":" + fjson(g_speed.load()) +
                ",\"practice\":" + bjson(g_practice.load()) +
                ",\"debug\":" + bjson(g_debugBoxes.load()) +
                "}}"
            );
        }

        const float objectElapsed = std::chrono::duration<float>(now - g_lastObjects).count();
        if (g_connected && objectElapsed >= (1.f / static_cast<float>(OBJECT_HZ))) {
            g_lastObjects = now;

            const int count = m_objects ? m_objects->count() : 0;

            g_objectPackets.fetch_add(1);
            const std::string items = objectsJson(this);
            sendJson(
                std::string(
                    "{\"type\":\"objects\",\"protocol\":4,\"count\":"
                ) +
                std::to_string(count) +
                ",\"sent_count\":" + std::to_string(
                    std::count(items.begin(), items.end(), '{')
                ) +
                ",\"items\":" + items +
                "}"
            );
        }

        static auto lastDebugDraw = std::chrono::steady_clock::now();
        const float debugInterval = 1.f / static_cast<float>(g_debugFPS.load());
        const float debugElapsed = std::chrono::duration<float>(now - lastDebugDraw).count();
        if (g_debugBoxes && debugElapsed >= debugInterval) {
            lastDebugDraw = now;
            drawDebugBoxes(this);
            sendDebugStatus();
        } else if (!g_debugBoxes) {
            clearDebugBoxes();
            g_debugDrawnObjects = 0;
            g_debugVisibleObjects = 0;
            sendDebugStatus();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_noclip) {
            sendJson("{\"type\":\"noclip_prevented_death\"}");
            return;
        }

        if (g_connected) {
            sendJson(
                std::string("{\"type\":\"player_death\",\"level_id\":") +
                std::to_string(m_level ? m_level->m_levelID : 0) +
                ",\"level_name\":\"" + escapeJson(m_level ? m_level->m_levelName : "") +
                "\",\"progress\":" + fjson(
                    (m_levelLength > 0.f && m_player1)
                        ? std::max(0.f, std::min(100.f, (m_player1->getPositionX() / m_levelLength) * 100.f))
                        : 0.f
                ) +
                "}"
            );
        }

        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (g_connected) {
            sendJson(
                std::string("{\"type\":\"level_complete\",\"level_id\":") +
                std::to_string(m_level ? m_level->m_levelID : 0) +
                ",\"level_name\":\"" + escapeJson(m_level ? m_level->m_levelName : "") +
                "\"}"
            );
        }
        PlayLayer::levelComplete();
    }

    void resetLevel() {
        g_levelStart = std::chrono::steady_clock::now();
        g_haveP1 = false;
        g_haveP2 = false;
        if (m_level && g_cachedLevelID != m_level->m_levelID) rebuildObjectCache(this);

        if (g_connected) {
            sendJson(
                std::string("{\"type\":\"attempt_reset\",\"level_id\":") +
                std::to_string(m_level ? m_level->m_levelID : 0) +
                ",\"level_name\":\"" + escapeJson(m_level ? m_level->m_levelName : "") +
                "\"}"
            );
        }

        PlayLayer::resetLevel();
    }

    void onExit() {
        if (g_connected) {
            sendJson(
                std::string("{\"type\":\"level_exit\",\"level_id\":") +
                std::to_string(m_level ? m_level->m_levelID : 0) +
                ",\"level_name\":\"" + escapeJson(m_level ? m_level->m_levelName : "") +
                "\"}"
            );
        }

        clearDebugBoxes();
        g_debugDrawnObjects = 0;
        g_debugVisibleObjects = 0;
        g_inLevel = false;
        g_lastReportedLevelID = -1;
        g_lastReportedLevelName.clear();
        g_sortedObjects.clear();
        g_cachedLevelID = -1;
        PlayLayer::onExit();
        sendDebugStatus(true);
    }
};

static constexpr int PANEL_TAG = 7001;
static constexpr int STATUS_TAG = 7002;
static constexpr int INFO_TAG = 7003;

class $modify(GDABridgeMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        const auto size = CCDirector::sharedDirector()->getWinSize();

        auto buttonSprite = ButtonSprite::create(
            "AI",
            "goldFont.fnt",
            "GJ_button_01.png",
            0.7f
        );

        auto button = CCMenuItemSpriteExtra::create(
            buttonSprite,
            this,
            menu_selector(GDABridgeMenuLayer::openPanel)
        );

        auto menu = CCMenu::create();
        menu->setPosition({size.width - 45.f, 45.f});
        menu->setZOrder(10000);
        menu->addChild(button);
        addChild(menu);

        auto panel = CCLayerColor::create(
            ccc4(10, 10, 10, 235),
            410.f,
            245.f
        );

        panel->setPosition({20.f, 20.f});
        panel->setTag(PANEL_TAG);
        panel->setZOrder(10001);
        panel->setVisible(false);
        addChild(panel);

        auto title = CCLabelBMFont::create(
            "GD AI BRIDGE",
            "bigFont.fnt"
        );
        title->setScale(0.55f);
        title->setAnchorPoint({0.f, 0.5f});
        title->setPosition({20.f, 215.f});
        panel->addChild(title);

        auto status = CCLabelBMFont::create(
            "MOD: LOADED",
            "bigFont.fnt"
        );
        status->setScale(0.32f);
        status->setAnchorPoint({0.f, 1.f});
        status->setPosition({20.f, 190.f});
        status->setTag(STATUS_TAG);
        panel->addChild(status);

        auto info = CCLabelBMFont::create(
            "TCP 127.0.0.1:8765",
            "bigFont.fnt"
        );
        info->setScale(0.27f);
        info->setAnchorPoint({0.f, 0.5f});
        info->setPosition({20.f, 90.f});
        info->setTag(INFO_TAG);
        panel->addChild(info);

        auto reconnect = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("RECONNECT", "goldFont.fnt", "GJ_button_01.png", 0.65f),
            this,
            menu_selector(GDABridgeMenuLayer::onReconnect)
        );

        auto ping = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("PING", "goldFont.fnt", "GJ_button_02.png", 0.65f),
            this,
            menu_selector(GDABridgeMenuLayer::onPing)
        );

        auto debug = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("DEBUG", "goldFont.fnt", "GJ_button_03.png", 0.65f),
            this,
            menu_selector(GDABridgeMenuLayer::onDebug)
        );

        auto close = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("CLOSE", "goldFont.fnt", "GJ_button_04.png", 0.65f),
            this,
            menu_selector(GDABridgeMenuLayer::closePanel)
        );

        auto panelMenu = CCMenu::create();
        panelMenu->setPosition({205.f, 43.f});
        reconnect->setPosition({-125.f, 0.f});
        ping->setPosition({0.f, 0.f});
        debug->setPosition({125.f, 0.f});
        panelMenu->addChild(reconnect);
        panelMenu->addChild(ping);
        panelMenu->addChild(debug);
        panel->addChild(panelMenu);

        close->setPosition({205.f, -8.f});
        panel->addChild(close);

        schedule(
            schedule_selector(GDABridgeMenuLayer::updateBridgeUI),
            0.25f
        );

        log::info("[GD AI Bridge] Diagnostic UI initialized");
        return true;
    }

    void updateBridgeUI(float) {
        processPendingCommands();

        auto panel = getChildByTag(PANEL_TAG);
        if (!panel) return;

        auto status = static_cast<CCLabelBMFont*>(
            panel->getChildByTag(STATUS_TAG)
        );
        if (!status) return;

        std::string text = "MOD: LOADED\n";

        if (g_connected) {
            text += "TCP: CONNECTED\n";
            text += g_pythonResponsive
                ? "PYTHON: RESPONDING\n"
                : "PYTHON: CONNECTED\n";
        } else {
            text += "TCP: OFFLINE\nPYTHON: WAITING\n";
        }

        text += "CONNECTIONS: " +
            std::to_string(g_connectionCount.load()) + "\n";

        text += "DISCONNECTS: " +
            std::to_string(g_disconnectCount.load()) + "\n";

        text += "NC " +
            std::string(g_noclip ? "ON" : "OFF") +
            " | SPD " +
            fjson(g_speed.load()) +
            "x | PR " +
            std::string(g_practice ? "ON" : "OFF") +
            "\nDBG " + std::string(g_debugBoxes ? "ON" : "OFF") +
            " | OBJ " + std::to_string(g_debugDrawnObjects.load()) +
            "/" + std::to_string(g_debugVisibleObjects.load());

        if (auto* pl = PlayLayer::get()) {
            const int levelID = pl->m_level ? pl->m_level->m_levelID : 0;
            const std::string levelName = pl->m_level ? pl->m_level->m_levelName : "";
            text += "\nLEVEL " + std::to_string(levelID) + "\n" + levelName;
        }

        status->setString(text.c_str());
    }

    void openPanel(CCObject*) {
        if (auto panel = getChildByTag(PANEL_TAG)) {
            panel->setVisible(true);
        }
    }

    void closePanel(CCObject*) {
        if (auto panel = getChildByTag(PANEL_TAG)) {
            panel->setVisible(false);
        }
    }

    void onReconnect(CCObject*) {
        log::info("[GD AI Bridge] Manual reconnect requested");
        closeSocket();
    }

    void onPing(CCObject*) {
        if (!g_connected) {
            log::warn("[GD AI Bridge] Ping requested while disconnected");
            return;
        }
        g_pythonResponsive = false;
        sendJson("{\"type\":\"ping\",\"source\":\"gd_mod_ui\"}");
    }

    void onDebug(CCObject*) {
        g_debugBoxes = !g_debugBoxes.load();
        sendJson(
            std::string(
                "{\"type\":\"command\",\"action\":\"debug_boxes\",\"enabled\":"
            ) + bjson(g_debugBoxes.load()) +
            ",\"fps\":" + std::to_string(g_debugFPS.load()) + "}"
        );
        sendDebugStatus(true);
    }
};

$on_mod(Loaded) {
    log::info("[GD AI Bridge] ========================================");
    log::info("[GD AI Bridge] MOD LOADED");
    log::info("[GD AI Bridge] Protocol {}", PROTOCOL);
    log::info("[GD AI Bridge] GD 2.2081 / Geode 5.10.1");
    log::info("[GD AI Bridge] TCP {}:{}", HOST, PORT);
    log::info("[GD AI Bridge] ========================================");

    g_running = true;
    g_senderThread = std::thread(senderThread);
    g_networkThread = std::thread(networkThread);
    g_senderThread.detach();
    g_networkThread.detach();
}

} // namespace GDABridge
