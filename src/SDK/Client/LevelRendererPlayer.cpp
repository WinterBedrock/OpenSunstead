#include "LevelRendererPlayer.h"
#include "ClientInstance.h"
#include <cmath>

namespace SDK {

// Plain POD walk (no C++ objects) so the __except filter actually fires.
static LevelRendererPlayer* sehWalk() {
    __try {
        ClientInstance* ci = ClientInstance::get();
        if (!ci || !sdk_safe_ptr(ci, Offsets().CI_LevelRenderer + 8)) return nullptr;
        LevelRenderer* lr = *(LevelRenderer**)((uintptr_t)ci + Offsets().CI_LevelRenderer);
        if (!lr || !sdk_safe_ptr(lr, Offsets().LevelRenderer_mRendererPlayer + 8)) return nullptr;
        LevelRendererPlayer* lrp = *(LevelRendererPlayer**)((uintptr_t)lr + Offsets().LevelRenderer_mRendererPlayer);
        if (!lrp || !sdk_safe_ptr(lrp, Offsets().LevelRendererPlayer_mProjMatrix + 0x40)) return nullptr;
        return lrp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

LevelRendererPlayer* sehGetLevelRendererPlayer() {
    return sehWalk();
}

bool sehGetFovFieldPtrs(float** fovXOut, float** fovYOut) {
    if (!fovXOut || !fovYOut) return false;
    __try {
        LevelRendererPlayer* lrp = sehWalk();
        if (!lrp) return false;
        // Probe-read both slots before handing the pointers out.
        volatile float probe = *(float*)((uintptr_t)lrp + Offsets().LevelRendererPlayer_mProjMatrix);
        (void)probe;
        probe = *(float*)((uintptr_t)lrp + (Offsets().LevelRendererPlayer_mProjMatrix + 5 * sizeof(float)));
        (void)probe;
        *fovXOut = (float*)((uintptr_t)lrp + Offsets().LevelRendererPlayer_mProjMatrix);
        *fovYOut = (float*)((uintptr_t)lrp + (Offsets().LevelRendererPlayer_mProjMatrix + 5 * sizeof(float)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool sehReadFovValues(float* fovXOut, float* fovYOut) {
    if (!fovXOut || !fovYOut) return false;
    __try {
        LevelRendererPlayer* lrp = sehWalk();
        if (!lrp) return false;
        const float x = *(float*)((uintptr_t)lrp + Offsets().LevelRendererPlayer_mProjMatrix);
        const float y = *(float*)((uintptr_t)lrp + (Offsets().LevelRendererPlayer_mProjMatrix + 5 * sizeof(float)));
        if (!std::isfinite(x) || !std::isfinite(y) || x == 0.0f || y == 0.0f) return false;
        *fovXOut = x;
        *fovYOut = y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// sehApplyFovMod — see the header for the rationale. Single writer (render
// thread), renormalized, POD-only inside the __try blocks.
namespace {
float  s_appliedTotal = 1.0f;   // scale currently baked into the fields
float  s_lastWroteX   = 0.0f;   // exact value we stored (bit-compared)
float  s_lastWroteY   = 0.0f;
bool   s_haveWritten  = false;
}

float sehGetAppliedFovMod() {
    return s_appliedTotal;
}

bool sehApplyFovMod(float total) {
    if (!std::isfinite(total) || total < 0.01f || total > 100.0f) total = 1.0f;
    __try {
        float* fx = nullptr;
        float* fy = nullptr;
        if (!sehGetFovFieldPtrs(&fx, &fy)) return false;
        float x = *fx;
        float y = *fy;
        if (!std::isfinite(x) || !std::isfinite(y)) return false;

        // Did OUR last write survive untouched? Bit-exact compare: the value
        // we stored is still there only if nothing (render-prep, another
        // module) rewrote the slot since.
        const bool untouched = s_haveWritten && x == s_lastWroteX && y == s_lastWroteY;
        if (untouched && s_appliedTotal != 1.0f) {
            x /= s_appliedTotal;
            y /= s_appliedTotal;
        }
        // x/y now hold the game's own base (fresh render-prep value or the
        // un-scaled survivor). Apply the new total.
        if (total != 1.0f) {
            x *= total;
            y *= total;
        }
        if (x == 0.0f || y == 0.0f) return false;
        *fx = x;
        *fy = y;
        s_lastWroteX = x;
        s_lastWroteY = y;
        s_haveWritten = true;
        s_appliedTotal = total;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool sehReadLrpMatrices(float viewOut[16], float projOut[16]) {
    if (!viewOut || !projOut) return false;
    __try {
        LevelRendererPlayer* lrp = sehWalk();
        if (!lrp) return false;
        const float* v = (const float*)((uintptr_t)lrp + Offsets().LevelRendererPlayer_mViewMatrix);
        const float* p = (const float*)((uintptr_t)lrp + Offsets().LevelRendererPlayer_mProjMatrix);
        for (int i = 0; i < 16; ++i) {
            viewOut[i] = v[i];
            projOut[i] = p[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(viewOut[i]) || !std::isfinite(projOut[i])) return false;
    return true;
}

} // namespace SDK
