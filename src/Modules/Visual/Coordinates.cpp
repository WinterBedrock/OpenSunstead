#include "Coordinates.h"
#include "../../Render/HudEditor.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Utils/MCSDK.h"
#include <string>
#include <cstdio>

// Latite-style coordinate fetch: read the local player's position directly via
// ClientInstance::getLocalPlayer() (vtable[0x1F], verified) -> Actor::getPos()
// (StateVectorComponent, fetched through entt with SEH guards in Actor.h).
//
// The previous code tried ClientInstance::getLevelRendererPlayer() via vtable
// index 187, which is version-specific and dereferenced a garbage pointer on
// world entry → crash. getLocalPlayer()->getPos() is the same path Latite's
// CustomCoordinates module uses, and it is already known-good (the WndProc
// handler calls getLocalPlayer() every keypress without crashing).
//
// Returns true and fills `out` on success.
static bool getCoordinatesSafe(float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;

    __try {
        SDK::ClientInstance* cInst = SDK::ClientInstance::get();
        if (!cInst) return false;

        SDK::Actor* lp = cInst->getLocalPlayer();
        if (!lp || !sdk_safe_ptr((void*)lp, sizeof(void*))) return false;

        Vec3 pos = lp->getPos();
        out[0] = pos.x;
        out[1] = pos.y - 1.62f; // eye offset, same as Latite CustomCoordinates
        out[2] = pos.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Coordinates::OnRender() {
    if (!IsEnabled()) return;
    // Also visible while the HUD editor is open (it runs on the pause/chat screens).
    if (!HudEditor::HudVisible()) return;

    // Decimals setting: 0 -> "13" (rounded), 2 -> "12.80", 3 -> "12.803".
    const int decimals = std::clamp((int)static_cast<NumberSetting*>(mSettings[8])->mValue, 0, 3);

    std::string value = "0 0 0";
    {
        char fmt[16];
        sprintf_s(fmt, "%%.%df", decimals);
        float coord[3] = {0, 0, 0};
        if (getCoordinatesSafe(coord)) {
            char buffer[80];
            sprintf_s(buffer, fmt, coord[0]);
            std::string s = buffer;
            for (int i = 1; i < 3; ++i) {
                sprintf_s(buffer, fmt, coord[i]);
                s += " " + std::string(buffer);
            }
            value = s;
        }
    }

    DrawCounterModule(this, value, "XYZ");
}
