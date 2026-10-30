// Dropped-item physics hooks (ItemPhysics): stop bobbing, lay items flat and
// keep stacked copies on the ground. The rotate-about-axis helper is shared
// with the first-person swing arc (Swing), disambiguated by return address.
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../Modules/Visual/ItemPhysics.h"
#include "../Modules/Visual/Swing.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "MinHook.h"
#include <cmath>

namespace {
    // ItemActorRender::getBobHeight. Vanilla (1.26.60 IDA):
    //   base   = 0.2f when `actor && (actor[0x131] & 1) == 0`, else 0.0f
    //   result = base + (sin term * 0.1f + 0.1f)
    // The sine swings -0.1..+0.1 around the trailing +0.1, so base + 0.1 is
    // the midpoint of the bob arc — the item rests lying there, and the
    // module's Height offset then grounds a flat-laid stack copy.
    typedef float(__fastcall* getBobHeight_t)(void* self, void* actor, float partialTick);
    static getBobHeight_t oGetBobHeight = nullptr;

    // POD-only for the __try.
    static bool itemIsInsideBlock(void* actor) {
        if (!actor) return true;   // matches the original's `actor &&` short-circuit
        __try {
            return (*reinterpret_cast<unsigned char*>(
                        reinterpret_cast<uintptr_t>(actor) + 0x131) & 1) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    }

    static float __fastcall hkGetBobHeight(void* self, void* actor, float partialTick) {
        if (ItemPhysics::NoBobEnabled()) {
            const float base = itemIsInsideBlock(actor) ? 0.0f : 0.2f;
            return base + 0.1f - ItemPhysics::HeightOffset();
        }
        return oGetBobHeight
            ? oGetBobHeight(self, actor, partialTick)
            : 0.0f;
    }

    // Matrix::rotateAxisAngle — called for everything, so the item-spin and
    // swing calls are identified by RETURN ADDRESS: the spin's upright Y
    // rotation is SUBSTITUTED (not composed) to tip the item over; the swing
    // arc only has its angle scaled.
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
            const float scale = Swing::SwingAngleScale();
            if (scale != 1.0f && ret == Swing::SwingRotateReturn())
                return oMatRotate(out, in, angle * scale, axis);
        }
        return oMatRotate
            ? oMatRotate(out, in, angle, axis)
            : out;
    }

    // renderStackedCopies: copy `i` is offset by the float triple at
    // renderer + 0x178 + 0xC*i — zero the table for the duration of the call
    // (it lives on the renderer, not a temporary, so restore right after).
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
        uintptr_t bAddr = SigManager::GetInstance().ItemActor_getBobHeight;
        if (bAddr && MH_CreateHook(reinterpret_cast<LPVOID>(bAddr), &hkGetBobHeight,
                                   reinterpret_cast<LPVOID*>(&oGetBobHeight)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(bAddr));
            LOG_INFO("ItemActorRender::getBobHeight hooked (ItemPhysics)", "GameHooks");
        }
    }
    {
        uintptr_t rAddr = SigManager::GetInstance().Matrix_rotateAxisAngle;
        if (rAddr && MH_CreateHook(reinterpret_cast<LPVOID>(rAddr), &hkMatRotate,
                                   reinterpret_cast<LPVOID*>(&oMatRotate)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(rAddr));
            LOG_INFO("Matrix::rotateAxisAngle hooked (ItemPhysics LayFlat)", "GameHooks");
        }
    }
    {
        uintptr_t sAddr = SigManager::GetInstance().ItemActor_renderStackedCopies;
        if (sAddr && MH_CreateHook(reinterpret_cast<LPVOID>(sAddr), &hkRenderStackedCopies,
                                   reinterpret_cast<LPVOID*>(&oRenderStacked)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(sAddr));
            LOG_INFO("ItemActorRender::renderStackedCopies hooked (ItemPhysics)", "GameHooks");
        }
    }
}
