#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/FLAlertLayer.hpp>

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
static constexpr int PROTOCOL = 2;


/* ============================================================
 * GLOBAL STATE
 * ============================================================ */

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

static CCNode* g_debugNode = nullptr;


/* ============================================================
 * PLAYER VELOCITY HISTORY
 * ============================================================ */

static float g_lastP1X = 0.f;
static float g_lastP1Y = 0.f;
static bool g_haveP1Position = false;

static float g_lastP2X = 0.f;
static float g_lastP2Y = 0.f;
static bool g_haveP2Position = false;


/* ============================================================
 * TIMING
 * ============================================================ */

static std::chrono::steady_clock::time_point g_lastStateTime =
    std::chrono::steady_clock::now();

static std::chrono::steady_clock::time_point g_lastObjectTime =
    std::chrono::steady_clock::now();


/* ============================================================
 * JSON HELPERS
 * ============================================================ */

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


/* ============================================================
 * SOCKET
 * ============================================================ */

static void closeSocket() {

    std::lock_guard<std::mutex> lock(
        g_socketMutex
    );

    bool wasConnected =
        g_connected.load();

    if (
        g_socket !=
        INVALID_SOCKET
    ) {

        shutdown(
            g_socket,
            SD_BOTH
        );

        closesocket(
            g_socket
        );

        g_socket =
            INVALID_SOCKET;
    }

    g_connected =
        false;

    g_pythonResponsive =
        false;

    if (wasConnected) {

        g_disconnectCount.fetch_add(
            1
        );
    }
}


static bool sendRaw(
    const std::string& data
) {

    std::lock_guard<std::mutex> lock(
        g_socketMutex
    );

    if (
        g_socket ==
        INVALID_SOCKET
    ) {

        return false;
    }


    size_t sentTotal = 0;


    while (
        sentTotal <
        data.size()
    ) {

        int sent =
            send(
                g_socket,
                data.data() + sentTotal,
                static_cast<int>(
                    data.size() -
                    sentTotal
                ),
                0
            );


        if (
            sent <= 0
        ) {

            return false;
        }


        sentTotal +=
            static_cast<size_t>(
                sent
            );
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


/* ============================================================
 * COMMAND QUEUE
 * ============================================================ */

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


static std::vector<std::string>
takeCommands() {

    std::lock_guard<std::mutex> lock(
        g_commandMutex
    );

    auto result =
        g_commands;

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


/* ============================================================
 * PROCESS COMMAND
 * ============================================================ */

static void processCommand(
    const std::string& command
) {

    log::info(
        "[GD AI Bridge] RX: {}",
        command
    );


    /* --------------------------------------------------------
     * PING
     * -------------------------------------------------------- */

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

        g_pythonResponsive =
            true;

        return;
    }


    /* --------------------------------------------------------
     * DEBUG BOXES
     * -------------------------------------------------------- */

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

            g_debugBoxes =
                true;
        }

        else if (
            contains(
                command,
                "\"enabled\":false"
            )
        ) {

            g_debugBoxes =
                false;
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


        return;
    }


    /* --------------------------------------------------------
     * STATE HZ
     * -------------------------------------------------------- */

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

            position +=
                8;


            int value =
                std::atoi(
                    command.c_str() +
                    position
                );


            if (
                value < 1
            ) {

                value = 1;
            }


            if (
                value > 120
            ) {

                value = 120;
            }


            g_stateHz =
                value;


            sendJson(
                std::string(
                    "{"
                    "\"type\":\"command_result\","
                    "\"action\":\"state_hz\","
                    "\"value\":"
                )
                +
                std::to_string(
                    value
                )
                +
                "}"
            );
        }


        return;
    }


    /* --------------------------------------------------------
     * JUMP
     * -------------------------------------------------------- */

    if (
        contains(
            command,
            "\"action\":\"jump\""
        )
    ) {

        auto playLayer =
            PlayLayer::get();


        if (
            playLayer &&
            playLayer->m_player1
        ) {

            playLayer
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


    /* --------------------------------------------------------
     * RELEASE
     * -------------------------------------------------------- */

    if (
        contains(
            command,
            "\"action\":\"release\""
        )
    ) {

        auto playLayer =
            PlayLayer::get();


        if (
            playLayer &&
            playLayer->m_player1
        ) {

            playLayer
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


    /* --------------------------------------------------------
     * UNKNOWN COMMAND
     * -------------------------------------------------------- */

    sendJson(
        "{"
        "\"type\":\"command_result\","
        "\"success\":false,"
        "\"reason\":\"unknown_command\""
        "}"
    );
}


/* ============================================================
 * PROCESS ALL PENDING COMMANDS
 * ============================================================ */

static void processPendingCommands() {

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
}


/* ============================================================
 * NETWORK RECEIVE
 * ============================================================ */

static void receiveLoop() {

    std::string buffer;

    char temp[8192];


    while (
        g_running
    ) {

        SOCKET socketCopy;


        {
            std::lock_guard<std::mutex> lock(
                g_socketMutex
            );

            socketCopy =
                g_socket;
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


        if (
            received <= 0
        ) {

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


            /* Python response to our ping */

            if (
                contains(
                    line,
                    "\"type\":\"pong\""
                )
            ) {

                g_pythonResponsive =
                    true;

                log::info(
                    "[GD AI Bridge] Python responded."
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


/* ============================================================
 * CONNECT
 * ============================================================ */

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
            "[GD AI Bridge] socket() failed."
        );

        return false;
    }


    sockaddr_in address{};


    address.sin_family =
        AF_INET;


    address.sin_port =
        htons(
            PORT
        );


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
        "[GD AI Bridge] ================================"
    );

    log::info(
        "[GD AI Bridge] TCP CONNECTED"
    );

    log::info(
        "[GD AI Bridge] {}:{}",
        HOST,
        PORT
    );

    log::info(
        "[GD AI Bridge] ================================"
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


    sendJson(
        "{"
        "\"type\":\"ping\","
        "\"source\":\"gd_mod\""
        "}"
    );


    return true;
}


/* ============================================================
 * NETWORK THREAD
 * ============================================================ */

static void networkThread() {

    WSADATA data{};


    if (
        WSAStartup(
            MAKEWORD(2, 2),
            &data
        ) != 0
    ) {

        log::error(
            "[GD AI Bridge] WSAStartup failed."
        );

        return;
    }


    log::info(
        "[GD AI Bridge] Network thread started."
    );


    while (
        g_running
    ) {

        if (
            !g_connected
        ) {

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
        "[GD AI Bridge] Network thread stopped."
    );
}


/* ============================================================
 * PLAYER JSON
 * ============================================================ */

static std::string playerJson(
    PlayerObject* player,
    float dt,
    int playerIndex
) {

    if (
        !player
    ) {

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
                (
                    x -
                    g_lastP1X
                ) / dt;


            vy =
                (
                    y -
                    g_lastP1Y
                ) / dt;
        }


        g_lastP1X =
            x;

        g_lastP1Y =
            y;

        g_haveP1Position =
            true;
    }

    else {

        if (
            g_haveP2Position &&
            dt > 0.f
        ) {

            vx =
                (
                    x -
                    g_lastP2X
                ) / dt;


            vy =
                (
                    y -
                    g_lastP2Y
                ) / dt;
        }


        g_lastP2X =
            x;

        g_lastP2Y =
            y;

        g_haveP2Position =
            true;
    }


    return
        "{"
        "\"x\":" +
        numberJson(
            x
        ) +
        ",\"y\":" +
        numberJson(
            y
        ) +
        ",\"vx\":" +
        numberJson(
            vx
        ) +
        ",\"vy\":" +
        numberJson(
            vy
        ) +
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
            player->m_isUpsideDown
        ) +
        ",\"onGround\":" +
        boolJson(
            player->m_isOnGround
        ) +
        "}";
}


/* ============================================================
 * OBJECT JSON
 * ============================================================ */

static std::string objectsJson(
    PlayLayer* layer
) {

    if (
        !layer
    ) {

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

            if (
                !object
            ) {

                continue;
            }


            if (
                !first
            ) {

                result += ",";
            }


            first =
                false;


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


    result +=
        "]";


    return result;
}


/* ============================================================
 * DEBUG DRAWING
 * ============================================================ */

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


    if (
        !node
    ) {

        return;
    }


    node->setZOrder(
        9999
    );


    /* Player 1 */

    if (
        layer->m_player1
    ) {

        auto rect =
            layer
                ->m_player1
                ->getObjectRect();


        node->drawRect(
            rect.origin,
            rect.origin +
                rect.size,
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


    /* Player 2 */

    if (
        layer->m_player2
    ) {

        auto rect =
            layer
                ->m_player2
                ->getObjectRect();


        node->drawRect(
            rect.origin,
            rect.origin +
                rect.size,
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


    /* Objects */

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

            if (
                !object
            ) {

                continue;
            }


            auto rect =
                object->getObjectRect();


            node->drawRect(
                rect.origin,
                rect.origin +
                    rect.size,
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


/* ============================================================
 * PLAYLAYER
 * ============================================================ */

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


        if (
            !result
        ) {

            return false;
        }


        g_haveP1Position =
            false;

        g_haveP2Position =
            false;


        log::info(
            "[GD AI Bridge] PlayLayer initialized."
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


        /*
         * Commands are processed on GD's thread.
         */

        processPendingCommands();


        /*
         * State
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
                now -
                g_lastStateTime
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
                    *
                    100.f;
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
                numberJson(
                    dt
                ) +
                ",\"progress\":" +
                numberJson(
                    progress
                ) +
                ",\"paused\":" +
                boolJson(
                    m_isPaused
                ) +
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
         * Objects
         */

        float objectElapsed =
            std::chrono::duration<float>(
                now -
                g_lastObjectTime
            ).count();


        if (
            objectElapsed >= 0.1f &&
            g_connected
        ) {

            g_lastObjectTime =
                now;


            int count =
                0;


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
         * Debug overlay
         */

        static int debugCounter = 0;

        debugCounter++;


        if (
            g_debugBoxes &&
            debugCounter >= 3
        ) {

            debugCounter =
                0;

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


        clearDebugBoxes();


        PlayLayer::onExit();
    }
};


/* ============================================================
 * DIAGNOSTIC UI
 * ============================================================ */

enum {
    PANEL_TAG = 7001,
    STATUS_TAG = 7002
};


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


        auto winSize =
            CCDirector::sharedDirector()
                ->getWinSize();


        /*
         * Main menu button
         */

        auto mainButtonSprite =
            ButtonSprite::create(
                "AI",
                "goldFont.fnt",
                "GJ_button_01.png",
                0.7f
            );


        auto mainButton =
            CCMenuItemSpriteExtra::create(
                mainButtonSprite,
                this,
                menu_selector(
                    GDABridgeMenuLayer::openPanel
                )
            );


        auto mainMenu =
            CCMenu::create();


        mainMenu->setPosition(
            {
                winSize.width - 45.f,
                45.f
            }
        );


        mainMenu->addChild(
            mainButton
        );


        mainMenu->setZOrder(
            10000
        );


        this->addChild(
            mainMenu
        );


        /*
         * Diagnostic panel
         */

        auto panel =
            CCLayerColor::create(
                ccc4(
                    10,
                    10,
                    10,
                    235
                ),
                380.f,
                225.f
            );


        if (
            !panel
        ) {

            return true;
        }


        panel->setPosition(
            {
                20.f,
                20.f
            }
        );


        panel->setZOrder(
            10001
        );


        panel->setTag(
            PANEL_TAG
        );


        panel->setVisible(
            false
        );


        this->addChild(
            panel
        );


        /*
         * Title
         */

        auto title =
            CCLabelBMFont::create(
                "GD AI BRIDGE",
                "bigFont.fnt"
            );


        title->setScale(
            0.55f
        );


        title->setAnchorPoint(
            {
                0.f,
                0.5f
            }
        );


        title->setPosition(
            {
                20.f,
                195.f
            }
        );


        panel->addChild(
            title
        );


        /*
         * Status
         */

        auto status =
            CCLabelBMFont::create(
                "MOD: LOADED",
                "bigFont.fnt"
            );


        status->setScale(
            0.33f
        );


        status->setAnchorPoint(
            {
                0.f,
                1.f
            }
        );


        status->setPosition(
            {
                20.f,
                172.f
            }
        );


        status->setTag(
            STATUS_TAG
        );


        panel->addChild(
            status
        );


        /*
         * Endpoint
         */

        auto endpoint =
            CCLabelBMFont::create(
                "TCP 127.0.0.1:8765",
                "bigFont.fnt"
            );


        endpoint->setScale(
            0.28f
        );


        endpoint->setPosition(
            {
                190.f,
                88.f
            }
        );


        panel->addChild(
            endpoint
        );


        /*
         * RECONNECT
         */

        auto reconnectSprite =
            ButtonSprite::create(
                "RECONNECT",
                "goldFont.fnt",
                "GJ_button_01.png",
                0.7f
            );


        auto reconnect =
            CCMenuItemSpriteExtra::create(
                reconnectSprite,
                this,
                menu_selector(
                    GDABridgeMenuLayer::onReconnect
                )
            );


        /*
         * PING
         */

        auto pingSprite =
            ButtonSprite::create(
                "PING",
                "goldFont.fnt",
                "GJ_button_02.png",
                0.7f
            );


        auto ping =
            CCMenuItemSpriteExtra::create(
                pingSprite,
                this,
                menu_selector(
                    GDABridgeMenuLayer::onPing
                )
            );


        /*
         * DEBUG
         */

        auto debugSprite =
            ButtonSprite::create(
                "DEBUG",
                "goldFont.fnt",
                "GJ_button_03.png",
                0.7f
            );


        auto debug =
            CCMenuItemSpriteExtra::create(
                debugSprite,
                this,
                menu_selector(
                    GDABridgeMenuLayer::onDebug
                )
            );


        /*
         * CLOSE
         */

        auto closeSprite =
            ButtonSprite::create(
                "CLOSE",
                "goldFont.fnt",
                "GJ_button_04.png",
                0.7f
            );


        auto close =
            CCMenuItemSpriteExtra::create(
                closeSprite,
                this,
                menu_selector(
                    GDABridgeMenuLayer::closePanel
                )
            );


        /*
         * Menu
         */

        auto menu =
            CCMenu::create();


        menu->setPosition(
            {
                190.f,
                42.f
            }
        );


        reconnect->setPosition(
            {
                -120.f,
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
                120.f,
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
         * Close button separately
         */

        close->setPosition(
            {
                190.f,
                -5.f
            }
        );


        panel->addChild(
            close
        );


        /*
         * UI update
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
            "[GD AI Bridge] Diagnostic UI initialized."
        );


        return true;
    }


    void updateBridgeUI(
        float
    ) {

        /*
         * The command queue is also processed from
         * the main menu, so PING works without opening a level.
         */

        processPendingCommands();


        auto panel =
            this->getChildByTag(
                PANEL_TAG
            );


        if (
            !panel
        ) {

            return;
        }


        auto status =
            static_cast<CCLabelBMFont*>(
                panel->getChildByTag(
                    STATUS_TAG
                )
            );


        if (
            !status
        ) {

            return;
        }


        std::string text;


        text +=
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


    void openPanel(
        CCObject*
    ) {

        auto panel =
            this->getChildByTag(
                PANEL_TAG
            );


        if (
            panel
        ) {

            panel->setVisible(
                true
            );


            updateBridgeUI(
                0.f
            );
        }
    }


    void closePanel(
        CCObject*
    ) {

        auto panel =
            this->getChildByTag(
                PANEL_TAG
            );


        if (
            panel
        ) {

            panel->setVisible(
                false
            );
        }
    }


    void onReconnect(
        CCObject*
    ) {

        log::info(
            "[GD AI Bridge] Manual reconnect requested."
        );


        closeSocket();


        /*
         * The network thread automatically tries again.
         */

        log::info(
            "[GD AI Bridge] Automatic reconnect enabled."
        );
    }


    void onPing(
        CCObject*
    ) {

        if (
            !g_connected
        ) {

            log::warn(
                "[GD AI Bridge] Cannot ping: not connected."
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


/* ============================================================
 * MOD LOAD
 * ============================================================ */

$on_mod(Loaded) {

    log::info(
        "[GD AI Bridge] =================================="
    );

    log::info(
        "[GD AI Bridge] MOD LOADED"
    );

    log::info(
        "[GD AI Bridge] GD 2.2081"
    );

    log::info(
        "[GD AI Bridge] Geode 5.10.1"
    );

    log::info(
        "[GD AI Bridge] TCP {}:{}",
        HOST,
        PORT
    );

    log::info(
        "[GD AI Bridge] =================================="
    );


    g_running =
        true;


    g_networkThread =
        std::thread(
            networkThread
        );
}


} // namespace GDABridge
