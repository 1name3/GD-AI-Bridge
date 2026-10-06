#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <atomic>
#include <chrono>
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
static constexpr int PROTOCOL_VERSION = 2;


/*
 * ============================================================
 * GLOBAL STATE
 * ============================================================
 */

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_pythonResponsive{false};
static std::atomic<bool> g_debugBoxes{false};

static std::atomic<int> g_stateHz{30};

static std::atomic<unsigned long> g_connectionCount{0};
static std::atomic<unsigned long> g_disconnectCount{0};

static SOCKET g_socket = INVALID_SOCKET;

static std::mutex g_socketMutex;
static std::mutex g_commandMutex;

static std::vector<std::string> g_commands;

static std::thread g_networkThread;

static PlayLayer* g_playLayer = nullptr;

static CCNode* g_debugNode = nullptr;


/*
 * Player 1 velocity history
 */

static float g_lastP1X = 0.f;
static float g_lastP1Y = 0.f;
static bool g_haveP1Position = false;


/*
 * Player 2 velocity history
 */

static float g_lastP2X = 0.f;
static float g_lastP2Y = 0.f;
static bool g_haveP2Position = false;


/*
 * Timing
 */

static std::chrono::steady_clock::time_point g_lastStateTime =
    std::chrono::steady_clock::now();

static std::chrono::steady_clock::time_point g_lastObjectTime =
    std::chrono::steady_clock::now();


/*
 * ============================================================
 * JSON HELPERS
 * ============================================================
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


/*
 * ============================================================
 * SOCKET
 * ============================================================
 */

static void closeSocket() {

    std::lock_guard<std::mutex> lock(
        g_socketMutex
    );

    if (g_socket != INVALID_SOCKET) {

        shutdown(
            g_socket,
            SD_BOTH
        );

        closesocket(
            g_socket
        );

        g_socket = INVALID_SOCKET;
    }

    if (g_connected) {
        g_disconnectCount.fetch_add(1);
    }

    g_connected = false;
    g_pythonResponsive = false;
}


static bool sendRaw(
    const std::string& data
) {

    std::lock_guard<std::mutex> lock(
        g_socketMutex
    );

    if (g_socket == INVALID_SOCKET) {
        return false;
    }

    size_t totalSent = 0;

    while (totalSent < data.size()) {

        int result = send(
            g_socket,
            data.data() + totalSent,
            static_cast<int>(
                data.size() - totalSent
            ),
            0
        );

        if (result <= 0) {
            return false;
        }

        totalSent += result;
    }

    return true;
}


static bool sendJson(
    const std::string& json
) {

    return sendRaw(
        json + "\n"
    );
}


/*
 * ============================================================
 * COMMAND QUEUE
 * ============================================================
 */

static void queueCommand(
    const std::string& command
) {

    std::lock_guard<std::mutex> lock(
        g_commandMutex
    );

    g_commands.push_back(
        command
    );
}


static std::vector<std::string> takeCommands() {

    std::lock_guard<std::mutex> lock(
        g_commandMutex
    );

    auto result = g_commands;

    g_commands.clear();

    return result;
}


static bool contains(
    const std::string& text,
    const std::string& value
) {

    return text.find(value)
        != std::string::npos;
}


/*
 * ============================================================
 * COMMAND PROCESSING
 * ============================================================
 */

static void processCommand(
    const std::string& command
) {

    log::info(
        "[GD AI Bridge] RX command: {}",
        command
    );


    /*
     * --------------------------------------------------------
     * PING
     * --------------------------------------------------------
     */

    if (
        contains(
            command,
            "\"action\":\"ping\""
        )
    ) {

        sendJson(
            "{"
            "\"type\":\"pong\","
            "\"protocol\":2"
            "}"
        );

        return;
    }


    /*
     * --------------------------------------------------------
     * DEBUG BOXES
     * --------------------------------------------------------
     */

    if (
        contains(
            command,
            "\"action\":\"debug_boxes\""
        )
    ) {

        if (
            contains(
                command,
                "\"enabled\":true"
            )
        ) {
            g_debugBoxes = true;
        }
        else if (
            contains(
                command,
                "\"enabled\":false"
            )
        ) {
            g_debugBoxes = false;
        }

        sendJson(
            std::string(
                "{"
                "\"type\":\"command_result\","
                "\"action\":\"debug_boxes\","
                "\"enabled\":"
            )
            +
            boolJson(
                g_debugBoxes
            )
            +
            "}"
        );

        log::info(
            "[GD AI Bridge] Debug boxes = {}",
            g_debugBoxes.load()
        );

        return;
    }


    /*
     * --------------------------------------------------------
     * STATE HZ
     * --------------------------------------------------------
     */

    if (
        contains(
            command,
            "\"action\":\"state_hz\""
        )
    ) {

        auto position =
            command.find(
                "\"value\":"
            );

        if (
            position !=
            std::string::npos
        ) {

            position += 8;

            int value =
                std::atoi(
                    command.c_str()
                    + position
                );

            if (value < 1)
                value = 1;

            if (value > 120)
                value = 120;

            g_stateHz = value;

            sendJson(
                std::string(
                    "{"
                    "\"type\":\"command_result\","
                    "\"action\":\"state_hz\","
                    "\"value\":"
                )
                +
                std::to_string(value)
                +
                "}"
            );
        }

        return;
    }


    /*
     * --------------------------------------------------------
     * JUMP
     * --------------------------------------------------------
     */

    if (
        contains(
            command,
            "\"action\":\"jump\""
        )
    ) {

        if (
            g_playLayer &&
            g_playLayer->m_player1
        ) {

            g_playLayer
                ->m_player1
                ->pushButton(
                    PlayerButton::Jump
                );

            sendJson(
                "{"
                "\"type\":\"command_result\","
                "\"action\":\"jump\","
                "\"success\":true"
                "}"
            );

            log::info(
                "[GD AI Bridge] Jump command executed"
            );
        }
        else {

            sendJson(
                "{"
                "\"type\":\"command_result\","
                "\"action\":\"jump\","
                "\"success\":false,"
                "\"reason\":\"no_player\""
                "}"
            );
        }

        return;
    }


    /*
     * --------------------------------------------------------
     * RELEASE
     * --------------------------------------------------------
     */

    if (
        contains(
            command,
            "\"action\":\"release\""
        )
    ) {

        if (
            g_playLayer &&
            g_playLayer->m_player1
        ) {

            g_playLayer
                ->m_player1
                ->releaseButton(
                    PlayerButton::Jump
                );

            sendJson(
                "{"
                "\"type\":\"command_result\","
                "\"action\":\"release\","
                "\"success\":true"
                "}"
            );

            log::info(
                "[GD AI Bridge] Release command executed"
            );
        }
        else {

            sendJson(
                "{"
                "\"type\":\"command_result\","
                "\"action\":\"release\","
                "\"success\":false,"
                "\"reason\":\"no_player\""
                "}"
            );
        }

        return;
    }


    /*
     * Unknown command
     */

    sendJson(
        "{"
        "\"type\":\"command_result\","
        "\"success\":false,"
        "\"reason\":\"unknown_command\""
        "}"
    );
}


/*
 * ============================================================
 * NETWORK RECEIVE LOOP
 * ============================================================
 */

static void receiveLoop() {

    std::string buffer;

    char temp[8192];


    while (g_running) {

        SOCKET socketCopy;

        {
            std::lock_guard<std::mutex> lock(
                g_socketMutex
            );

            socketCopy = g_socket;
        }

        if (
            socketCopy ==
            INVALID_SOCKET
        ) {
            break;
        }


        int received =
            recv(
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

            auto newline =
                buffer.find(
                    '\n'
                );

            if (
                newline ==
                std::string::npos
            ) {
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


            if (
                line.empty()
            ) {
                continue;
            }


            /*
             * Python pong
             */

            if (
                contains(
                    line,
                    "\"type\":\"pong\""
                )
            ) {

                g_pythonResponsive = true;

                log::info(
                    "[GD AI Bridge] Python responded to ping"
                );

                continue;
            }


            queueCommand(
                line
            );
        }
    }


    closeSocket();
}


/*
 * ============================================================
 * CONNECT
 * ============================================================
 */

static bool connectToServer() {

    SOCKET newSocket =
        socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP
        );


    if (
        newSocket ==
        INVALID_SOCKET
    ) {

        log::error(
            "[GD AI Bridge] socket() failed"
        );

        return false;
    }


    sockaddr_in address{};

    address.sin_family =
        AF_INET;

    address.sin_port =
        htons(PORT);


    if (
        inet_pton(
            AF_INET,
            HOST,
            &address.sin_addr
        ) != 1
    ) {

        closesocket(
            newSocket
        );

        return false;
    }


    int result =
        connect(
            newSocket,
            reinterpret_cast<sockaddr*>(
                &address
            ),
            sizeof(address)
        );


    if (
        result != 0
    ) {

        closesocket(
            newSocket
        );

        return false;
    }


    {
        std::lock_guard<std::mutex> lock(
            g_socketMutex
        );

        if (
            g_socket !=
            INVALID_SOCKET
        ) {

            closesocket(
                newSocket
            );

            return false;
        }

        g_socket =
            newSocket;

        g_connected =
            true;

        g_pythonResponsive =
            false;

        g_connectionCount.fetch_add(
            1
        );
    }


    log::info(
        "[GD AI Bridge] ========================================"
    );

    log::info(
        "[GD AI Bridge] TCP CONNECTED"
    );

    log::info(
        "[GD AI Bridge] Server: {}:{}",
        HOST,
        PORT
    );

    log::info(
        "[GD AI Bridge] ========================================"
    );


    sendJson(
        "{"
        "\"type\":\"hello\","
        "\"protocol\":2,"
        "\"game\":\"Geometry Dash\","
        "\"mod\":\"GD AI Bridge\","
        "\"gd_version\":\"2.2081\""
        "}"
    );


    /*
     * Immediately ask Python to prove that
     * the other side is alive.
     */

    sendJson(
        "{"
        "\"type\":\"ping\","
        "\"source\":\"gd_mod\""
        "}"
    );


    return true;
}


/*
 * ============================================================
 * NETWORK THREAD
 * ============================================================
 */

static void networkThread() {

    WSADATA data{};


    if (
        WSAStartup(
            MAKEWORD(2, 2),
            &data
        ) != 0
    ) {

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

            if (
                connectToServer()
            ) {

                receiveLoop();
            }
            else {

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(
                        500
                    )
                );
            }

            continue;
        }


        std::this_thread::sleep_for(
            std::chrono::milliseconds(
                50
            )
        );
    }


    closeSocket();

    WSACleanup();


    log::info(
        "[GD AI Bridge] Network thread stopped"
    );
}


/*
 * ============================================================
 * PLAYER STATE
 * ============================================================
 */

static std::string playerJson(
    PlayerObject* player,
    float dt,
    int playerIndex
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


    if (
        playerIndex == 1
    ) {

        if (
            g_haveP1Position &&
            dt > 0.f
        ) {

            vx =
                (x - g_lastP1X)
                / dt;

            vy =
                (y - g_lastP1Y)
                / dt;
        }


        g_lastP1X = x;
        g_lastP1Y = y;

        g_haveP1Position =
            true;
    }
    else {

        if (
            g_haveP2Position &&
            dt > 0.f
        ) {

            vx =
                (x - g_lastP2X)
                / dt;

            vy =
                (y - g_lastP2Y)
                / dt;
        }


        g_lastP2X = x;
        g_lastP2Y = y;

        g_haveP2Position =
            true;
    }


    return
        "{"
        "\"x\":" +
        numberJson(x) +
        ",\"y\":" +
        numberJson(y) +
        ",\"vx\":" +
        numberJson(vx) +
        ",\"vy\":" +
        numberJson(vy) +
        ",\"rotation\":" +
        numberJson(
            player->getRotation()
        ) +
        ",\"scale\":" +
        numberJson(
            player->getScale()
        ) +
        ",\"dead\":" +
        boolJson(
            player->m_isDead
        ) +
        ",\"gravityFlipped\":" +
        boolJson(
            player->m_gravityFlipped
        ) +
        ",\"onGround\":" +
        boolJson(
            player->m_isOnGround
        ) +
        "}";
}


/*
 * ============================================================
 * OBJECT STATE
 * ============================================================
 */

static std::string objectsJson(
    PlayLayer* layer
) {

    if (!layer) {
        return "[]";
    }


    std::string result =
        "[";

    bool first = true;


    if (
        layer->m_objects
    ) {

        for (
            auto object :
            CCArrayExt<GameObject*>(
                layer->m_objects
            )
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


            result +=
                "{"
                "\"id\":" +
                std::to_string(
                    object->m_objectID
                ) +
                ",\"x\":" +
                numberJson(
                    object->getPositionX()
                ) +
                ",\"y\":" +
                numberJson(
                    object->getPositionY()
                ) +
                ",\"w\":" +
                numberJson(
                    rect.size.width
                ) +
                ",\"h\":" +
                numberJson(
                    rect.size.height
                ) +
                ",\"rotation\":" +
                numberJson(
                    object->getRotation()
                ) +
                "}";
        }
    }


    result += "]";

    return result;
}


/*
 * ============================================================
 * DEBUG VISUALIZATION
 * ============================================================
 */

static void clearDebugBoxes() {

    if (
        g_debugNode
    ) {

        g_debugNode
            ->removeFromParent();

        g_debugNode =
            nullptr;
    }
}


static void drawDebugBoxes(
    PlayLayer* layer
) {

    clearDebugBoxes();


    if (
        !g_debugBoxes ||
        !layer
    ) {
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

    if (
        layer->m_player1
    ) {

        auto rect =
            layer
                ->m_player1
                ->getObjectRect();

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
                0.12f
            )
        );
    }


    /*
     * Player 2
     */

    if (
        layer->m_player2
    ) {

        auto rect =
            layer
                ->m_player2
                ->getObjectRect();

        node->drawRect(
            rect.origin,
            rect.origin + rect.size,
            ccc4f(
                0.f,
                1.f,
                1.f,
                1.f
            ),
            2.f,
            ccc4f(
                0.f,
                1.f,
                1.f,
                0.12f
            )
        );
    }


    /*
     * Objects
     *
     * Limit to 150 objects so debug mode does
     * not become unnecessarily expensive.
     */

    if (
        layer->m_objects
    ) {

        int drawn = 0;

        for (
            auto object :
            CCArrayExt<GameObject*>(
                layer->m_objects
            )
        ) {

            if (!object) {
                continue;
            }


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
                    0.04f
                )
            );


            drawn++;


            if (
                drawn >= 150
            ) {
                break;
            }
        }
    }


    layer->addChild(
        node
    );

    g_debugNode =
        node;
}


/*
 * ============================================================
 * PLAYLAYER HOOK
 * ============================================================
 */

class $modify(
    GDABridgePlayLayer,
    PlayLayer
) {

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


        g_playLayer =
            this;


        g_haveP1Position =
            false;

        g_haveP2Position =
            false;


        log::info(
            "[GD AI Bridge] PlayLayer initialized"
        );


        sendJson(
            "{"
            "\"type\":\"level_loaded\""
            "}"
        );


        return true;
    }


    void postUpdate(
        float dt
    ) {

        PlayLayer::postUpdate(
            dt
        );


        g_playLayer =
            this;


        /*
         * ------------------------------------------------------
         * COMMANDS
         * ------------------------------------------------------
         */

        auto commands =
            takeCommands();


        for (
            const auto& command :
            commands
        ) {

            processCommand(
                command
            );
        }


        /*
         * ------------------------------------------------------
         * STATE
         * ------------------------------------------------------
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


            float progress =
                0.f;


            if (
                m_levelLength > 0.f &&
                m_player1
            ) {

                progress =
                    (
                        m_player1
                            ->getPositionX()
                        /
                        m_levelLength
                    )
                    * 100.f;
            }


            std::string levelName;

            int levelID = 0;


            if (
                m_level
            ) {

                levelName =
                    m_level->m_levelName;

                levelID =
                    m_level->m_levelID;
            }


            std::string packet =
                "{"
                "\"type\":\"state\","
                "\"protocol\":2,"
                "\"frame_dt\":" +
                numberJson(dt) +
                ",\"progress\":" +
                numberJson(progress) +
                ",\"paused\":" +
                boolJson(m_isPaused) +
                ",\"level_id\":" +
                std::to_string(
                    levelID
                ) +
                ",\"level_name\":\"" +
                jsonEscape(
                    levelName
                ) +
                "\",\"player1\":" +
                playerJson(
                    m_player1,
                    dt,
                    1
                ) +
                ",\"player2\":" +
                playerJson(
                    m_player2,
                    dt,
                    2
                ) +
                "}";


            if (
                !sendJson(
                    packet
                )
            ) {

                closeSocket();
            }
        }


        /*
         * ------------------------------------------------------
         * OBJECTS
         * ------------------------------------------------------
         */

        float objectElapsed =
            std::chrono::duration<float>(
                now - g_lastObjectTime
            ).count();


        if (
            objectElapsed >= 0.1f &&
            g_connected
        ) {

            g_lastObjectTime =
                now;


            int count = 0;


            if (
                m_objects
            ) {

                count =
                    m_objects->count();
            }


            std::string packet =
                "{"
                "\"type\":\"objects\","
                "\"protocol\":2,"
                "\"count\":" +
                std::to_string(
                    count
                ) +
                ",\"items\":" +
                objectsJson(
                    this
                ) +
                "}";


            if (
                !sendJson(
                    packet
                )
            ) {

                closeSocket();
            }
        }


        /*
         * ------------------------------------------------------
         * DEBUG BOXES
         * ------------------------------------------------------
         *
         * Draw at a lower frequency to avoid recreating
         * the node unnecessarily every frame.
         */

        static int debugFrameCounter = 0;

        debugFrameCounter++;


        if (
            g_debugBoxes &&
            debugFrameCounter >= 3
        ) {

            debugFrameCounter = 0;

            drawDebugBoxes(
                this
            );
        }
        else if (
            !g_debugBoxes
        ) {

            clearDebugBoxes();
        }
    }


    void destroyPlayer(
        PlayerObject* player,
        GameObject* object
    ) {

        if (
            g_connected
        ) {

            sendJson(
                "{"
                "\"type\":\"player_death\""
                "}"
            );
        }


        PlayLayer::destroyPlayer(
            player,
            object
        );
    }


    void levelComplete() {

        if (
            g_connected
        ) {

            sendJson(
                "{"
                "\"type\":\"level_complete\""
                "}"
            );
        }


        PlayLayer::levelComplete();
    }


    void resetLevel() {

        g_haveP1Position =
            false;

        g_haveP2Position =
            false;


        if (
            g_connected
        ) {

            sendJson(
                "{"
                "\"type\":\"attempt_reset\""
                "}"
            );
        }


        PlayLayer::resetLevel();
    }


    void onExit() {

        if (
            g_connected
        ) {

            sendJson(
                "{"
                "\"type\":\"level_exit\""
                "}"
            );
        }


        if (
            g_playLayer ==
            this
        ) {

            g_playLayer =
                nullptr;
        }


        clearDebugBoxes();


        PlayLayer::onExit();
    }
};


/*
 * ============================================================
 * IN-GAME / MAIN MENU DIAGNOSTIC UI
 * ============================================================
 *
 * This appears when Geometry Dash opens the main menu.
 *
 * It tells us:
 *
 *   MOD: LOADED
 *   TCP: CONNECTED / OFFLINE
 *   PYTHON: RESPONDING / WAITING
 *   Connections
 *   Disconnects
 *
 * Buttons:
 *
 *   RECONNECT
 *   PING
 *   DEBUG
 *
 * ============================================================
 */

class $modify(
    GDABridgeMenuLayer,
    MenuLayer
) {

    bool init() {

        if (
            !MenuLayer::init()
        ) {
            return false;
        }


        /*
         * ------------------------------------------------------
         * Panel
         * ------------------------------------------------------
         */

        auto panel =
            CCLayerColor::create(
                ccc4(
                    0,
                    0,
                    0,
                    210
                ),
                360.f,
                195.f
            );


        if (!panel) {
            return true;
        }


        panel->ignoreAnchorPointForPosition(
            false
        );


        panel->setAnchorPoint(
            {0.f, 0.f}
        );


        panel->setPosition(
            {20.f, 20.f}
        );


        panel->setZOrder(
            10000
        );


        panel->setID(
            "gdai-panel"_spr
        );


        this->addChild(
            panel
        );


        /*
         * ------------------------------------------------------
         * Title
         * ------------------------------------------------------
         */

        auto title =
            CCLabelBMFont::create(
                "GD AI BRIDGE",
                "bigFont.fnt"
            );


        if (title) {

            title->setScale(
                0.55f
            );

            title->setAnchorPoint(
                {0.f, 0.5f}
            );

            title->setPosition(
                {20.f, 170.f}
            );

            panel->addChild(
                title
            );
        }


        /*
         * ------------------------------------------------------
         * Status label
         * ------------------------------------------------------
         */

        auto status =
            CCLabelBMFont::create(
                "MOD: LOADED",
                "bigFont.fnt"
            );


        if (status) {

            status->setScale(
                0.35f
            );

            status->setAnchorPoint(
                {0.f, 1.f}
            );

            status->setPosition(
                {20.f, 145.f}
            );

            status->setID(
                "gdai-status"_spr
            );

            panel->addChild(
                status
            );
        }


        /*
         * ------------------------------------------------------
         * RECONNECT BUTTON
         * ------------------------------------------------------
         */

        auto reconnectSprite =
            ButtonSprite::create(
                "RECONNECT",
                95,
                true,
                "goldFont.fnt",
                "GJ_button_01.png",
                30.f,
                0.6f
            );


        auto reconnect =
            CCMenuItemSpriteExtra::create(
                reconnectSprite,
                this,
                menu_selector(
                    GDABridgeMenuLayer::onReconnect
                )
            );


        reconnect->setID(
            "gdai-reconnect"_spr
        );


        /*
         * ------------------------------------------------------
         * PING BUTTON
         * ------------------------------------------------------
         */

        auto pingSprite =
            ButtonSprite::create(
                "PING",
                95,
                true,
                "goldFont.fnt",
                "GJ_button_02.png",
                30.f,
                0.6f
            );


        auto ping =
            CCMenuItemSpriteExtra::create(
                pingSprite,
                this,
                menu_selector(
                    GDABridgeMenuLayer::onPing
                )
            );


        ping->setID(
            "gdai-ping"_spr
        );


        /*
         * ------------------------------------------------------
         * DEBUG BUTTON
         * ------------------------------------------------------
         */

        auto debugSprite =
            ButtonSprite::create(
                "DEBUG",
                95,
                true,
                "goldFont.fnt",
                "GJ_button_03.png",
                30.f,
                0.6f
            );


        auto debug =
            CCMenuItemSpriteExtra::create(
                debugSprite,
                this,
                menu_selector(
                    GDABridgeMenuLayer::onDebug
                )
            );


        debug->setID(
            "gdai-debug"_spr
        );


        /*
         * ------------------------------------------------------
         * Button menu
         * ------------------------------------------------------
         */

        auto menu =
            CCMenu::create();


        menu->setPosition(
            {
                180.f,
                38.f
            }
        );


        reconnect->setPosition(
            {
                -110.f,
                0.f
            }
        );


        ping->setPosition(
            {
                0.f,
                0.f
            }
        );


        debug->setPosition(
            {
                110.f,
                0.f
            }
        );


        menu->addChild(
            reconnect
        );

        menu->addChild(
            ping
        );

        menu->addChild(
            debug
        );


        panel->addChild(
            menu
        );


        /*
         * ------------------------------------------------------
         * Footer
         * ------------------------------------------------------
         */

        auto footer =
            CCLabelBMFont::create(
                "127.0.0.1:8765",
                "bigFont.fnt"
            );


        if (footer) {

            footer->setScale(
                0.28f
            );

            footer->setPosition(
                {
                    180.f,
                    90.f
                }
            );

            panel->addChild(
                footer
            );
        }


        /*
         * Update UI four times per second.
         */

        this->schedule(
            schedule_selector(
                GDABridgeMenuLayer::updateBridgeUI
            ),
            0.25f
        );


        updateBridgeUI(
            0.f
        );


        log::info(
            "[GD AI Bridge] Diagnostic UI loaded"
        );


        return true;
    }


    /*
     * ----------------------------------------------------------
     * UPDATE UI
     * ----------------------------------------------------------
     */

    void updateBridgeUI(
        float
    ) {

        auto panel =
            this->getChildByID(
                "gdai-panel"_spr
            );


        if (!panel) {
            return;
        }


        auto status =
            panel->getChildByID(
                "gdai-status"_spr
            );


        if (!status) {
            return;
        }


        std::string text =
            "MOD: LOADED\n";


        if (
            g_connected
        ) {

            text +=
                "TCP: CONNECTED\n";

            if (
                g_pythonResponsive
            ) {

                text +=
                    "PYTHON: RESPONDING\n";
            }
            else {

                text +=
                    "PYTHON: CONNECTED\n";
            }
        }
        else {

            text +=
                "TCP: OFFLINE\n"
                "PYTHON: WAITING\n";
        }


        text +=
            "CONNECTIONS: " +
            std::to_string(
                g_connectionCount.load()
            ) +
            "\n";


        text +=
            "DISCONNECTS: " +
            std::to_string(
                g_disconnectCount.load()
            );


        status->setString(
            text.c_str()
        );
    }


    /*
     * ----------------------------------------------------------
     * RECONNECT
     * ----------------------------------------------------------
     */

    void onReconnect(
        CCObject*
    ) {

        log::info(
            "[GD AI Bridge] Manual reconnect requested"
        );


        g_pythonResponsive =
            false;


        closeSocket();


        log::info(
            "[GD AI Bridge] Socket closed. "
            "Automatic reconnect will start."
        );
    }


    /*
     * ----------------------------------------------------------
     * PING
     * ----------------------------------------------------------
     */

    void onPing(
        CCObject*
    ) {

        if (
            !g_connected
        ) {

            log::warn(
                "[GD AI Bridge] Ping requested "
                "while disconnected"
            );

            return;
        }


        g_pythonResponsive =
            false;


        bool success =
            sendJson(
                "{"
                "\"type\":\"command\","
                "\"action\":\"ping\""
                "}"
            );


        log::info(
            "[GD AI Bridge] Ping sent: {}",
            success
        );
    }


    /*
     * ----------------------------------------------------------
     * DEBUG
     * ----------------------------------------------------------
     */

    void onDebug(
        CCObject*
    ) {

        bool enabled =
            !g_debugBoxes.load();


        g_debugBoxes =
            enabled;


        if (
            g_connected
        ) {

            sendJson(
                std::string(
                    "{"
                    "\"type\":\"command\","
                    "\"action\":\"debug_boxes\","
                    "\"enabled\":"
                )
                +
                boolJson(
                    enabled
                )
                +
                "}"
            );
        }


        log::info(
            "[GD AI Bridge] Debug boxes: {}",
            enabled
        );
    }
};


/*
 * ============================================================
 * MOD LIFECYCLE
 * ============================================================
 */

$on_mod(Loaded) {

    log::info(
        "[GD AI Bridge] ========================================"
    );

    log::info(
        "[GD AI Bridge] MOD LOADED"
    );

    log::info(
        "[GD AI Bridge] Geometry Dash 2.2081"
    );

    log::info(
        "[GD AI Bridge] Geode protocol {}",
        PROTOCOL_VERSION
    );

    log::info(
        "[GD AI Bridge] TCP target {}:{}",
        HOST,
        PORT
    );

    log::info(
        "[GD AI Bridge] ========================================"
    );


    g_running =
        true;


    g_networkThread =
        std::thread(
            networkThread
        );
}


$on_mod(Unloaded) {

    log::info(
        "[GD AI Bridge] Unloading..."
    );


    g_running =
        false;


    closeSocket();


    if (
        g_networkThread.joinable()
    ) {

        g_networkThread.join();
    }


    clearDebugBoxes();


    g_playLayer =
        nullptr;


    log::info(
        "[GD AI Bridge] Unloaded."
    );
}


} // namespace GDABridge
