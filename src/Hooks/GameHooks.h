#pragma once
#include <Windows.h>
#include <atomic>
#include <string>

namespace SDK { class Packet; }

namespace GameHooks {
    void Initialize();
    void Shutdown();
    // True once Initialize installed every detour AND the signature scan is
    // complete. The Present hook (installed FIRST, before any of that exists)
    // consults this before it lets the overlay initialize or draw, so client
    // code can never run against half-populated modules or settings.
    bool HooksReady();
    // Last perspective the game asked for (0=first,1=third-back,2=third-front).
    int LastPerspective();
    // Live perspective through the REAL getter (Options object remembered by
    // the getPerspective hook) — used by the FreeCam ghost injection so a
    // stale cached value can never gate it.
    int LivePerspectiveForGhost();
    void SendChatMessage(const std::string& text);

    // ---- Deferred incoming-packet re-delivery --------------------------------
    // A module may HOLD the packet currently being dispatched (call during
    // OnPacketReceive, then set cancel), rewrite its payload asynchronously
    // and later RESEND it through the original handler. The resend queue is
    // drained at the top of the receive hook, so delivery runs on the same
    // network-thread context as a normal packet (Translate uses this).
    // raw must be the packet pointer passed to OnPacketReceive; returns false
    // when no hold could be taken — the caller must NOT cancel the packet then.
    bool HoldIncomingPacket(SDK::Packet* raw);
    // realId must be the packet's own getId() (vtable[1]) so its dispatch
    // context can be found. Returns false when no hold/context exists for it.
    bool ResendIncomingPacket(SDK::Packet* raw, int realId);

    // Snapshot the current-frame camera transform into g_frameView. Called
    // from the DX Present hook: the game writes lastViewMatrix only AFTER the
    // world-render returns, so a renderLevel-time read steals the PREVIOUS
    // frame's matrix and overlays lag/swim on rotation.
    void CaptureFrameViewNow();
    // Called at the very start of every DX Present: advances the frame counter
    // and drains the camera queue, serving the transform that rendered the
    // backbuffer about to be presented.
    void BeginPresentFrame(unsigned bbNow);

    // ---- FreeCam ghost body (see Modules/Player/FreeCam.h) -------------------
    // Armed by the Chams ActorRenderDispatcher detour once per frame so the
    // model draws exactly once; cleared at the top of every world-render.
    extern std::atomic<bool> g_freeCamGhostDrawn;
    void ResetFreeCamGhostFrame();
}

// Live RTT to the current server, captured by the RakPeer::GetAveragePing
// detour (RakNet averages the last five ping/pong samples, ms). NetPing itself
// re-polls the captured (peer, remote) pair at display cadence.
namespace NetPing {
    // Latest average ping in ms, -1 when no remote answered yet.
    int  LastMs();
    // Milliseconds since LastMs was last refreshed.
    unsigned long AgeMs();
    // Re-poll the captured remote now (SEH-guarded no-op without a capture).
    void ForcePoll();
}
