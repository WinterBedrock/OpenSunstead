#include "AnyOffhand.h"
#include "../../Memory/SigManager.h"
#include "../../Core/Logger.h"
#include "../../Hooks/HookEngine.h"

#include <cstdint>

bool AnyOffhand::s_hooksInstalled = false;

namespace {
bool g_anyOffhandEnabled = false;

using tCheckA = bool(__fastcall*)(__int64 a1, __int64 a2, __int64 a3);
using tCheckB = bool(__fastcall*)(__int64 a1, __int64 a2, __int64 a3, __int64 a4);
tCheckA oA = nullptr;
tCheckB oB = nullptr;

bool __fastcall hkA(__int64 a1, __int64 a2, __int64 a3) {
    if (g_anyOffhandEnabled) return 1;
    return oA ? oA(a1, a2, a3) : 1;
}

bool __fastcall hkB(__int64 a1, __int64 a2, __int64 a3, __int64 a4) {
    if (g_anyOffhandEnabled) return 1;
    return oB ? oB(a1, a2, a3, a4) : 1;
}
} // namespace

AnyOffhand::AnyOffhand()
    : Module("AnyOffhand", "Allow any item in the offhand slot", Category::Player, 0) {
    canBind = true;
}

bool AnyOffhand::IsActive() {
    return g_anyOffhandEnabled;
}

void AnyOffhand::OnEnable()  { g_anyOffhandEnabled = true; }
void AnyOffhand::OnDisable() { g_anyOffhandEnabled = false; }

void AnyOffhand::InstallHooks() {
    if (s_hooksInstalled) return;
    auto& sig = SigManager::GetInstance();

    // Direct prologues only — no reloc/offset fields in these patterns.
    if (sig.Item_allowOffhandCheckA) {
        HookEngine::Create(reinterpret_cast<LPVOID>(sig.Item_allowOffhandCheckA),
                &hkA, reinterpret_cast<LPVOID*>(&oA));
    }

    if (sig.Item_allowOffhandCheckB) {
        HookEngine::Create(reinterpret_cast<LPVOID>(sig.Item_allowOffhandCheckB),
                &hkB, reinterpret_cast<LPVOID*>(&oB));
    }

    s_hooksInstalled = true;
}
