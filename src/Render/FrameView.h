#pragma once
#include "../SDK/Types.h"
#include "../Core/Logger.h"
#include <cmath>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// Frame transform pipeline — freshest-camera queue.
//
// IDA-verified on 1.26.50.04 (Minecraft.Windows.exe, imagebase 0x140000000):
//   * 0x1404C0DA0 (CameraSetup::writeMatrices, the function this project
//     hooks) resolves the camera/matrix object by calling vtable byte-offset
//     0x5D0 = slot 186 on the IClientInstance stored at [cam+0x1B8], then
//     writes the VIEW 4x4 to [cam+0x388..0x3C4] and the PROJECTION 4x4 to
//     [cam+0x408..0x444] (16 movss stores each, confirmed in disasm). The
//     hook resolves the same object the same way (ci->vtable[186](ci)) —
//     see sehCaptureCameraTransform in GameHooks.cpp.
//     (1.26.42: writer 0x1404F92D0, slot 185, view +0x380, proj +0x400 — all
//     three drifted on 1.26.50.04; slot 185 on the new build is a perf-timer
//     getter, which is what caused the W2S jitter.)
//   * MATRIX SOURCE — the LevelRendererPlayer's own copies. sub_1446CED30
//     ("render level prep", called immediately BEFORE writeMatrices)
//     computes the view/projection from the active camera and stores them
//     BOTH into a shared matrix map AND directly onto the LRP
//     (view @ LRP+0xF18, projection @ LRP+0xF58 — Latite's getFovX/getFovY
//     are the projection's [0]/[5] elements). writeMatrices copies the MAP
//     copy into cam+0x388/+0x408. The two copies are identical UNTIL a
//     module scales the LRP elements (Zoom / Sprint FOV kick): the world
//     render projects with the LRP copy, so the capture reads THE LRP COPY
//     TOO — reading cam+0x408 instead left overlays at the pre-zoom screen
//     positions whenever a module changed the FOV (the "ESP stays at the
//     old place while the world zooms" bug; in-game FOV settings updates
//     both copies, which is why those tracked fine).
//
// TIMING — the empirically settled answer:
//   Serve the push ONE frame old. The live A/B sweep over delays 0..3 found
//   exactly ONE glued value: 1 (0 = overlays lead the world, 2+ = they trail
//   — "the ESP only returns to the target after a while"). The presented
//   buffer's world is rendered one writeMatrices pass before the Present
//   that shows it, and the queue reproduces that offset by construction.
//   The earlier backbuffer-index matching was a false ground truth (it
//   pigeonhole-matched a bb value within the scan window — "age avg=2.00"),
//   and delay 0 was equally wrong in the opposite direction. The BB scan is
//   kept purely as a diagnostic.
// ---------------------------------------------------------------------------

struct FrameTransform {
    float view[16];      // final MVP (proj * view, column-major) at push time
    Vec3  origin;        // render origin (LevelRendererPlayer camera pos)
    float timerAlpha;    // game Timer::alpha at push time (entity lerp fraction)
    unsigned long long pushMs; // wall-clock ms at push time (render-alpha reconstruction)
    unsigned serial;     // push order (monotonic, for age diagnostics)
    unsigned bb;         // swapchain backbuffer index at push time (0xFFFFFFFF = unknown)
    Vec3 lpPos;
    Vec3 lpPosOld;
    bool  lpValid = false;
    bool valid = false;
    float capturedFovX = 0.0f;
    float capturedFovY = 0.0f;
    bool fromLrp = false;
    float fovScale = 1.0f;
};

// Read the SERVED push's local-player pair (copies out; returns false when
// the served push had no local player). Defined in GameHooks.cpp.
bool ServedSelfPair(Vec3* pos, Vec3* posOld);

constexpr int kTransformRing = 32;  // ring capacity (history scan stays well below this)
constexpr int kQueueHistory  = 16;  // entries kept for backbuffer matching
constexpr int kBbScanDepth   = 12;  // how far back to search for a buffer match

extern FrameTransform g_transformRing[kTransformRing];
extern volatile long g_qTail;             // total pushes (ring slot = tail % ring)
extern volatile long g_qHead;             // oldest kept push (trimmed to kQueueHistory)
extern FrameTransform g_servedTransform; // the drained transform every overlay projects with
extern volatile float g_servedTimerAlpha; // served push's Timer::alpha (entity lerp fraction; -1 = unknown)
extern volatile float g_servedFrameAlpha; // Timer::alpha OF THE SERVED PUSH (see below; -1 = unknown)
extern volatile bool  g_servedValid;      // true once the first drain served an entry
extern volatile unsigned g_presentSeq;   // ++ at each Present (diagnostics only)
extern volatile long g_pushedThisFrame;   // per-frame push dedup (first camera wins)
extern volatile long g_camCallsThisFrame; // writeMatrices calls since the last Present
extern volatile long g_camRawCallsThisFrame; // RAW writeMatrices calls (incl. deduped-out)

// The game's swapchain as last seen at Present (owned by DXHooks) and the
// SEH-guarded backbuffer index probe used by both the producer and Present.
extern void* g_gameSwapChain;
unsigned QueryPresentBackBufferIndex();

// Snapshot of the camera transform captured at Present time (final fallback
// for the very first frames, before the queue has filled).
struct FrameView {
    float mvp[16];     // proj * view, column-major
    Vec3  origin;      // render origin (camera-relative space)
    volatile bool valid = false;
    unsigned int frame = 0; // increments each capture (staleness guard)
};

extern FrameView g_frameView;

extern volatile float g_zoomProjScale;

extern volatile float g_sprintFovScale;

// ---------------------------------------------------------------------------
// Producer — called from the CameraSetup_writeMatrices hook (GameHooks) right
// AFTER the original returned. view/proj are the 4x4 float matrices at
// camObj+0x380 / +0x400. SEH lives in the caller; this only does pure math
// + queue write. First camera setup per Present interval wins (the world
// camera; later per-frame calls are hand/shadow cameras — pushing them all
// would corrupt the one-push-per-frame cadence the diagnostics expect).
// timerAlpha is the game's Timer::alpha sampled at this exact moment — the
// entity-lerp fraction the world pass renders with. Overlays interpolate
// actor positions with the SERVED push's alpha so their anchor lands on the
// model exactly as drawn in the presented frame.
// ---------------------------------------------------------------------------
static inline bool pushCameraTransform(const float* view, const float* proj, const Vec3& origin,
                                        float timerAlpha,
                                        const Vec3& lpPos, const Vec3& lpPosOld, bool lpValid,
                                        bool fromLrp = false) {
    if (g_pushedThisFrame != 0) return false;   // one (world) camera setup per frame

    bool ok = true;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(view[i]) || !std::isfinite(proj[i])) { ok = false; break; }
    if (!ok) return false;

    // Zero origin means the world pass hasn't been positioned yet (menus) —
    // the consumer rejects it anyway, so don't pollute the queue.
    if (origin.x == 0.0f && origin.y == 0.0f && origin.z == 0.0f) return false;

    g_pushedThisFrame = 1;
    g_camCallsThisFrame = g_camCallsThisFrame + 1;

    const long tail = g_qTail;
    const long head = g_qHead;
    if (tail - head >= kTransformRing) g_qHead = head + 1;

    const long idx = tail % kTransformRing;
    mat4Multiply(proj, view, g_transformRing[idx].view);  // stored mvp = proj * view
    g_transformRing[idx].capturedFovX = proj[0];
    g_transformRing[idx].capturedFovY = proj[5];
    g_transformRing[idx].fromLrp = fromLrp;
    g_transformRing[idx].fovScale = g_zoomProjScale * g_sprintFovScale;
    g_transformRing[idx].origin = origin;
    g_transformRing[idx].timerAlpha = timerAlpha;
    g_transformRing[idx].lpPos = lpPos;
    g_transformRing[idx].lpPosOld = lpPosOld;
    g_transformRing[idx].lpValid = lpValid;
    g_transformRing[idx].pushMs = GetTickCount64();
    g_transformRing[idx].serial = (unsigned)tail;
    g_transformRing[idx].bb = QueryPresentBackBufferIndex();
    g_transformRing[idx].valid = true;
    g_qTail = tail + 1;
    return true;
}

// ---------------------------------------------------------------------------
// Consumer drain — called once per Present (BeginPresentFrame in GameHooks),
// BEFORE the overlay draws. Serves the push ONE frame old.
// ---------------------------------------------------------------------------
static inline void drainCameraQueue(unsigned bbNow) {
    (void)bbNow;
    const long tail = g_qTail;
    const long avail = tail - g_qHead;
    if (avail <= 0) return;
    if (avail > kQueueHistory) g_qHead = tail - kQueueHistory;
    long pick = (avail > 1) ? tail - 2 : tail - 1;
    const FrameTransform& t = g_transformRing[(pick % kTransformRing + kTransformRing) % kTransformRing];
    if (!t.valid) return;
    g_servedTransform = t;
    g_servedTimerAlpha = t.timerAlpha;
    g_servedFrameAlpha = t.timerAlpha;
    g_servedValid = true;
}

// ---------------------------------------------------------------------------
#include "../SDK/Client/ClientInstance.h"
#include "../SDK/Client/LevelRendererPlayer.h"

static inline bool acquireCameraMVP(SDK::ClientInstance* ci, float mvpOut[16], Vec3& originOut) {
    // ---- Primary: the drained backbuffer-locked transform ------------------
    if (g_servedValid) {
        bool okMat = true;
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(g_servedTransform.view[i])) { okMat = false; break; }
        if (okMat && !(g_servedTransform.origin.x == 0.0f &&
                       g_servedTransform.origin.y == 0.0f &&
                       g_servedTransform.origin.z == 0.0f)) {
            for (int i = 0; i < 16; ++i) mvpOut[i] = g_servedTransform.view[i];
            originOut = g_servedTransform.origin;
            if (g_servedTransform.fromLrp) {
                const float capX = g_servedTransform.capturedFovX;
                const float capY = g_servedTransform.capturedFovY;
                float liveX = 0.0f, liveY = 0.0f;
                if (capX != 0.0f && capY != 0.0f && SDK::sehReadFovValues(&liveX, &liveY)) {
                    const float sx = liveX / capX;
                    const float sy = liveY / capY;
                    if (std::isfinite(sx) && std::isfinite(sy) &&
                        sx > 0.05f && sx < 20.0f && sy > 0.05f && sy < 20.0f &&
                        (std::fabs(sx - 1.0f) > 0.0005f || std::fabs(sy - 1.0f) > 0.0005f)) {
                        mvpOut[0]  *= sx; mvpOut[4]  *= sx; mvpOut[8]  *= sx; mvpOut[12] *= sx;
                        mvpOut[1]  *= sy; mvpOut[5]  *= sy; mvpOut[9]  *= sy; mvpOut[13] *= sy;
                    }
                }
            } else {
                const float k = g_servedTransform.fovScale;
                if (std::isfinite(k) && k > 0.01f && k < 100.0f &&
                    (std::fabs(k - 1.0f) > 0.0005f)) {
                    mvpOut[0]  *= k; mvpOut[4]  *= k; mvpOut[8]  *= k; mvpOut[12] *= k;
                    mvpOut[1]  *= k; mvpOut[5]  *= k; mvpOut[9]  *= k; mvpOut[13] *= k;
                }
            }
            return true;
        }
    }

    // ---- Fallbacks: live Present-time matrices, then old snapshot ----------
    if (!ci || !sdk_safe_ptr(ci, sizeof(void*))) return false;
    SDK::MinecraftGame* mcg = ci->getMinecraftGame();
    if (!mcg || !sdk_safe_ptr(mcg, sizeof(void*))) return false;
    SDK::GameRenderer* gr = mcg->getGameRenderer();
    if (!gr || !sdk_safe_ptr(gr, sizeof(void*))) return false;

    Matrix4* viewM = &gr->lastViewMatrix;
    Matrix4* projM = &gr->lastProjectionMatrix;
    if (!sdk_safe_ptr(viewM, sizeof(Matrix4)) || !sdk_safe_ptr(projM, sizeof(Matrix4))) return false;

    mat4Multiply(projM->m, viewM->m, mvpOut); // mvp = proj * view (column-major)

    originOut = Vec3(0, 0, 0);
    SDK::LevelRenderer* lr = ci->getLevelRenderer();
    if (lr && sdk_safe_ptr(lr, sizeof(void*))) {
        SDK::LevelRendererPlayer* lrp = lr->getLevelRendererPlayer();
        if (lrp && sdk_safe_ptr(lrp, sizeof(void*))) originOut = lrp->getOrigin();
    }
    if ((originOut.x == 0.0f && originOut.y == 0.0f && originOut.z == 0.0f) && g_frameView.valid) {
        for (int i = 0; i < 16; ++i) mvpOut[i] = g_frameView.mvp[i];
        originOut = g_frameView.origin;
    }
    return true;
}
