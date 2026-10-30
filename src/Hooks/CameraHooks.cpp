// Camera transform capture — the world-to-screen source for ESP/Tracers.
//
// Producer: hkCameraWriteMatrices pushes transforms into the ring (records the
// swapchain backbuffer index at push time). Consumer: GameHooks::BeginPresentFrame
// (Present hook) serves the push that rendered the buffer being presented.
// Full timing rationale lives in Render/FrameView.h.
//
// This TU also owns the FrameView camera-queue globals (FrameView.h is
// header-only) and the served-transform accessors RenderPos.h declares.
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../SDK/Client/ClientInstance.h"
#include "../SDK/Client/LevelRendererPlayer.h"
#include "../SDK/Actor/RenderPos.h"
#include "../Render/FrameView.h"
#include "../Memory/OffsetManager.h"
#include "../Memory/SigManager.h"
#include "../Core/Logger.h"
#include "MinHook.h"
#include <atomic>
#include <cmath>

FrameView g_frameView;

FrameTransform g_transformRing[kTransformRing];
volatile long g_qTail = 0;
volatile long g_qHead = 0;
volatile long g_camDelayFrames = -1;  // unused A/B knob; kept for the FrameView.h extern
FrameTransform g_servedTransform;
volatile float g_servedTimerAlpha = -1.0f;
volatile float g_servedFrameAlpha = -1.0f;
volatile bool g_servedValid = false;
volatile unsigned g_presentSeq = 0;
volatile long g_pushedThisFrame = 0;
volatile long g_camCallsThisFrame = 0;
volatile long g_camRawCallsThisFrame = 0;

unsigned ServedPushSerial() {
    return g_servedTransform.valid ? g_servedTransform.serial : 0xFFFFFFFFu;
}

bool ServedSelfPair(Vec3* pos, Vec3* posOld) {
    if (!g_servedTransform.valid || !g_servedTransform.lpValid || !pos || !posOld)
        return false;
    *pos = g_servedTransform.lpPos;
    *posOld = g_servedTransform.lpPosOld;
    return true;
}

namespace {
    using namespace SDK;

    typedef void(__fastcall* CameraSetup_writeMatrices_t)(void* cam, void* a2);
    CameraSetup_writeMatrices_t oCameraWriteMatrices = nullptr;

    // Raw game reads go through this SEH helper: the world can be tearing down
    // while the render thread walks these pointers.
    static bool sehCaptureCameraTransform(void* camThis) {
        void* ci = nullptr;
        {
            uintptr_t cam = (uintptr_t)camThis;
            if (!cam || !sdk_safe_ptr((void*)cam, 0x1C0)) return false;
            uintptr_t ctrl = *(uintptr_t*)(cam + 0x1A8);
            void* ptr = *(void**)(cam + 0x1B8);
            if (!ptr || !sdk_safe_ptr(ptr, sizeof(void*))) return false;
            if (ctrl) {
                if (!sdk_safe_ptr((void*)ctrl, 8)) return false;
                if (*(uint8_t*)ctrl == 0) return false;   // dangling NonOwnerPointer
            }
            ci = ptr;
        }
        if (!ci) return false;

        void* mat = nullptr;
        float timerAlpha = -1.0f;
        __try {
            void** vt = *(void***)ci;
            if (!vt || !sdk_safe_ptr(vt, Layout::VTable::Guard(Layout::VTable::ClientInstance_getGameRenderer))) return false;
            void* fn = vt[Layout::VTable::ClientInstance_getGameRenderer];
            if (!fn || !sdk_safe_ptr(fn, 16)) return false;
            mat = ((void*(__fastcall*)(void*))fn)(ci);

            // Timer::alpha is the third float in Timer.
            uintptr_t mc = *(uintptr_t*)((uintptr_t)ci + Offsets().CI_Minecraft);
            if (mc && sdk_safe_ptr((void*)mc, 0x100)) {
                uintptr_t tm = *(uintptr_t*)(mc + Offsets().MC_Timer);
                if (tm && sdk_safe_ptr((void*)tm, sizeof(SDK::Timer))) {
                    float a = *(float*)(tm + 8);
                    if (a >= 0.0f && a <= 1.0f) timerAlpha = a;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!mat || !sdk_safe_ptr(mat, 0x480)) return false;

        SDK::LevelRenderer* lr = *(SDK::LevelRenderer**)((uintptr_t)ci + Offsets().CI_LevelRenderer);
        if (!lr || !sdk_safe_ptr((void*)lr, Offsets().LevelRenderer_mRendererPlayer + sizeof(void*))) return false;
        SDK::LevelRendererPlayer* lrp = *(SDK::LevelRendererPlayer**)((uintptr_t)lr + Offsets().LevelRenderer_mRendererPlayer);
        if (!lrp || !sdk_safe_ptr((void*)lrp, Offsets().LevelRendererPlayer_mProjMatrix + sizeof(Matrix4))) return false;
        Vec3 origin = lrp->getOrigin();

        Vec3 lpPos(0, 0, 0), lpPosOld(0, 0, 0);
        bool lpValid = false;
        __try {
            SDK::Actor* lp = ((SDK::Actor*(__fastcall*)(void*))(*(void***)ci)[Layout::VTable::ClientInstance_getLocalPlayer])(ci);
            if (lp && sdk_safe_ptr(lp, 0x400)) {
                lpPos = lp->getPos();
                lpPosOld = lp->getPosOld();
                lpValid = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { lpValid = false; }

        {
            bool okCam = false;
            float view[16], proj[16];
            __try {
                const float* v = (const float*)((uintptr_t)mat + offsetof(SDK::GameRenderer, lastViewMatrix));
                const float* p = (const float*)((uintptr_t)mat + offsetof(SDK::GameRenderer, lastProjectionMatrix));
                for (int i = 0; i < 16; ++i) { view[i] = v[i]; proj[i] = p[i]; }
                okCam = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { okCam = false; }
            if (okCam) {
                bool finite = true;
                for (int i = 0; i < 16; ++i)
                    if (!std::isfinite(view[i]) || !std::isfinite(proj[i])) { finite = false; break; }
                if (finite)
                    return pushCameraTransform(view, proj, origin, timerAlpha,
                                               lpPos, lpPosOld, lpValid,
                                               /*fromLrp=*/false);
            }
        }

        // Fallback: the LRP's own copies (carry module FOV scaling; Present
        // rescales by the live/captured ratio for these pushes).
        float view[16], proj[16];
        if (SDK::sehReadLrpMatrices(view, proj)) {
            return pushCameraTransform(view, proj, origin, timerAlpha,
                                       lpPos, lpPosOld, lpValid,
                                       /*fromLrp=*/true);
        }

        return false;
    }

    // Actor pair snapshot AT PUSH TIME (see RenderPos.h PushPairRing for why
    // the pair must be sampled at push time). Runs outside any __try so C++
    // containers are legal; raw game access stays inside POD SEH helpers.
    static void capturePushActorPairs() {
        SDK::GameContext ctx;
        if (!SDK::resolveContext(ctx, /*needLevel=*/true)) return;
        SDK::CapturePushPairs(ctx.level);
    }

    void __fastcall hkCameraWriteMatrices(void* cam, void* a2) {
        if (!oCameraWriteMatrices) return;
        g_camRawCallsThisFrame = g_camRawCallsThisFrame + 1;
        oCameraWriteMatrices(cam, a2);
        bool pushed = false;
        __try { pushed = sehCaptureCameraTransform(cam); }
        __except (EXCEPTION_EXECUTE_HANDLER) { pushed = false; }
        if (pushed) capturePushActorPairs();
    }

    static void mat4Mul_fv(const float* a, const float* b, float* out) {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[col * 4 + k];
                out[col * 4 + row] = s;
            }
    }

    // Capture view*proj + origin during the CURRENT frame's world render;
    // overlays read g_frameView at Present time.
    static bool captureFrameViewImpl() {
        __try {
            SDK::ClientInstance* ci = SDK::ClientInstance::get();
            if (!ci || !sdk_safe_ptr(ci, sizeof(void*))) return false;
            SDK::MinecraftGame* mcg = ci->getMinecraftGame();
            if (!mcg || !sdk_safe_ptr(mcg, sizeof(void*))) return false;
            SDK::GameRenderer* gr = mcg->getGameRenderer();
            if (!gr || !sdk_safe_ptr(gr, sizeof(void*))) return false;
            Matrix4* viewM = &gr->lastViewMatrix;
            Matrix4* projM = &gr->lastProjectionMatrix;
            if (!sdk_safe_ptr(viewM, sizeof(Matrix4)) || !sdk_safe_ptr(projM, sizeof(Matrix4))) return false;
            float mvp[16];
            mat4Mul_fv(projM->m, viewM->m, mvp);
            Vec3 origin(0, 0, 0);
            SDK::LevelRenderer* lr = ci->getLevelRenderer();
            if (lr && sdk_safe_ptr(lr, sizeof(void*))) {
                SDK::LevelRendererPlayer* lrp = lr->getLevelRendererPlayer();
                if (lrp && sdk_safe_ptr(lrp, sizeof(void*))) origin = lrp->getOrigin();
            }
            for (int i = 0; i < 16; ++i) g_frameView.mvp[i] = mvp[i];
            g_frameView.origin = origin;
            g_frameView.frame++;
            g_frameView.valid = true;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

void InstallCameraHooks() {
    uintptr_t camAddr = SigManager::GetInstance().CameraSetup_writeMatrices;
    if (camAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(camAddr), &hkCameraWriteMatrices,
                reinterpret_cast<LPVOID*>(&oCameraWriteMatrices)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(camAddr));
            LOG_INFO("CameraSetup::writeMatrices hooked (world-to-screen source)", "GameHooks");
        } else {
            LOG_ERROR("FAILED to hook CameraSetup::writeMatrices", "GameHooks");
        }
    } else {
        LOG_WARN("CameraSetup::writeMatrices signature not found; overlays fall back to Present-time capture", "GameHooks");
    }
}

// Called from the DX Present hook AFTER the world render wrote this frame's
// lastViewMatrix — a renderLevel-time read would grab the previous frame.
void GameHooks::CaptureFrameViewNow() {
    captureFrameViewImpl();
}

void GameHooks::BeginPresentFrame(unsigned bbNow) {
    g_presentSeq = g_presentSeq + 1;
    drainCameraQueue(bbNow);
    // Present marks the frame boundary: re-arm the camera push dedup.
    g_pushedThisFrame = 0;
    g_camCallsThisFrame = 0;
    g_camRawCallsThisFrame = 0;
}
