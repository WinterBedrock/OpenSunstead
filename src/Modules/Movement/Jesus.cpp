#include "Jesus.h"
#include "../../Utils/MCSDK.h"

#include <cmath>


bool Jesus::s_solid = false;

bool Jesus::ShouldSolid() { return s_solid; }

void Jesus::OnEnable() {
    s_solid = true;

}

void Jesus::OnDisable() {
    s_solid = false;

}

void Jesus::OnTick() {
    if (!IsEnabled()) return;

    // SHIFT is text input while the chat (or any other screen) is open —
    // never read it there.
    if (!SDK::isGameplayInputActive()) return;

    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool allowShift = GetAllowShift();

    // Hold shift → sink (disable solid water collision). No velocity hacks.
    if (shift && allowShift) {
        s_solid = false;
        return;
    }
    s_solid = true;

    // Soft surface stick: only cancel *downward* sink when feet are in water.
    // Never inject positive Y — that was launching the player.
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;
    auto* sv = lp->getStateVector();
    if (!sv) return;
    SDK::BlockSource* bs = ci->getBlockSource();
    if (!bs) return;

    Vec3 pos = lp->getPos();
    const float PLAYER_HEIGHT = 1.62f;
    int bx = (int)std::floor(pos.x);
    int by = (int)std::floor(pos.y - PLAYER_HEIGHT);
    int bz = (int)std::floor(pos.z);

    auto isWater = [&](int x, int y, int z) -> bool {
        SDK::Block* b = bs->getBlock(SDK::BlockPos(x, y, z));
        if (!b) return false;
        SDK::BlockLegacy* l = b->getLegacy();
        if (!l) return false;
        std::string n = l->getName();
        if (n.find("water") != std::string::npos) return true;
        uint16_t id = l->getBlockId();
        return id == 8 || id == 9;
    };

    if (isWater(bx, by, bz) || isWater(bx, by - 1, bz)) {
        if (sv->velocity.y < 0.0f)
            sv->velocity.y = 0.0f;
    }
}
