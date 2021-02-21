#ifndef AVP_NET_H
#define AVP_NET_H

#ifdef __cplusplus
extern "C" {
#endif
void InitializeNetwork();
void ShutdownNetwork();

HRESULT NetSessionReceiveMessages();
HRESULT NetSessionSendMessage(const char *lpData, DWORD dwDataSize);
BOOL NetSessionInit(DWORD cGrntdBufs, DWORD cBytesPerBuf, BOOL bErrChcks);
void NetSessionUnInit();
int NetDisconnectSession();
HRESULT NetGetPlayerName(int glpDP, DPID id, char *data, DWORD *size);
int NetConnectToSession(int sessionNumber, char *playerName);
int NetConnectingToSession();
int NetConnectingToLobbiedGame(char* playerName);
void NetEnumConnections();

#ifdef __cplusplus
};
#endif

#endif
