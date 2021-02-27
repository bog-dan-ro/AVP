#include "fixer.h"
#include "bink.h"

BOOL BinkSys_Init()
{
    return TRUE;
}
void BinkSys_Release()
{
}

//--- intro/outro
void PlayBinkedFMV(char *filenamePtr, int volume)
{
}

//--- menu background
void StartMenuBackgroundBink()
{}
int PlayMenuBackgroundBink()
{
    return FALSE;
}
void EndMenuBackgroundBink()
{}

//---- music
int StartMusicBink(char* filenamePtr, BOOL looping)
{
    return FALSE;
}
int PlayMusicBink(int volume)
{
    return FALSE;
}
void EndMusicBink()
{}
