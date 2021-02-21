#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <assert.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include "3dc.h"
#include "stratdef.h"

#include "pldnet.h"
#include "avp_menus.h"

using system_clock = std::chrono::system_clock;
using time_point = std::chrono::time_point<system_clock>;

static const uint16_t DEFAULT_PORT = 50706;

#ifdef __cplusplus
extern "C" {
#endif
DPID AVPDPNetID;
int QuickStartMultiplayer=1;
char AVPDPplayerName[NET_PLAYERNAMELENGTH + 3] = "";
int glpDP; /* directplay object */
#ifdef __cplusplus
}
#endif

class Connection {
public:
    Connection(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
        : m_timeout(timeout)
    {}
    virtual ~Connection(){}
    virtual HRESULT ReceiveMessages() = 0;
    virtual HRESULT SendMessage(const char *data, DWORD dataSize) = 0;
    virtual void SendGreetings(const std::string &/*name*/){};

protected:
    std::chrono::milliseconds m_timeout;
};

class MessageData {
public:
    constexpr MessageData(const char *data, size_t size)
        : m_data(data)
        , m_size(size)
    {}
    constexpr inline const char *data() const { return m_data; }
    constexpr inline size_t size() const { return m_size;}

    inline MessageData& operator >>(std::string &val) {
        const uint16_t *len = reinterpret_cast<const uint16_t*>(m_data);
        assert(*len <= m_size);
        val.append(m_data + 2 , *len);
        m_data += 2 + *len;
        m_size -= 2 + *len;
        return *this;
    };

    template<typename T>
    inline MessageData& operator >> (T &val) {
        val = *reinterpret_cast<const T*>(m_data);
        m_data += sizeof (T);
        m_size -= sizeof (T);
        return *this;
    }
private:
    const char *m_data;
    size_t m_size;
};

enum class MessageType : uint8_t {
    Data,
    SessionInfo,
    AddPlayer,
    RemovePlayer
};

class MessageParser
{
    using MessageHandler = std::function <void(MessageType type, MessageData data)>;
public:
    MessageParser()
    {}

    void setMessageHandler(const MessageHandler &handler)
    {
        m_messageHandler = handler;
    }

    void parse(const char *data, uint32_t size)
    {
        if (m_parsed < sizeof (uint32_t) && size) {
            uint32_t diff = sizeof (uint32_t) - m_parsed;
            diff = min(diff, size);
            memcpy(&m_header, data, diff);
            size -= diff;
            m_parsed += diff;
            data += diff;
        }
        if (!size)
            return;
        uint32_t messageLength = m_header & 0x00ffffff;
        const auto bufferedSize = m_data.size();
        const auto remainingSize = messageLength - bufferedSize;
        if (size >= remainingSize) {
            m_parsed = 0;
            assert(m_messageHandler);
            if (bufferedSize) {
                m_data.append(data, remainingSize);
                m_messageHandler(MessageType(m_header>>24), {m_data.data(), messageLength});
                m_data.clear();
            } else {
                m_messageHandler(MessageType(m_header>>24), {data, messageLength});
            }
            return parse(data + remainingSize, size - remainingSize);
        } else {
            m_data.append(data, size);
        }
    }

private:
    uint32_t m_header;
    uint32_t m_parsed = 0;
    std::string m_data;
    MessageHandler m_messageHandler;
};

class MessageWriter : public std::string
{
public:
    MessageWriter() = default;

    inline MessageWriter &begin()
    {
        uint32_t len = 0;
        m_pos = size();
        append(reinterpret_cast<const char*>(&len), 4);
        return *this;
    }

    inline MessageWriter &operator <<(const std::string &string)
    {
        uint16_t len = string.size();
        append(reinterpret_cast<const char *>(&len), sizeof(len));
        append(string);
        return *this;
    }

    template<typename T>
    inline MessageWriter &operator <<(T val)
    {
        append(reinterpret_cast<const char *>(&val), sizeof (val));
        return *this;
    }

    inline MessageWriter &end(MessageType type)
    {
        assert(size() < 0x00ffffff);
        uint32_t header = (uint32_t(type) << 24) | size() - m_pos - sizeof (uint32_t);
        memcpy(const_cast<char*>(data() + m_pos), &header, sizeof(header));
        return *this;
    }

    inline MessageWriter &appendData(MessageType type, const char *data, size_t size)
    {
        uint32_t header = (uint32_t(type) << 24) | size;
        operator << (header);
        append(data, size);
        return *this;
    }
private:
    uint32_t m_pos;
};

class Server : public Connection
{
public:
    Server(const std::string &sessionName, uint16_t port = DEFAULT_PORT, size_t maxConnections = NET_MAXPLAYERS);
    ~Server();

    // IOChannel interface
    HRESULT ReceiveMessages() final;
    HRESULT SendMessage(const char *data, DWORD dataSize) final;

private:
    struct Socket {
        Socket(int sock, const std::string &sessionName, std::string &sharedBuffer)
            : socket(sock)
            , sharedBuffer(sharedBuffer)
        {
            readParser.setMessageHandler([this](MessageType type, MessageData data){
                switch (type) {
                case MessageType::Data:
                    ProcessGameMessage(socket, data.data(), data.size());
                    break;
                case MessageType::AddPlayer: {
                    int id;
                    std::string name;
                    data >> id >> name;
                    assert(!data.size());
                    assert(socket == id);
                    AddPlayerToGame(id, name.c_str());
                    // send back all players info
                    for (const auto &player : netGameData.playerData) {
                        if (!player.playerId)
                            continue;
                        writeBuffer.begin();
                        writeBuffer << player.playerId << std::string(player.name);
                        writeBuffer.end(MessageType::AddPlayer);
                    }
                    Flush();
                }
                    break;
                default:
                    assert(false);
                    break;
                }
            });
            int opt = 1;
            setsockopt(socket, SOL_TCP, TCP_NODELAY, &opt, sizeof(int));
            writeBuffer.begin();
            writeBuffer << sessionName;
            writeBuffer << socket;
            writeBuffer << netGameData.levelNumber;
            writeBuffer.end(MessageType::SessionInfo);
            Flush();
        }
        ~Socket() {
            std::cout << __PRETTY_FUNCTION__ << " " << socket << " " << std::endl;
            RemovePlayerFromGame(socket);
            ::shutdown(socket, SHUT_RDWR);
            ::close(socket);
        }

        HRESULT ReceiveMessages()
        {
            while (true) {
                ssize_t size = ::read(socket, &sharedBuffer[0], sharedBuffer.size());
                if (size < 0) {
                    if (errno != EAGAIN) {
                        return DPERR_CONNECTIONLOST;
                    }
                    return DP_OK;
                } else {
                    if (size)
                        readParser.parse(sharedBuffer.data(), size);
                    return DP_OK;
                }
            };
        }

        HRESULT SendMessage(const std::vector<int> &lostConnections, const char *data, size_t size)
        {
            writeBuffer.appendData(MessageType::Data, data, size);
            for (int id : lostConnections) {
                writeBuffer.begin();
                writeBuffer << id;
                writeBuffer.end(MessageType::RemovePlayer);
            }
            return Flush();
        }

        HRESULT Flush() {
            while (!writeBuffer.empty()) {
                ssize_t written = ::write(socket, writeBuffer.data(), writeBuffer.size());
                if (written < 0) {
                    if (errno != EAGAIN)
                        return DPERR_CONNECTIONLOST;
                    return DP_OK;
                } else {
                    if (!written)
                        return DP_OK;
                    writeBuffer.erase(0, written);
                }
            }
            return writeBuffer.empty() ? DP_OK : DPERR_BUSY;
        }
        int socket;
        std::string &sharedBuffer;
        MessageParser readParser;
        MessageWriter writeBuffer;
    };
    std::string m_sharedBuffer;
    std::vector<std::unique_ptr<Socket>> m_connections;
    std::vector<int> m_lostConnections;
    int m_acceptSocket;
    size_t m_maxConnections;
    std::string m_sesionName;
};

Server::Server(const std::string &sessionName, uint16_t port, size_t maxConnections)
    : m_maxConnections(maxConnections)
    , m_sesionName(sessionName)
{
    if ((m_acceptSocket = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0)
        throw std::runtime_error{"Can't create the socket"};
    std::cout << "Listen socker " << m_acceptSocket << std::endl;
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port        = htons(port);
    if (::bind(m_acceptSocket,(struct sockaddr *) &saddr, sizeof(saddr)) < 0) {
        ::close(m_acceptSocket);
        throw std::runtime_error{"Can't bind the socket"};
    }
    if (::listen(m_acceptSocket, 8) < 0) {
        ::close(m_acceptSocket);
        throw std::runtime_error{"Can't listen"};
    }
    int n;
    unsigned int m = sizeof(n);
    if (getsockopt(m_acceptSocket, SOL_SOCKET, SO_RCVBUF,(void *)&n, &m) != 0 || n < 4096)
        n = 4 * 1024 * 1024;
    m_sharedBuffer.resize(n);
    AVPDPNetID = m_acceptSocket;
}

Server::~Server()
{
    ::close(m_acceptSocket);
}

HRESULT Server::ReceiveMessages()
{
    struct sockaddr_storage in_addr;
    socklen_t in_len = sizeof(struct sockaddr_storage);
    while (m_connections.size() < m_maxConnections) {
        int sock = ::accept4(m_acceptSocket, (struct sockaddr *)&in_addr, &in_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (-1 == sock)
            break;
        try {
            m_connections.push_back(std::make_unique<Socket>(sock, m_sesionName, m_sharedBuffer));
        } catch (...) {
            ::close(sock);
        }
    }

    for (auto it = m_connections.begin(); it != m_connections.end();) {
        auto res = (*it)->ReceiveMessages();
        switch (res) {
        case DPERR_CONNECTIONLOST:
            m_lostConnections.push_back((*it)->socket);
            it = m_connections.erase(it);
            break;
        default:
            ++it;
        }
    }
    return DP_OK;
}

HRESULT Server::SendMessage(const char *data, DWORD dataSize)
{
    bool busy = false;
    for (auto it = m_connections.begin(); it != m_connections.end();) {
        auto res =(*it)->Flush();
        switch (res) {
        case DPERR_CONNECTIONLOST:
            m_lostConnections.push_back((*it)->socket);
            it = m_connections.erase(it);
            break;
        case DPERR_BUSY:
            busy = true;
//            [[fallthrough]]
        default:
            ++it;
        }
    }
    if (busy)
        return DPERR_BUSY;

    auto lostConnections = std::move(m_lostConnections);
    m_lostConnections.clear();
    for (auto it = m_connections.begin(); it != m_connections.end();) {
        auto res = (*it)->SendMessage(lostConnections, data, dataSize);
        switch (res) {
        case DPERR_CONNECTIONLOST:
            m_lostConnections.push_back((*it)->socket);
            it = m_connections.erase(it);
            break;
        default:
            ++it;
        }
    }
    return DP_OK;
}

class Client: public Connection
{
public:
    Client(const char * ipAddress, uint16_t port= DEFAULT_PORT);
    ~Client();

    // IOChannel interface
    HRESULT ReceiveMessages() final;
    HRESULT SendMessage(const char *data, DWORD size) final;
    HRESULT Flush();
    void SendGreetings(const std::string &name) final;

private:
    int BeHost(int res);
    int m_socket;
    std::string m_sharedBuffer;
    MessageParser m_readParser;
    MessageWriter m_writeBuffer;
};

Client::Client(const char *ipAddress, uint16_t port)
{
    struct hostent *server = gethostbyname(ipAddress);
    if (!server)
        throw std::runtime_error{"Invalid host"};

    if ((m_socket = ::socket(AF_INET, SOCK_STREAM, 0)) < 0)
        throw std::runtime_error{"Can't create the socket"};
    std::cout << "Client socker " << m_socket << std::endl;
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);
    if (::connect(m_socket,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) != 0) {
        ::close(m_socket);
        throw std::runtime_error{"Can't connect"};
    }
    int flags = ::fcntl(m_socket, F_GETFL, 0);
    if (flags == -1) {
        ::close(m_socket);
        throw std::runtime_error{"Can't get flags"};
    }
    flags |= O_NONBLOCK;
    if (::fcntl(m_socket, F_SETFL, flags) != 0) {
        ::close(m_socket);
        throw std::runtime_error{"Can't set flags"};
    }
    int n = 4096;
    unsigned int m = sizeof(n);
    if (getsockopt(m_socket, SOL_SOCKET, SO_RCVBUF,(void *)&n, &m) != 0 || n < 4096)
        n = 4 * 1024 * 1024;
    m_sharedBuffer.resize(n);
    int opt = 1;
    setsockopt(m_socket, SOL_TCP, TCP_NODELAY, &opt, sizeof(int));
    AVPDPNetID = 0;
    m_readParser.setMessageHandler([this](MessageType type, MessageData data){
        switch (type) {
        case MessageType::Data:
            ProcessGameMessage(m_socket, data.data(), data.size());
            break;
        case MessageType::SessionInfo: {
            std::string name;
            data >> name >> AVPDPNetID >> SessionData[0].levelIndex;
            assert(!data.size());
            std::cout << "Found server " << name
                      << " client id " << AVPDPNetID
                      << " level index" << SessionData[0].levelIndex
                      << std::endl;
            SessionData[0].AllowedToJoin = true;
            strncpy(&SessionData[0].Name[0], name.c_str(), 40);
            NumberOfSessionsFound = 1;
        }
            break;
        case MessageType::AddPlayer: {
            DPID playerId;
            std::string name;
            data >> playerId >> name;
            assert(!data.size());
            std::cout << "AddPlayer " << playerId << " " << name << std::endl;
            AddPlayerToGame(playerId, name.c_str());
        }
            break;
        case MessageType::RemovePlayer: {
            DPID playerId;
            data >> playerId;
            assert(!data.size());
            std::cout << "RemovePlayer " << playerId << std::endl;
            RemovePlayerFromGame(playerId);
        }
            break;
        default:
            assert(false);
            break;
        }
    });
}

Client::~Client()
{
    std::cout << __PRETTY_FUNCTION__ << " " << m_socket << " " << std::endl;
    RemovePlayerFromGame(AVPDPNetID);
    ::shutdown(m_socket, SHUT_RDWR);
    ::close(m_socket);
}

HRESULT Client::ReceiveMessages()
{
    while (true) {
        ssize_t size = ::read(m_socket, &m_sharedBuffer[0], m_sharedBuffer.size());
        if (size < 0) {
            if (errno != EAGAIN)
                return BeHost(DPERR_CONNECTIONLOST);
            return DP_OK;
        } else {
            if (size)
                m_readParser.parse(m_sharedBuffer.data(), size);
            return DP_OK;
        }
    };
}

HRESULT Client::SendMessage(const char *data, DWORD size)
{
    auto res = Flush();
    if (res != DP_OK)
        return res;
    m_writeBuffer.appendData(MessageType::Data, data, size);
    res = Flush();
    if (res == DPERR_CONNECTIONLOST)
        return BeHost(DPERR_CONNECTIONLOST);
    return DP_OK;
}

HRESULT Client::Flush()
{
    while (!m_writeBuffer.empty()) {
        ssize_t written = ::write(m_socket, m_writeBuffer.data(), m_writeBuffer.size());
        if (written < 0) {
            if (errno != EAGAIN)
                return BeHost(DPERR_CONNECTIONLOST);
            return DP_OK;
        } else {
            if (!written)
                return DP_OK;
            m_writeBuffer.erase(0, written);
        }
    }
    return m_writeBuffer.empty() ? DP_OK : DPERR_BUSY;
}

void Client::SendGreetings(const std::string &name)
{
    m_writeBuffer.begin();
    m_writeBuffer << AVPDPNetID << name;
    m_writeBuffer.end(MessageType::AddPlayer);
    Flush();
}

int Client::BeHost(int res)
{
    if (AvP.Network != I_Peer)
        return res;

    for (const auto &player : netGameData.playerData) {
        if (player.playerId && player.playerId != AVPDPNetID)
            RemovePlayerFromGame(player.playerId);
    }

    /* Aha... the host has died, then. This is a terminal game state,
    as the host was managing the game. Thefore, temporarily adopt host
    duties for the purpose of ending the game...
    This is most important during the playing state, but also happens in
    startup. In startup, peers keep a host timeout which should fire before
    this message arrives anyway. */
    if((netGameData.myGameState==NGS_StartUp)||(netGameData.myGameState==NGS_Playing)||(netGameData.myGameState==NGS_Joining)||(netGameData.myGameState==NGS_EndGameScreen))
    {
        AvP.Network=I_Host;
        /* Eek, I guess the old AIs bite the dust? */
        //but the new host can create some more
        AvP.NetworkAIServer = (netGameData.gameType==NGT_Coop);
        Inform_NewHost();
//        TransmitEndOfGameNetMsg();
//        netGameData.myGameState = NGS_EndGame;
//        AvP.MainLoopRunning = 0;

        if(LobbiedGame)
        {
            //no longer a lowly client
            LobbiedGame=LobbiedGame_Server;
        }
    }
    LogNetInfo("system message:  DPSYS_HOST \n");
    glpDP = 0; // don't recv/send messages anymore
    return res;
}

static std::unique_ptr<Connection> connection;

#ifdef __cplusplus
extern "C" {
#endif

void InitializeNetwork()
{
    // Ignore sigpipe
    signal(SIGPIPE, SIG_IGN);
}

void ShutdownNetwork()
{
    connection.reset();
}

BOOL NetSessionInit(DWORD cGrntdBufs, DWORD cBytesPerBuf, BOOL bErrChcks)
{
    NumberOfSessionsFound = 0;
    fprintf(stderr, "DpExtInit(%d, %d, %d)\n", cGrntdBufs, cBytesPerBuf, bErrChcks);

    return TRUE;
}

void NetSessionUnInit()
{
    connection.reset();
    NumberOfSessionsFound = 0;
    fprintf(stderr, "DpExtUnInit()\n");
    glpDP = 0;
}

HRESULT NetSessionReceiveMessages()
{
    if (connection)
        return connection->ReceiveMessages();
    return DPERR_CONNECTIONLOST;
}

HRESULT NetSessionSendMessage(const char *lpData, DWORD dwDataSize)
{
    if (connection)
        return connection->SendMessage(lpData, dwDataSize);
    return DPERR_CONNECTIONLOST;
}

/* directplay.c */
int NetConnectingToLobbiedGame(char* playerName)
{
    fprintf(stderr, "NetConnectingToLobbiedGame(%s)\n", playerName);

    return 0;
}

int NetConnectToSession(int sessionNumber, char *playerName)
{
    if (!connection || !AVPDPNetID)
        return 0;
    glpDP = 1;
    connection->SendGreetings(playerName);
    fprintf(stderr, "NetConnectToSession(%d, %s)\n", sessionNumber, playerName);
    InitAVPNetGameForJoin();
    netGameData.levelNumber = SessionData[sessionNumber].levelIndex;
    netGameData.joiningGameStatus = JOINNETGAME_WAITFORDESC;
    return 1;
}

int NetConnectingToSession()
{
//    fprintf(stderr, "NetConnectingToSession()\n");
    MinimalNetCollectMessages();
    if(!netGameData.needGameDescription)
    {
        //we now have the game description , so we can go to the configuration menu
        return AVPMENU_MULTIPLAYER_CONFIG_JOIN;
    }
    return 1;
}

BOOL NetUpdateSessionList(int */*SelectedItem*/)
{
    if (connection && !NumberOfSessionsFound)
        connection->ReceiveMessages();
//    fprintf(stderr, "NetUpdateSessionList(%p)\n", SelectedItem);
    return NumberOfSessionsFound;
}

int NetJoinGame()
{
    extern const char IPAddressString[];
    try {
        connection = std::make_unique<Client>(IPAddressString);
    } catch(...) {}
    fprintf(stderr, "NetJoinGame(%s)\n", IPAddressString);
    return NumberOfSessionsFound = 0;
}

void NetEnumConnections()
{
    fprintf(stderr, "NetEnumConnections()\n");

    netGameData.tcpip_available = 1;
    netGameData.ipx_available = 0;
    netGameData.modem_available = 0;
    netGameData.serial_available = 0;
}

int NetHostGame(char *playerName, char *sessionName,int species,int gamestyle,int level)
{
    try {
        connection = std::make_unique<Server>(sessionName);
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        AVPDPNetID = 0;
        return 0;
    } catch (...) { return 0; }

    strncpy(AVPDPplayerName, playerName, NET_PLAYERNAMELENGTH + 2);

    if(!netGameData.skirmishMode) {
        fprintf(stderr, "NetHostGame(%s, %s, %d, %d, %d)\n", playerName, sessionName, species, gamestyle, level);
        glpDP = 1;
    }

    InitAVPNetGameForHost(species,gamestyle,level);
    return 1;
}

int NetDisconnectSession()
{
    connection.reset();
    glpDP = 0;
    fprintf(stderr, "NetDisconnect()\n");
    return 1;
}

HRESULT NetGetPlayerName(int glpDP, DPID id, char *data, DWORD *size)
{
    for (const auto &player : netGameData.playerData) {
        if (player.playerId == id) {
            if (data) {
                if (*size <= strlen(player.name))
                    return DPERR_BUFFERTOOSMALL;
                strncpy(data, player.name, *size);
            }
            *size = strlen(player.name) + 1;
            return DP_OK;
        }
    }
    fprintf(stderr, "IDirectPlayX_GetPlayerName(%d, %d, %p, %p)\n", glpDP, id, data, size);
    return DPERR_INVALIDPLAYER;
}
#ifdef __cplusplus
};
#endif
