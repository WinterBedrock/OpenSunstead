#include "ViewModel.h"
#include "../../Core/Logger.h"
#include "../../Memory/SigManager.h"
#include "../../Utils/MCSDK.h"
#include "MinHook.h"
#include <atomic>
#include <cmath>
#include <cstring>

std::atomic<bool> ViewModel::g_active{ false };
ViewModel::Params ViewModel::g_params;

using RenderFn = void(__fastcall*)(void* a1, void* a2, void* a3, uint32_t a4);
static RenderFn oRender = nullptr;

// pod
static bool sehReadPtr(uintptr_t addr, uintptr_t* out) {
    __try {
        *out = *reinterpret_cast<uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool sehReadQ(uintptr_t addr, uint64_t* out) {
    __try {
        *out = *reinterpret_cast<uint64_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// world MatrixStack top matrix
static float* sehResolveWorldMatrixTop(void* ctx) {
    uintptr_t p = 0, holder = 0, buffer = 0;
    uint64_t cap = 0, head = 0, size = 0;
    if (!sehReadPtr(reinterpret_cast<uintptr_t>(ctx) + 0x28, &p) || !p) return nullptr;
    if (!sdk_safe_ptr((void*)p, 0x20)) return nullptr;
    if (!sehReadPtr(p + 0x18, &holder) || !holder) return nullptr;
    if (!sdk_safe_ptr((void*)holder, 0x68)) return nullptr;
    if (!sehReadPtr(holder + 0x48, &buffer) || !buffer) return nullptr;
    if (!sehReadQ(holder + 0x50, &cap)) return nullptr;
    if (!sehReadQ(holder + 0x58, &head)) return nullptr;
    if (!sehReadQ(holder + 0x60, &size)) return nullptr;
    if (cap == 0 || (cap & (cap - 1)) != 0) return nullptr;         // power of two
    if (size == 0 || size > cap || head >= cap) return nullptr;
    const uint64_t idx = (head + size - 1) & (cap - 1);
    uintptr_t entryPtr = 0;
    if (!sehReadPtr(buffer + 8 * idx, &entryPtr) || !entryPtr) return nullptr;
    if (!sdk_safe_ptr((void*)entryPtr, 64)) return nullptr;
    float* m = reinterpret_cast<float*>(entryPtr);
    if (!std::isfinite(m[0]) || !std::isfinite(m[12]) || std::abs(m[15] - 1.0f) > 0.01f) return nullptr;
    return m;
}

// Post-multiply translate, ref:
//   m[12..14] += M3x3 * (x, y, z)
static void matTranslate(float* m, float x, float y, float z) {
    m[12] += x * m[0] + y * m[4] + z * m[8];
    m[13] += x * m[1] + y * m[5] + z * m[9];
    m[14] += x * m[2] + y * m[6] + z * m[10];
}

// Post-multiply scale: columns 0/1/2 scaled, translation untouched
static void matScale(float* m, float sx, float sy, float sz) {
    for (int r = 0; r < 4; ++r) {
        m[r] *= sx;
        m[4 + r] *= sy;
        m[8 + r] *= sz;
    }
}

using MatRotateFn = void*(__fastcall*)(float* out, const float* in, float angle, const float* axis);

static void __fastcall hkItemInHandRender(void* a1, void* a2, void* a3, uint32_t a4) {
    const ViewModel::Params& pm = ViewModel::GetParams();
    float* top = nullptr;
    if (ViewModel::IsActive() && a2)
        top = sehResolveWorldMatrixTop(a2);

    float saved[16];
    if (top) {
        std::memcpy(saved, top, sizeof(saved));

        MatRotateFn rot = reinterpret_cast<MatRotateFn>(SigManager::GetInstance().Matrix_rotateAxisAngle);
        float tmp[16];
        // translate - rotX - rotY - rotZ - scale
        matTranslate(top, pm.px, pm.py, pm.pz);
        if (rot) {
            constexpr float kD2R = 3.14159265358979f / 180.0f;
            const float ax[3] = { 1.0f, 0.0f, 0.0f };
            const float ay[3] = { 0.0f, 1.0f, 0.0f };
            const float az[3] = { 0.0f, 0.0f, 1.0f };
            if (pm.rx != 0.0f) { rot(tmp, top, pm.rx * kD2R, ax); std::memcpy(top, tmp, sizeof(tmp)); }
            if (pm.ry != 0.0f) { rot(tmp, top, pm.ry * kD2R, ay); std::memcpy(top, tmp, sizeof(tmp)); }
            if (pm.rz != 0.0f) { rot(tmp, top, pm.rz * kD2R, az); std::memcpy(top, tmp, sizeof(tmp)); }
        }
        matScale(top, pm.sx, pm.sy, pm.sz);
    }

    oRender(a1, a2, a3, a4);

    if (top) std::memcpy(top, saved, sizeof(saved));   // leave the stack clean
}

static bool s_hookTried = false;

static void InstallHook() {
    if (s_hookTried || oRender) return;
    s_hookTried = true;
    auto& sig = SigManager::GetInstance();
    const uintptr_t fn = sig.ItemInHandRenderer_render;
    if (!fn) {
        LOG_WARN("ItemInHandRenderer::render signature not resolved - ViewModel unavailable", "ViewModel");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(&hkItemInHandRender),
                       reinterpret_cast<LPVOID*>(&oRender)) != MH_OK
        || MH_EnableHook(reinterpret_cast<LPVOID>(fn)) != MH_OK) {
        oRender = nullptr;
        LOG_ERROR("ItemInHandRenderer::render hook failed", "ViewModel");
        return;
    }
    LOG_INFO("ItemInHandRenderer::render hooked", "ViewModel");
}

void ViewModel::RefreshParams() {
    Params p;
    p.px = static_cast<NumberSetting*>(mSettings[0])->mValue;
    p.py = static_cast<NumberSetting*>(mSettings[1])->mValue;
    p.pz = static_cast<NumberSetting*>(mSettings[2])->mValue;
    p.rx = static_cast<NumberSetting*>(mSettings[3])->mValue;
    p.ry = static_cast<NumberSetting*>(mSettings[4])->mValue;
    p.rz = static_cast<NumberSetting*>(mSettings[5])->mValue;
    p.sx = static_cast<NumberSetting*>(mSettings[6])->mValue;
    p.sy = static_cast<NumberSetting*>(mSettings[7])->mValue;
    p.sz = static_cast<NumberSetting*>(mSettings[8])->mValue;
    g_params = p;   // plain struct assignment, read on the render thread
}

void ViewModel::OnEnable() {
    InstallHook();
    RefreshParams();
    g_active.store(true, std::memory_order_relaxed);
    LOG_INFO(oRender ? "ViewModel enabled - transform composes under the vanilla hand animation"
                     : "ViewModel enabled (render hook unavailable)", "ViewModel");
}

void ViewModel::OnDisable() {
    g_active.store(false, std::memory_order_relaxed);
}
