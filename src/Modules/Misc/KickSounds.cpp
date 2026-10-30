#include "KickSounds.h"
#include "../Setting.h"
#include "../../resource.h"
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

extern HMODULE g_hSelfModule;

// Disconnect / PlayerDisconnect packet id on 1.26.30.05.
static constexpr int kDisconnectPacketId = 0x5;

void KickSounds::OnPacketReceive(SDK::Packet* /*packet*/, int packetId, bool& /*cancel*/) {
    if (packetId != kDisconnectPacketId) return;

    int resId;
    switch (GetSoundIndex()) {
        case 1:  resId = IDR_SOUND_MARIO;    break;
        case 2:  resId = IDR_SOUND_WINERROR; break;
        case 3:  resId = IDR_SOUND_67;       break;
        case 0:
        default: resId = IDR_SOUND_FARD;     break;
    }

    HMODULE mod = g_hSelfModule ? g_hSelfModule : GetModuleHandleW(nullptr);
    PlaySoundW(MAKEINTRESOURCEW(resId), mod, SND_RESOURCE | SND_ASYNC | SND_NODEFAULT);
}
