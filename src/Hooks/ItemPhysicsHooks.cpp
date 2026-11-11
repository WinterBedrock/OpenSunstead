#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../Modules/Visual/ItemPhysics.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "HookEngine.h"
#include <cmath>

namespace {
    typedef void*(__fastcall* matRotate_t)(void* out, void* in, float angle, float* axis);
    static matRotate_t oMatRotate = nullptr;

    static void* __fastcall hkMatRotate(void* out, void* in, float angle, float* axis) {
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (oMatRotate) {
            if (ItemPhysics::LayFlatEnabled() && ret == ItemPhysics::SpinCallReturnAddr()) {
                // 90° about X tips the item onto the ground plane; a local
                // axis buffer instead of writing through the caller's temporary.
                float flatAxis[3] = { 1.0f, 0.0f, 0.0f };
                constexpr float kHalfPi = 1.57079633f;
                return oMatRotate(out, in, kHalfPi, flatAxis);
            }
            // Swing Custom Angle used to ride here via return-address matching;
            // it now lives in Swing.cpp as a direct displacement patch and needs
            // nothing from this shared detour.
            return oMatRotate(out, in, angle, axis);
        }
        return out;
    }

    typedef void(__fastcall* renderStacked_t)(void* self, void* a2, void* actor,
                                               unsigned copies, float scale,
                                               float partial, char a7);
    static renderStacked_t oRenderStacked = nullptr;

    static constexpr uintptr_t kStackOffsetTable = 0x178;
    static constexpr size_t    kStackMaxCopies   = 32;

    static bool saveAndZeroStackOffsets(void* self, unsigned copies, float* saved) {
        if (!self || copies == 0 || copies > kStackMaxCopies) return false;
        __try {
            auto* t = (float*)((uintptr_t)self + kStackOffsetTable);
            const size_t n = (size_t)copies * 3;
            for (size_t i = 0; i < n; ++i) { saved[i] = t[i]; t[i] = 0.0f; }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void restoreStackOffsets(void* self, unsigned copies, const float* saved) {
        __try {
            auto* t = (float*)((uintptr_t)self + kStackOffsetTable);
            const size_t n = (size_t)copies * 3;
            for (size_t i = 0; i < n; ++i) t[i] = saved[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void __fastcall hkRenderStackedCopies(void* self, void* a2, void* actor,
                                                  unsigned copies, float scale,
                                                  float partial, char a7) {
        if (!oRenderStacked) return;

        float saved[kStackMaxCopies * 3];
        const bool flattened = ItemPhysics::LayFlatEnabled() &&
                               saveAndZeroStackOffsets(self, copies, saved);

        oRenderStacked(self, a2, actor, copies, scale, partial, a7);

        if (flattened) restoreStackOffsets(self, copies, saved);
    }
}

void InstallItemPhysicsHooks() {
    {
        uintptr_t rAddr = SigManager::GetInstance().Matrix_rotateAxisAngle;
        if (rAddr && HookEngine::Create(reinterpret_cast<LPVOID>(rAddr), &hkMatRotate,
                                   reinterpret_cast<LPVOID*>(&oMatRotate))) {
            LOG_INFO("Matrix::rotateAxisAngle hooked (ItemPhysics LayFlat)", "GameHooks");
        }
    }
    {
        uintptr_t sAddr = SigManager::GetInstance().ItemActor_renderStackedCopies;
        if (sAddr && HookEngine::Create(reinterpret_cast<LPVOID>(sAddr), &hkRenderStackedCopies,
                                   reinterpret_cast<LPVOID*>(&oRenderStacked))) {
            LOG_INFO("ItemActorRender::renderStackedCopies hooked (ItemPhysics)", "GameHooks");
        }
    }
}
