#ifndef _BINK_H_
#define _BINK_H_

#include "fixer.h"

#ifdef __cplusplus
extern "C" {
#endif

extern BOOL BinkSys_Init();
extern void BinkSys_Release();

//--- intro/outro
extern void PlayBinkedFMV(char *filenamePtr, int volume);

//--- menu background
extern void StartMenuBackgroundBink();
extern int 	PlayMenuBackgroundBink();
extern void EndMenuBackgroundBink();

//---- music
extern int StartMusicBink(char* filenamePtr, BOOL looping);
extern int PlayMusicBink(int volume);
extern void EndMusicBink();

#ifdef __cplusplus
} // extern "C" {
#endif

#endif //_BINK_H_
