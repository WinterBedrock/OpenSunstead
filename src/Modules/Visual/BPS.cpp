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
    if (!HudEditor::HudVisible()) return;

    static double lastSampleTime = 0.0;
    static double lastShowTime   = 0.0;
    static float  lastX = 0.0f, lastZ = 0.0f;
    static bool   havePrev = false;
    static float  smoothBps = 0.0f;
    static float  shownBps  = 0.0f;
    const double  kRefresh  = 0.25;

    float p[3];
    double now = ImGui::GetTime();
    if (getPlayerPosSafe(p)) {
        if (havePrev) {
            double dt = now - lastSampleTime;
            if (dt > 0.0005) {
                float dx = p[0] - lastX;
                float dz = p[2] - lastZ;
                float inst = (float)(std::sqrt(dx * dx + dz * dz) / dt);
                smoothBps += (inst - smoothBps) * 0.15f;
                lastSampleTime = now; lastX = p[0]; lastZ = p[2];
            }
        } else {
            havePrev = true; lastSampleTime = now; lastShowTime = now;
            lastX = p[0]; lastZ = p[2];
        }
    }
    if (now - lastShowTime >= kRefresh) {
        shownBps = smoothBps;
        lastShowTime = now;
    }

    char buffer[48];
    sprintf_s(buffer, "%.1f", shownBps);
    DrawCounterModule(this, buffer, "BPS");
}
