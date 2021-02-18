#ifndef AVP_NET_H
#define AVP_NET_H

#ifdef __cplusplus
extern "C" {
#endif
void init_net();
void shutdown_net();

BOOL DpExtInit(DWORD cGrntdBufs, DWORD cBytesPerBuf, BOOL bErrChcks);
HRESULT DpExtRecv(int lpDP2A, void *lpidFrom, void *lpidTo, DWORD dwFlags, void *lplpData, LPDWORD lpdwDataSize);
HRESULT DpExtSend(int lpDP2A, DPID idFrom, DPID idTo, DWORD dwFlags, void *lpData, DWORD dwDataSize);
void DpExtUnInit();
int DirectPlay_Disconnect();
HRESULT IDirectPlayX_GetPlayerName(int glpDP, DPID id, void *data, void *size);
int DirectPlay_ConnectingToSession();
int DirectPlay_ConnectingToLobbiedGame(char* playerName);
void DirectPlay_EnumConnections();

#ifdef __cplusplus
};
#endif

#endif
