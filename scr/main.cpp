#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

static constexpr int PROTOCOL_VERSION = 2;

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_debugBoxes{false};
static std::atomic<int> g_stateHz{30};

static SOCKET g_socket = INVALID_SOCKET;

static std::mutex g_socketMutex;
static std::mutex g_commandMutex;

static std::vector<std::string> g_commands;

static std::thread g_networkThread;

static PlayLayer* g_playLayer = nullptr;

static float g_lastPlayerX = 0.f;
static float g_lastPlayerY = 0.f;
static bool g_haveLastPosition = false;

static std::chrono::steady_clock::time_point g_lastStateTime;
static std::chrono::steady_clock::time_point g_lastObjectTime;

static CCNode* g_debugNode = nullptr;


/*
 * ------------------------------------------------------------
 * JSON helpers
 * ------------------------------------------------------------
 */

static std::string jsonEscape(const std::string& input) {
    std::string output;

    for (char c : input) {
        switch (c) {
            case '"':
                output += "\\\"";
                break;

            case '\\':
                output += "\\\\";
                break;

            case '\n':
                output += "\\n";
                break;

            case '\r':
                output += "\\r";
                break;

            case '\t':
                output += "\\t";
                break;

            default:
                output += c;
                break;
        }
    }

    return output;
}

static std::string boolJson(bool value) {
    return value ? "true" : "false";
}

static std::string numberJson(float value) {
    std::ostringstream stream;

    stream.setf(std::ios::fixed);
    stream.precision(3);

    stream << value;

    return stream.str();
}

static std::string intJson(int value) {
    return std::to_string(value);
}


/*
 * ------------------------------------------------------------
 * Socket
 * ------------------------------------------------------------
 */

static void closeSocket() {
    std::lock_guard<std::mutex> lock(g_socketMutex);

    if (g_socket != INVALID_SOCKET) {
        shutdown(g_socket, SD_BOTH);
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }

    g_connected = false;
}

static bool sendRaw(const std::string& data) {
    std::lock_guard<std::mutex> lock(g_socketMutex);

    if (g_socket == INVALID_SOCKET) {
        return false;
    }

    size_t totalSent = 0;

    while (totalSent < data.size()) {
        int result = send(
            g_socket,
            data.data() + totalSent,
            static_cast<int>(data.size() - totalSent),
            0
        );

        if (result <= 0) {
            return false;
        }

        totalSent += result;
    }

    return true;
}

static bool sendJson(const std::string& json) {
    return sendRaw(json + "\n");
}


/*
 * ------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------
 */

static bool contains(const std::string& text, const std::string& value) {
    return text.find(value) != std::string::npos;
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

static void processCommand(const std::string& command) {

    log::info("[GD AI Bridge] Command: {}", command);

    /*
     * Ping
     */

    if (contains(command, "\"action\":\"ping\"")) {
        sendJson(
            "{\"type\":\"pong\",\"protocol\":2}"
        );

        return;
    }

    /*
     * Enable / disable debug boxes
     */

    if (contains(command, "\"action\":\"debug_boxes\"")) {

        if (contains(command, "\"enabled\":true")) {
            g_debugBoxes = true;
        }
        else if (contains(command, "\"enabled\":false")) {
            g_debugBoxes = false;
        }

        sendJson(
            std::string(
                "{\"type\":\"command_result\","
                "\"action\":\"debug_boxes\","
                "\"enabled\":"
            )
            + boolJson(g_debugBoxes)
            + "}"
        );

        return;
    }

    /*
     * State update frequency
     */

    if (contains(command, "\"action\":\"state_hz\"")) {

        auto position = command.find("\"value\":");

        if (position != std::string::npos) {

            position += 8;

            int value = std::atoi(
                command.c_str() + position
            );

            if (value < 1)
                value = 1;

            if (value > 120)
                value = 120;

            g_stateHz = value;

            sendJson(
                std::string(
                    "{\"type\":\"command_result\","
                    "\"action\":\"state_hz\","
                    "\"value\":"
                )
                + intJson(value)
                + "}"
            );
        }

        return;
    }

    /*
     * Jump / release
     */

    if (contains(command, "\"action\":\"jump\"")) {

        if (g_playLayer && g_playLayer->m_player1) {

            g_playLayer->m_player1->pushButton(
                PlayerButton::Jump
            );

            sendJson(
                "{\"type\":\"command_result\","
                "\"action\":\"jump\","
                "\"success\":true}"
            );
        }

        return;
    }

    if (contains(command, "\"action\":\"release\"")) {

        if (g_playLayer && g_playLayer->m_player1) {

            g_playLayer->m_player1->releaseButton(
                PlayerButton::Jump
            );

            sendJson(
                "{\"type\":\"command_result\","
                "\"action\":\"release\","
                "\"success\":true}"
            );
        }

        return;
    }
}


/*
 * ------------------------------------------------------------
 * Network receiver
 * ------------------------------------------------------------
 */

static void receiveLoop() {

    std::string buffer;

    char temp[4096];

    while (g_running) {

        SOCKET socketCopy;

        {
            std::lock_guard<std::mutex> lock(g_socketMutex);

            socketCopy = g_socket;
        }

        if (socketCopy == INVALID_SOCKET) {
            break;
        }

        int received = recv(
            socketCopy,
            temp,
            sizeof(temp),
            0
        );

        if (received <= 0) {
            break;
        }

        buffer.append(
            temp,
            received
        );

        while (true) {

            auto newline = buffer.find('\n');

            if (newline == std::string::npos) {
                break;
            }

            std::string line =
                buffer.substr(
                    0,
                    newline
                );

            buffer.erase(
                0,
                newline + 1
            );

            if (!line.empty()) {
                queueCommand(line);
            }
        }
    }

    closeSocket();
}


/*
 * ------------------------------------------------------------
 * Network connection
 * ------------------------------------------------------------
 */

static bool connectToServer() {

    SOCKET newSocket =
        socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP
        );

    if (newSocket == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in address{};

    address.sin_family = AF_INET;

    address.sin_port =
        htons(PORT);

    inet_pton(
        AF_INET,
        HOST,
        &address.sin_addr
    );

    int result = connect(
        newSocket,
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address)
    );

    if (result != 0) {

        closesocket(
            newSocket
        );

        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_socketMutex);

        g_socket = newSocket;

        g_connected = true;
    }

    log::info(
        "[GD AI Bridge] Connected to {}:{}",
        HOST,
        PORT
    );

    sendJson(
        "{\"type\":\"hello\","
        "\"protocol\":2,"
        "\"game\":\"Geometry Dash\","
        "\"mod\":\"GD AI Bridge\","
        "\"gd_version\":\"2.2081\"}"
    );

    return true;
}


/*
 * ------------------------------------------------------------
 * Network thread
 * ------------------------------------------------------------
 */

static void networkThread() {

    WSADATA data{};

    if (WSAStartup(
        MAKEWORD(2, 2),
        &data
    ) != 0) {

        log::error(
            "[GD AI Bridge] WSAStartup failed"
        );

        return;
    }

    log::info(
        "[GD AI Bridge] Network thread started"
    );

    while (g_running) {

        if (!g_connected) {

            if (connectToServer()) {

                receiveLoop();
            }
            else {

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1000)
                );
            }

            continue;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(50)
        );
    }

    closeSocket();

    WSACleanup();

    log::info(
        "[GD AI Bridge] Network thread stopped"
    );
}


/*
 * ------------------------------------------------------------
 * Player state
 * ------------------------------------------------------------
 */

static std::string playerJson(
    PlayerObject* player,
    float deltaTime
) {

    if (!player) {
        return "null";
    }

    float x =
        player->getPositionX();

    float y =
        player->getPositionY();

    float vx = 0.f;
    float vy = 0.f;

    if (g_haveLastPosition && deltaTime > 0.f) {

        vx =
            (x - g_lastPlayerX)
            / deltaTime;

        vy =
            (y - g_lastPlayerY)
            / deltaTime;
    }

    g_lastPlayerX = x;
    g_lastPlayerY = y;

    g_haveLastPosition = true;

    return
        "{"
        "\"x\":" + numberJson(x) + ","
        "\"y\":" + numberJson(y) + ","
        "\"vx\":" + numberJson(vx) + ","
        "\"vy\":" + numberJson(vy) + ","
        "\"rotation\":" +
            numberJson(player->getRotation()) + ","
        "\"scale\":" +
            numberJson(player->getScale()) + ","
        "\"dead\":" +
            boolJson(player->m_isDead) + ","
        "\"gravityFlipped\":" +
            boolJson(player->m_gravityFlipped) + ","
        "\"onGround\":" +
            boolJson(player->m_isOnGround) +
        "}";
}


/*
 * ------------------------------------------------------------
 * Game objects
 * ------------------------------------------------------------
 */

static std::string objectsJson(
    PlayLayer* layer
) {

    if (!layer) {
        return "[]";
    }

    std::string result = "[";

    bool first = true;

    if (layer->m_objects) {

        for (
            auto object :
            CCArrayExt<GameObject*>(layer->m_objects)
        ) {

            if (!object) {
                continue;
            }

            if (!first) {
                result += ",";
            }

            first = false;

            auto rect =
                object->getObjectRect();

            float width =
                rect.size.width;

            float height =
                rect.size.height;

            result +=
                "{"
                "\"id\":" +
                    intJson(object->m_objectID) +
                ",\"x\":" +
                    numberJson(object->getPositionX()) +
                ",\"y\":" +
                    numberJson(object->getPositionY()) +
                ",\"w\":" +
                    numberJson(width) +
                ",\"h\":" +
                    numberJson(height) +
                ",\"rotation\":" +
                    numberJson(object->getRotation()) +
                "}";
        }
    }

    result += "]";

    return result;
}


/*
 * ------------------------------------------------------------
 * Debug visualization
 * ------------------------------------------------------------
 */

static void clearDebugBoxes() {

    if (g_debugNode) {

        g_debugNode->removeFromParent();

        g_debugNode = nullptr;
    }
}

static void drawDebugBoxes(
    PlayLayer* layer
) {

    clearDebugBoxes();

    if (!g_debugBoxes || !layer) {
        return;
    }

    auto node =
        CCDrawNode::create();

    if (!node) {
        return;
    }

    node->setZOrder(
        9999
    );

    /*
     * Player 1
     */

    if (layer->m_player1) {

        auto player =
            layer->m_player1;

        auto rect =
            player->getObjectRect();

        node->drawRect(
            rect.origin,
            rect.origin + rect.size,
            ccc4f(
                1.f,
                1.f,
                0.f,
                1.f
            ),
            2.f,
            ccc4f(
                1.f,
                1.f,
                0.f,
                0.15f
            )
        );
    }

    /*
     * Nearby objects
     */

    if (layer->m_objects) {

        int count = 0;

        for (
            auto object :
            CCArrayExt<GameObject*>(layer->m_objects)
        ) {

            if (!object)
                continue;

            auto rect =
                object->getObjectRect();

            node->drawRect(
                rect.origin,
                rect.origin + rect.size,
                ccc4f(
                    1.f,
                    0.f,
                    0.f,
                    1.f
                ),
                1.f,
                ccc4f(
                    1.f,
                    0.f,
                    0.f,
                    0.05f
                )
            );

            count++;

            /*
             * Avoid drawing thousands of objects
             * every frame.
             */

            if (count >= 150) {
                break;
            }
        }
    }

    layer->addChild(
        node
    );

    g_debugNode = node;
}


/*
 * ------------------------------------------------------------
 * Events
 * ------------------------------------------------------------
 */

static void sendSimpleEvent(
    const char* type
) {

    sendJson(
        std::string(
            "{\"type\":\""
        )
        + type
        + "\"}"
    );
}


/*
 * ------------------------------------------------------------
 * PlayLayer hook
 * ------------------------------------------------------------
 */

class $modify(GDABridgePlayLayer, PlayLayer) {

    bool init(
        GJGameLevel* level,
        bool useReplay,
        bool dontCreateObjects
    ) {

        bool result =
            PlayLayer::init(
                level,
                useReplay,
                dontCreateObjects
            );

        if (!result) {
            return false;
        }

        g_playLayer = this;

        g_haveLastPosition = false;

        log::info(
            "[GD AI Bridge] PlayLayer initialized"
        );

        sendJson(
            "{\"type\":\"level_loaded\"}"
        );

        return true;
    }


    void postUpdate(
        float dt
    ) {

        PlayLayer::postUpdate(dt);

        g_playLayer = this;

        /*
         * Process commands.
         */

        auto commands =
            takeCommands();

        for (const auto& command : commands) {
            processCommand(command);
        }

        /*
         * State frequency.
         */

        auto now =
            std::chrono::steady_clock::now();

        float interval =
            1.f /
            static_cast<float>(
                g_stateHz.load()
            );

        float elapsed =
            std::chrono::duration<float>(
                now - g_lastStateTime
            ).count();

        if (
            elapsed >= interval &&
            g_connected
        ) {

            g_lastStateTime =
                now;

            float progress = 0.f;

            if (
                m_levelLength > 0.f &&
                m_player1
            ) {

                progress =
                    (
                        m_player1->getPositionX()
                        /
                        m_levelLength
                    )
                    * 100.f;
            }

            std::string levelName;

            int levelID = 0;

            if (m_level) {

                levelName =
                    m_level->m_levelName;

                levelID =
                    m_level->m_levelID;
            }

            std::string packet =
                "{"
                "\"type\":\"state\","
                "\"frame_dt\":" +
                    numberJson(dt) +
                ",\"progress\":" +
                    numberJson(progress) +
                ",\"paused\":" +
                    boolJson(m_isPaused) +
                ",\"level_id\":" +
                    intJson(levelID) +
                ",\"level_name\":\"" +
                    jsonEscape(levelName) +
                "\",\"player1\":" +
                    playerJson(
                        m_player1,
                        dt
                    ) +
                ",\"player2\":" +
                    playerJson(
                        m_player2,
                        dt
                    ) +
                "}";

            if (!sendJson(packet)) {
                closeSocket();
            }
        }

        /*
         * Object update.
         */

        auto objectElapsed =
            std::chrono::duration<float>(
                now - g_lastObjectTime
            ).count();

        if (
            objectElapsed >= 0.1f &&
            g_connected
        ) {

            g_lastObjectTime =
                now;

            std::string packet =
                "{\"type\":\"objects\","
                "\"count\":";

            int count = 0;

            if (m_objects) {
                count =
                    m_objects->count();
            }

            packet +=
                intJson(count);

            packet +=
                ",\"items\":" +
                objectsJson(this) +
                "}";

            if (!sendJson(packet)) {
                closeSocket();
            }
        }

        /*
         * Debug drawing.
         */

        if (g_debugBoxes) {
            drawDebugBoxes(this);
        }
        else {
            clearDebugBoxes();
        }
    }


    void destroyPlayer(
        PlayerObject* player,
        GameObject* object
    ) {

        if (g_connected) {

            sendJson(
                "{\"type\":\"player_death\"}"
            );
        }

        PlayLayer::destroyPlayer(
            player,
            object
        );
    }


    void levelComplete() {

        if (g_connected) {

            sendJson(
                "{\"type\":\"level_complete\"}"
            );
        }

        PlayLayer::levelComplete();
    }


    void resetLevel() {

        g_haveLastPosition = false;

        if (g_connected) {

            sendJson(
                "{\"type\":\"attempt_reset\"}"
            );
        }

        PlayLayer::resetLevel();
    }


    void onExit() {

        if (g_connected) {

            sendJson(
                "{\"type\":\"level_exit\"}"
            );
        }

        if (g_playLayer == this) {
            g_playLayer = nullptr;
        }

        clearDebugBoxes();

        PlayLayer::onExit();
    }
};


/*
 * ------------------------------------------------------------
 * Mod lifecycle
 * ------------------------------------------------------------
 */

$on_mod(Loaded) {

    log::info(
        "[GD AI Bridge] Loading..."
    );

    g_running = true;

    g_networkThread =
        std::thread(
            networkThread
        );

    log::info(
        "[GD AI Bridge] Loaded."
    );
}


$on_mod(Unloaded) {

    log::info(
        "[GD AI Bridge] Unloading..."
    );

    g_running = false;

    closeSocket();

    if (
        g_networkThread.joinable()
    ) {

        g_networkThread.join();
    }

    clearDebugBoxes();

    g_playLayer = nullptr;

    log::info(
        "[GD AI Bridge] Unloaded."
    );
}

} // namespace GDABridge
