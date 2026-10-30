#include "AirJump.h"
#include "../../Memory/SigManager.h"
#include "../../SDK/SDK.h"
#include "../../Utils/MCSDK.h"
#include <Windows.h>
#include <cstdint>

// im lazy so i just used hash
static constexpr uint32_t kOnGroundHash = 0xC29078A0u;

using EmplaceFlag_t = void(__fastcall*)(void*, void*, void*, int, int);

static bool sehForceOnGround(SDK::Actor* lp) {
    if (!lp) return false;
    const uintptr_t emplace = SigManager::GetInstance().Entt_emplaceFlag;
    if (!emplace || !g_entt_assure) return false;
    __try {
        auto* ctx = (SDK::EntityContext*)((uintptr_t)lp + 0x8);
        if (!ctx || !sdk_safe_ptr(ctx, sizeof(SDK::EntityContext))) return false;
        void* reg = *(void**)((uintptr_t)ctx + 8);
        if (!reg || !sdk_safe_ptr(reg, sizeof(void*))) return false;
        if (sdk_hasComponentRaw(ctx, g_entt_assure, kOnGroundHash) == 1) return true;
        void* sp = entt_assure_seh(g_entt_assure, reg, kOnGroundHash);
        if (!sp || !sdk_safe_ptr(sp, 0x18)) return false;
        if (!sdk_safe_ptr((void*)emplace, 16)) return false;
        uint32_t scratch[5] = {};
        uint32_t ent = (uint32_t)ctx->entity;
        scratch[0] = ent;
        ((EmplaceFlag_t)emplace)(sp, scratch, &ent, 0, 0);
        return sdk_hasComponentRaw(ctx, g_entt_assure, kOnGroundHash) == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void AirJump::OnTick() {
    if (!IsEnabled()) return;
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;
    ForceOnGround(lp);
}

bool AirJump::ForceOnGround(SDK::Actor* lp) {
    return sehForceOnGround(lp);
}
