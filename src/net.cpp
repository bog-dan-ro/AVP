#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "fixer.h"

#include "3dc.h"
#include "inline.h"
#include "module.h"
#include "stratdef.h"
#include "equipmnt.h"

#include "pldnet.h"
#include "net.h"
#include "avp_menus.h"

using system_clock = std::chrono::system_clock;
using time_point = std::chrono::time_point<system_clock>;

static const uint16_t DEFAULT_PORT = 50706;

class Connection {
public:
    Connection(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
        : m_timeout(timeout)
    {}
    virtual ~Connection(){}
    virtual HRESULT Receive() = 0;
    virtual HRESULT Send(const char *data, DWORD dataSize) = 0;
    virtual HRESULT GetPlayerName(int id, char *data, DWORD *size) = 0;
    virtual bool Established() const = 0;
    virtual void SendGreetings(std::string name) = 0;

protected:
    std::chrono::milliseconds m_timeout;
};

class Greetings {
public:
    std::string name;
    std::function<void(const std::string &)> completedCallback;
    bool completed = false;
    size_t Parse(const char * data, size_t size)
    {
        if (completed || !size)
            return 0;
        u_int8_t res = 0;
        if (!m_parsing) {
            m_parsing = true;
            m_remainingBytes = *data++;
            --size;
            ++res;
        }
        res = std::min<size_t>(m_remainingBytes, size);
        name.append(data, res);
        m_remainingBytes -= res;
        if (!m_remainingBytes) {
            completed = true;
            if (completedCallback)
                completedCallback(name);
        }
        return res;
    }
private:
    uint8_t m_remainingBytes;
    bool m_parsing = false;
};

class Server : public Connection
{
public:
    Server(const std::string &name, uint16_t port = DEFAULT_PORT, size_t maxConnections = NET_MAXPLAYERS);
    ~Server();
    // IOChannel interface
    HRESULT Receive() final;
    HRESULT Send(const char *data, DWORD dataSize) final;
    HRESULT GetPlayerName(int id, char *data, DWORD *size) final;
    bool Established() const final { return true; }
    void SendGreetings(std::string /*name*/) final {}
private:
    struct Socket {
        Socket(int sock, const std::string &grStr, std::string &readBuffer)
            : socket(sock)
            , readBuffer(readBuffer)
        {
            int opt = 1;
            setsockopt(socket, SOL_TCP, TCP_NODELAY, &opt, sizeof(int));
            greetings.completedCallback = [this] (const std::string &name) {
                std::cout << __PRETTY_FUNCTION__ << " " << socket << " " << name << std::endl;
                AddPlayerToGame(socket, name.c_str());
            };
            Send(grStr.data(), grStr.size());
        }
        ~Socket() {
            std::cout << __PRETTY_FUNCTION__ << " " << socket << " " << std::endl;
            RemovePlayerFromGame(socket);
            ::shutdown(socket, SHUT_RDWR);
            ::close(socket);
        }
        HRESULT Recive()
        {
            ssize_t size = ::read(socket, &readBuffer[0], readBuffer.size());
            if (size < 0) {
                if (errno != EAGAIN) {
                    return DPERR_CONNECTIONLOST;
                }
            } else {
                if (size) {
                    size_t pos = !greetings.completed ? greetings.Parse(readBuffer.data(), size) : 0;
                    ProcessGameMessage(socket, readBuffer.data() + pos, size - pos);
                }
            }
            return DP_OK;
        }
        HRESULT Send(const char *data, size_t size)
        {
            ssize_t written = ::write(socket, data, size);
            if (written < 0) {
                if (errno != EAGAIN) {
                    return DPERR_CONNECTIONLOST;
                }
                bytesToWrite = size;
            } else {
                bytesToWrite = size - written;
            }
            return bytesToWrite ? DPERR_BUSY : DP_OK;
        }

        HRESULT SendRemainingBytes()
        {
            if (!bytesToWrite)
                return DP_OK;
            auto res = Send(buffer->data() + buffer->length() - bytesToWrite, bytesToWrite);
            if (res == DP_OK) {
                buffer.reset();
            }
            return res;
        }
        int socket;
        time_point timeout = system_clock::now();
        Greetings greetings;
        int bytesToWrite = 0;
        std::shared_ptr<std::string> buffer;
        std::string &readBuffer;
    };
    std::vector<std::unique_ptr<Socket>> m_connections;
    std::string m_readBuffer;
    int m_acceptSocket;
    size_t m_maxConnections;
    std::string m_greetings;
};

Server::Server(const std::string &name, uint16_t port, size_t maxConnections)
    : m_maxConnections(maxConnections)
    , m_greetings(name)
{
    m_greetings.insert(0, 1, m_greetings.size());
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
    m_readBuffer.resize(n);
}

Server::~Server()
{
    ::close(m_acceptSocket);
}

HRESULT Server::Receive()
{
    struct sockaddr_storage in_addr;
    socklen_t in_len = sizeof(struct sockaddr_storage);
    while (m_connections.size() < m_maxConnections) {
        int sock = ::accept4(m_acceptSocket, (struct sockaddr *)&in_addr, &in_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (-1 == sock)
            break;
        m_connections.push_back(std::make_unique<Socket>(sock, m_greetings, m_readBuffer));
    }

    for (auto it = m_connections.begin(); it != m_connections.end();) {
        auto res = (*it)->Recive();
        switch (res) {
        case DPERR_CONNECTIONLOST:
            it = m_connections.erase(it);
            break;
        default:
            ++it;
        }
    }
    return DP_OK;
}

HRESULT Server::Send(const char *data, DWORD dataSize)
{
    bool busy = false;
    for (auto it = m_connections.begin(); it != m_connections.end();) {
        auto res = (*it)->SendRemainingBytes();
        switch (res) {
        case DPERR_CONNECTIONLOST:
            it = m_connections.erase(it);
            break;
        case DPERR_BUSY:
            busy = true;
        default: // fallthrough
            ++it;
        }
    }
    if (busy)
        return DPERR_BUSY;

    std::shared_ptr<std::string> sharedBuffer;
    for (auto it = m_connections.begin(); it != m_connections.end();) {
        auto res = (*it)->Send(data, dataSize);
        switch (res) {
        case DPERR_CONNECTIONLOST:
            it = m_connections.erase(it);
            break;
        case DPERR_BUSY:
            if (!sharedBuffer)
                sharedBuffer = std::make_shared<std::string>(data, dataSize);
            (*it)->buffer = sharedBuffer;
        default: // fallthrough
            ++it;
        }
    }
    return DP_OK;
}

HRESULT Server::GetPlayerName(int id, char *data, DWORD *size)
{
    for (const auto &con : m_connections) {
        if (con->socket == id) {
            const auto &name = con->greetings.name;
            if (!data) {
                *size = name.size() + 1;
            } else if (*size <= name.size()) {
                *size = name.size() + 1;
                return DPERR_BUFFERTOOSMALL;
            } else {
                strncpy(data, name.c_str(), *size);
            }
            return DP_OK;
        }
    }
    return DPERR_INVALIDPLAYER;
}

class Client: public Connection
{
public:
    Client(const char * ipAddress, uint16_t port= DEFAULT_PORT);
    ~Client();
    // IOChannel interface
    HRESULT Receive() final;
    HRESULT Send(const char *data, DWORD dataSize) final;
    bool Established() const final { return m_greetings.completed; }
    void SendGreetings(std::string name) final;

private:
    int m_socket;
    std::string m_readBuffer;
    std::string m_writeBuffer;
    Greetings m_greetings;

    // Connection interface
public:
    HRESULT GetPlayerName(int id, char *data, DWORD *size) override;
};

HRESULT Client::GetPlayerName(int id, char *data, DWORD *size)
{

    return DP_OK;
}

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
    int n;
    unsigned int m = sizeof(n);
    if (getsockopt(m_socket, SOL_SOCKET, SO_RCVBUF,(void *)&n, &m) != 0 || n < 4096)
        n = 4 * 1024 * 1024;
    m_readBuffer.resize(n);
    int opt = 1;
    setsockopt(m_socket, SOL_TCP, TCP_NODELAY, &opt, sizeof(int));
    m_greetings.completedCallback = [this] (const std::string &name) {
        SessionData[0].AllowedToJoin = true;
        SessionData[0].Guid = m_socket;
        strncpy(&SessionData[0].Name[0], name.c_str(), 40);
        std::cout << "Found server " << name << std::endl;
        NumberOfSessionsFound = 1;
    };
}

Client::~Client()
{
    std::cout << __PRETTY_FUNCTION__ << " " << m_socket << " " << std::endl;
    RemovePlayerFromGame(m_socket);
    ::shutdown(m_socket, SHUT_RDWR);
    ::close(m_socket);
}

HRESULT Client::Receive()
{
    ssize_t size = ::read(m_socket, &m_readBuffer[0], m_readBuffer.size());
    if (size < 0) {
        if (errno != EAGAIN)
            return DPERR_CONNECTIONLOST;
    } else {
        if (size) {
            size_t pos = !m_greetings.completed ? m_greetings.Parse(m_readBuffer.data(), size) : 0;
            ProcessGameMessage(m_socket, m_readBuffer.data() + pos, size - pos);
        }
    }
    return DP_OK;
}

HRESULT Client::Send(const char *data, DWORD dataSize)
{
    if (m_writeBuffer.size()) {
        std::string tmp = std::move(m_writeBuffer);
        m_writeBuffer.clear();
        auto res = Send(tmp.data(), tmp.size());
        if (res != DP_OK)
            return res;
        if (m_writeBuffer.size())
            return DPERR_BUSY;
    }
    ssize_t written = ::write(m_socket, data, dataSize);
    if (written < 0) {
        if (errno != EAGAIN)
            return DPERR_CONNECTIONLOST;
        written = 0;
    }
    if (written != dataSize)
        m_writeBuffer = std::string(data + written, dataSize - written);
    return DP_OK;
}

void Client::SendGreetings(std::string name)
{
    name.insert(0, 1, name.size());
    Send(name.data(), name.size());
}

static std::unique_ptr<Connection> connection;

#ifdef __cplusplus
extern "C" {
#endif
DPID AVPDPNetID;
int QuickStartMultiplayer=1;
char AVPDPplayerName[NET_PLAYERNAMELENGTH + 3] = "";
int glpDP; /* directplay object */
//static int accept_sock = -1;
//static bool server_mode = false;
void InitializeNetwork()
{
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

HRESULT SessionReceiveMessages()
{
    if (connection)
        return connection->Receive();
    return DPERR_CONNECTIONLOST;
}

HRESULT SessionSendMessage(const char *lpData, DWORD dwDataSize)
{
    if (connection)
        return connection->Send(lpData, dwDataSize);
    return DPERR_CONNECTIONLOST;
}

/* directplay.c */
int DirectPlay_ConnectingToLobbiedGame(char* playerName)
{
    fprintf(stderr, "DirectPlay_ConnectingToLobbiedGame(%s)\n", playerName);

    return 0;
}

int NetConnectToSession(int sessionNumber, char *playerName)
{
    if (!connection)
        return 0;
    AVPDPNetID = 1;
    glpDP = 1;
    connection->SendGreetings(playerName);
    fprintf(stderr, "DirectPlay_ConnectToSession(%d, %s)\n", sessionNumber, playerName);
    InitAVPNetGameForJoin();
    netGameData.levelNumber = SessionData[sessionNumber].levelIndex;
    netGameData.joiningGameStatus = JOINNETGAME_WAITFORDESC;
    return 1;
}

int DirectPlay_ConnectingToSession()
{
    fprintf(stderr, "DirectPlay_ConnectingToSession()\n");
    MinimalNetCollectMessages();
    if(!netGameData.needGameDescription)
    {
        //we now have the game description , so we can go to the configuration menu
        return AVPMENU_MULTIPLAYER_CONFIG_JOIN;
    }
    return 1;
}

BOOL DirectPlay_UpdateSessionList(int */*SelectedItem*/)
{
    if (connection && !connection->Established())
        connection->Receive();
//    fprintf(stderr, "DirectPlay_UpdateSessionList(%p)\n", SelectedItem);
    return NumberOfSessionsFound;
}

int NetJoinGame()
{
    extern const char IPAddressString[];
    try {
        connection = std::make_unique<Client>(IPAddressString);
    } catch(...) {
    }
    fprintf(stderr, "DirectPlay_JoinGame(%s)\n", IPAddressString);
    return NumberOfSessionsFound = 0;
}

void DirectPlay_EnumConnections()
{
    fprintf(stderr, "DirectPlay_EnumConnections()\n");

    netGameData.tcpip_available = 1;
    netGameData.ipx_available = 0;
    netGameData.modem_available = 0;
    netGameData.serial_available = 0;
}

int DirectPlay_HostGame(char *playerName, char *sessionName,int species,int gamestyle,int level)
{
    try {
        connection = std::make_unique<Server>(sessionName);
    }  catch (...) { return 0; }
    extern int DetermineAvailableCharacterTypes(int);
    int maxPlayers=DetermineAvailableCharacterTypes(FALSE);
    if(maxPlayers<1) maxPlayers=1;
    if(maxPlayers>8) maxPlayers=8;

    AVPDPNetID = 1;
    strcpy(AVPDPplayerName, playerName);

    if(!netGameData.skirmishMode) {
        fprintf(stderr, "DirectPlay_HostGame(%s, %s, %d, %d, %d)\n", playerName, sessionName, species, gamestyle, level);

        //fake multiplayer
        //need to set the id to an non zero value

        glpDP = 1;
    }

    InitAVPNetGameForHost(species,gamestyle,level);
    return 1;
}

int NetDisconnectSession()
{
    connection.reset();
    glpDP = 0;
    fprintf(stderr, "DirectPlay_Disconnect()\n");

    return 1;
}

HRESULT NetGetPlayerName(int glpDP, DPID id, char *data, DWORD *size)
{
    if (connection)
        return connection->GetPlayerName(id, data, size);
    fprintf(stderr, "IDirectPlayX_GetPlayerName(%d, %d, %p, %p)\n", glpDP, id, data, size);

    return DPERR_INVALIDPLAYER;
}

#ifdef __cplusplus
};
#endif
/* End of Linux-related junk */

