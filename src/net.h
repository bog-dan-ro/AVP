#ifndef AVP_NET_H
#define AVP_NET_H

#ifdef __cplusplus
extern "C" {
#endif
void InitializeNetwork();
void ShutdownNetwork();

HRESULT SessionReceiveMessages();
HRESULT SessionSendMessage(const char *lpData, DWORD dwDataSize);
BOOL NetSessionInit(DWORD cGrntdBufs, DWORD cBytesPerBuf, BOOL bErrChcks);
void NetSessionUnInit();
int NetDisconnectSession();
HRESULT NetGetPlayerName(int glpDP, DPID id, char *data, DWORD *size);
int NetConnectToSession(int sessionNumber, char *playerName);
int DirectPlay_ConnectingToSession();
int DirectPlay_ConnectingToLobbiedGame(char* playerName);
void DirectPlay_EnumConnections();

#ifdef __cplusplus
};
#endif

#endif
