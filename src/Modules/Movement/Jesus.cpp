#include "Jesus.h"
#include "../../Utils/MCSDK.h"
#include "../../Memory/SigManager.h"
#include "MinHook.h"
#include <cmath>

// 26.30 WaterBlockLegacy::getCollisionShapeForCamera @ sub_14D957E90:
//   copies BlockLegacy visual AABB into out (24 bytes / 6 floats). Water boxes
//   are empty → no collision. Detour returns a solid unit cube while Jesus is on.

bool Jesus::s_solid = false;

bool Jesus::ShouldSolid() { return s_solid; }

using WaterColFn = void* (__fastcall*)(void* self, void* aabbOut);
static WaterColFn oWaterCol = nullptr;

static void* __fastcall hkWaterCol(void* self, void* aabbOut) {
    if (Jesus::ShouldSolid() && aabbOut) {
        float* f = reinterpret_cast<float*>(aabbOut);
        f[0] = 0.0f; f[1] = 0.0f; f[2] = 0.0f;
        f[3] = 1.0f; f[4] = 1.0f; f[5] = 1.0f;
        return aabbOut;
    }
    return oWaterCol ? oWaterCol(self, aabbOut) : aabbOut;
}

bool Jesus::installHook() {
    if (mHooked) return true;
    uintptr_t fn = SigManager::GetInstance().WaterBlockLegacy_getCollisionShape;
    if (!fn) return false;
    if (MH_CreateHook(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(&hkWaterCol),
                      reinterpret_cast<LPVOID*>(&oWaterCol)) != MH_OK)
        return false;
    if (MH_EnableHook(reinterpret_cast<LPVOID>(fn)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<LPVOID>(fn));
        oWaterCol = nullptr;
        return false;
    }
    mHooked = true;
    return true;
}

void Jesus::removeHook() {
    if (!mHooked) return;
    uintptr_t fn = SigManager::GetInstance().WaterBlockLegacy_getCollisionShape;
    if (fn) {
        MH_DisableHook(reinterpret_cast<LPVOID>(fn));
        MH_RemoveHook(reinterpret_cast<LPVOID>(fn));
    }
    oWaterCol = nullptr;
    mHooked = false;
}

void Jesus::OnEnable() {
    s_solid = true;
    installHook();
}

void Jesus::OnDisable() {
    s_solid = false;
    removeHook();
}

void Jesus::OnTick() {
    if (!IsEnabled()) return;
    if (!mHooked) installHook();
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
