#include "Ping.h"
#include "../../Render/HudEditor.h"
#include "../../Hooks/GameHooks.h"
#include <string>

void Ping::OnTick() {
    if (!IsEnabled()) return;
    // The game polls GetAveragePing seconds apart at best, so refresh the
    // captured remote on a display cadence.
    static ULONGLONG s_lastPoll = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastPoll >= 500) {
        s_lastPoll = now;
        NetPing::ForcePoll();
    }
}

void Ping::OnRender() {
    if (!IsEnabled()) return;
    // Also visible while the HUD editor is open (it runs on the pause/chat screens).
    if (!HudEditor::HudVisible()) return;

    const int ping = NetPing::LastMs();
    const std::string value = (ping >= 0) ? std::to_string(ping) : "-";
    DrawCounterModule(this, value, "ms");
}
