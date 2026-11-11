#pragma once
#include <Windows.h>
#include <atomic>
#include <string>

namespace SDK { class Packet; }

namespace GameHooks {
    void Initialize();
    void Shutdown();
    bool HooksReady();
    int LastPerspective();
    int LivePerspectiveForGhost();
    void SendChatMessage(const std::string& text);

    bool HoldIncomingPacket(SDK::Packet* raw);
    // ref vt1
    bool ResendIncomingPacket(SDK::Packet* raw, int realId);

    void CaptureFrameViewNow();
    void BeginPresentFrame(unsigned bbNow);

    extern std::atomic<bool> g_freeCamGhostDrawn;
    void ResetFreeCamGhostFrame();
}

namespace NetPing {
    int  LastMs();
    unsigned long AgeMs();
    void ForcePoll();
}
