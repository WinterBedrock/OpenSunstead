#include "BPS.h"
#include "../../Render/HudEditor.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Utils/MCSDK.h"
#include <imgui.h>
#include <string>
#include <cmath>
#include <cstdio>

// Reads the local player position safely (same known-good path as Coordinates).
static bool getPlayerPosSafe(float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    __try {
        SDK::ClientInstance* cInst = SDK::ClientInstance::get();
        if (!cInst) return false;
        SDK::Actor* lp = cInst->getLocalPlayer();
        if (!lp || !sdk_safe_ptr((void*)lp, sizeof(void*))) return false;
        Vec3 pos = lp->getPos();
        out[0] = pos.x; out[1] = pos.y; out[2] = pos.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void BPS::OnRender() {
    if (!IsEnabled()) return;
    // Also visible while the HUD editor is open (it runs on the pause/chat screens).
    if (!HudEditor::HudVisible()) return;

    // --- speed computation (horizontal blocks per second) ---
    // The displayed number is refreshed on a fixed cadence so it stays
    // readable instead of flickering every frame.
    static double lastSampleTime = 0.0;   // last time we measured a delta
    static double lastShowTime   = 0.0;   // last time we updated the readout
    static float  lastX = 0.0f, lastZ = 0.0f;
    static bool   havePrev = false;
    static float  smoothBps = 0.0f;       // continuously smoothed speed
    static float  shownBps  = 0.0f;       // value actually drawn
    const double  kRefresh  = 0.25;       // seconds between readout updates

    float p[3];
    double now = ImGui::GetTime();
    if (getPlayerPosSafe(p)) {
        if (havePrev) {
            double dt = now - lastSampleTime;
            if (dt > 0.0005) {
                float dx = p[0] - lastX;
                float dz = p[2] - lastZ;
                float inst = (float)(std::sqrt(dx * dx + dz * dz) / dt);
                smoothBps += (inst - smoothBps) * 0.15f;   // heavier smoothing
                lastSampleTime = now; lastX = p[0]; lastZ = p[2];
            }
        } else {
            havePrev = true; lastSampleTime = now; lastShowTime = now;
            lastX = p[0]; lastZ = p[2];
        }
    }
    // Only commit a new visible value every kRefresh seconds.
    if (now - lastShowTime >= kRefresh) {
        shownBps = smoothBps;
        lastShowTime = now;
    }

    char buffer[48];
    sprintf_s(buffer, "%.1f", shownBps);
    DrawCounterModule(this, buffer, "BPS");
}