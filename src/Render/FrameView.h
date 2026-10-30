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
    // Projection [0]/[5] at push time. Present-time W2S rescales the served
    // MVP by live/captured so Zoom/Sprint FOV writes after the capture still
    // land overlays on the world.
    float capturedFovX = 0.0f;
    float capturedFovY = 0.0f;
    // TRUE when view/proj came from the LRP copies (they already carry
    // module FOV scaling -> Present rescales by live/captured). FALSE when
    // they came from the camera object (module-scaling-free base -> Present
    // applies the PUSH's own recorded fovScale instead). The flag MUST be
    // part of the push: mixing the two rescale rules across frames is
    // exactly the double-ESP flicker.
    bool fromLrp = false;
    // The module FOV total (g_zoomProjScale * g_sprintFovScale) AS OF PUSH
    // TIME. Present must rescale by THIS, never by a live read: the live
    // value races the world render (which applies its scale at the
    // renderLevel hook, between capture and draw) and any disagreement shows
    // the overlays at the wrong scale until the animation settles.
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
                                              // — >1 per interval means several cameras
                                              // run per frame and the FIRST may not be
                                              // the world pass (jitter suspect).

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

// Live zoom projection scale published by the Zoom module (1.0 = not zooming).
// OWNERSHIP FLAG (Sprint stays off the FOV fields while Zoom magnifies) AND,
// since the flicker fix, ALSO the overlay's zoom multiplier: the capture now
// pushes the camera-object matrices (cam+0x388/+0x408 — the game's own copies,
// written by writeMatrices itself, NEVER touched by Zoom/Sprint), and Present
// applies this multiplier to the projection rows. The LRP projection copy
// (LRP+0xF58) is written by THREE writers (render-prep's map copy, Zoom,
// Sprint — verified in IDA: sub_1446CED30 stores 16 dwords into it per call),
// so the old live/captured rescale read a contested value and the overlays
// flickered between the zoomed and unzoomed positions.
extern volatile float g_zoomProjScale;

// Sprint's own FOV kick multiplier (s_fovMod in Sprint.cpp), published for the
// same reason: Sprint writes LRP+0xF58 post-computation, so the camera-object
// projection does NOT carry its kick and the overlay has to apply it itself.
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
    // Overflow guard: pushes outpacing the trim (should not happen with the
    // per-frame dedup) would walk over unread entries — drop the OLDEST by
    // bumping head so the ring never wraps live data.
    if (tail - head >= kTransformRing) g_qHead = head + 1;

    const long idx = tail % kTransformRing;
    mat4Multiply(proj, view, g_transformRing[idx].view);  // stored mvp = proj * view
    g_transformRing[idx].capturedFovX = proj[0];
    g_transformRing[idx].capturedFovY = proj[5];
    g_transformRing[idx].fromLrp = fromLrp;
    // The module FOV total at push time — the scale the world render applies
    // to these exact base matrices (sehApplyFovMod writes zoom*sprint into the
    // LRP focal elements right before the draw). Recorded per push so Present
    // never races a live read against the render thread.
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

    // ---- producer diagnostics (time-windowed, ~2 lines/sec max) -----------
    // fwdRot: angular delta of the camera forward axis between consecutive
    // pushes — must be ~constant while the user turns at constant speed.
    // matO is the origin encoded in the raw view matrix (this build stores a
    // rotation-only view; the origin lives in LevelRendererPlayer), logged so
    // a build change that bakes translation into the view is caught by dOrg.
    static unsigned long long s_lastWinMs = 0;
    static unsigned s_winPushes = 0;
    static float s_rotSum = 0.0f, s_rotMax = 0.0f, s_orgMax = 0.0f;
    static float s_alphaSum = 0.0f, s_alphaMin = 1e9f, s_alphaMax = -1e9f;
    static unsigned s_alphaN = 0;
    static float s_prevFwd[3] = {0, 0, 0};
    static bool s_havePrevFwd = false;

    ++s_winPushes;
    if (std::isfinite(timerAlpha) && timerAlpha >= 0.0f && timerAlpha <= 1.0f) {
        s_alphaSum += timerAlpha;
        if (timerAlpha < s_alphaMin) s_alphaMin = timerAlpha;
        if (timerAlpha > s_alphaMax) s_alphaMax = timerAlpha;
        ++s_alphaN;
    }
    const float* v = g_transformRing[idx].view;   // stored mvp; row 2 keeps the view's z axis
    float fx = v[2], fy = v[6], fz = v[10];
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl > 1e-6f) { fx /= fl; fy /= fl; fz /= fl; }
    if (s_havePrevFwd) {
        float dot = fx * s_prevFwd[0] + fy * s_prevFwd[1] + fz * s_prevFwd[2];
        if (dot > 1.0f) dot = 1.0f;
        if (dot < -1.0f) dot = -1.0f;
        float deg = std::acos(dot) * 57.2957795f;
        s_rotSum += deg;
        if (deg > s_rotMax) s_rotMax = deg;
    }
    s_prevFwd[0] = fx; s_prevFwd[1] = fy; s_prevFwd[2] = fz;
    s_havePrevFwd = true;

    const unsigned long long nowMs = GetTickCount64();
    if (nowMs - s_lastWinMs >= 2000ull) {
        const float winSec = (float)(nowMs - s_lastWinMs) / 1000.0f;
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "push: %u in %.2fs (%.0f/s) fwdRot avg=%.2f max=%.2f deg | "
                      "timerAlpha avg=%.2f min=%.2f max=%.2f (n=%u) | "
                      "lrpO(%.1f,%.1f,%.1f) serial=%u bb=%u camCalls=%ld raw=%ld",
                      s_winPushes, winSec, (float)s_winPushes / winSec,
                      s_rotSum / (s_winPushes > 0 ? s_winPushes : 1), s_rotMax,
                      s_alphaN ? s_alphaSum / s_alphaN : -1.0f,
                      s_alphaN ? s_alphaMin : -1.0f,
                      s_alphaN ? s_alphaMax : -1.0f, s_alphaN,
                      origin.x, origin.y, origin.z,
                      g_transformRing[idx].serial, g_transformRing[idx].bb,
                      g_camCallsThisFrame, g_camRawCallsThisFrame);
        // (self pair deliberately not logged — never changes shape)
        LOG_INFO(buf, "FrameView");
        s_winPushes = 0; s_rotSum = 0; s_rotMax = 0; s_orgMax = 0;
        s_alphaSum = 0; s_alphaMin = 1e9f; s_alphaMax = -1e9f; s_alphaN = 0;
        s_lastWinMs = nowMs;
    }
}

// srcDelta cross-check stats (published by srcDeltaCrossCheck, printed by the
// drain log): rotation delta between the SERVED frame-B matrix and the LIVE
// GameRenderer::lastViewMatrix at Present. With the newest-push serving this
// MUST read ~0 while rotating (served == the matrix the world just rendered
// with; the live GameRenderer matrix is written by that same render). A
// systematic ~Nx servedRot while rotating means the served matrix is N frames
// off the presented world — the regression signal for the rotational lag.
static float g_srcDeltaAvg = 0.0f, g_srcDeltaMax = 0.0f;
static unsigned g_srcDeltaN = 0;

static void srcDeltaCrossCheck(unsigned long long nowMs);

// ---------------------------------------------------------------------------
// Consumer drain — called once per Present (BeginPresentFrame in GameHooks),
// BEFORE the overlay draws, with the backbuffer index about to be presented.
//
// Selection: the push ONE frame old — the A/B-verified glue value (see the
// header comment). The backbuffer scan is kept for DIAGNOSTICS only.
// ---------------------------------------------------------------------------
static inline void drainCameraQueue(unsigned bbNow) {
    static unsigned long long s_lastWinMs = 0;
    static long s_lastTail = -1;                 // g_qTail at the previous Present
    static unsigned s_winPresents = 0, s_winServes = 0, s_winStarved = 0;
    static unsigned s_winBbMatch = 0, s_winBbMiss = 0, s_winBbUnknown = 0;
    static unsigned s_hist[4] = {0, 0, 0, 0};   // pushes per present interval: 0/1/2/3+
    static float s_ageSum = 0; static long s_ageMax = 0;
    static float s_srvRotSum = 0, s_srvRotMax = 0, s_srvRotMin = 1e9f;
    static unsigned s_srvRotN = 0;
    static float s_jerkSum = 0, s_jerkMax = 0;      // |Δrot[i] - Δrot[i-1]|: the
    static float s_srvPrevRot = -1.0f;              // wobble signature (smooth
    static unsigned s_jerkN = 0;                    // turning -> small, jitter -> large)
    static float s_srvPxSum = 0, s_srvPxMax = 0;
    static unsigned s_srvPxN = 0;
    static FrameTransform s_prevServed;
    static bool s_havePrevServed = false;

    // Per-interval push histogram (the dedup normally keeps this at exactly 1;
    // 0 = a world pass was skipped that frame).
    const long tail = g_qTail;
    long pushedThisInterval = 0;
    if (s_lastTail < 0) s_lastTail = tail;
    pushedThisInterval = tail - s_lastTail;
    s_lastTail = tail;
    if (pushedThisInterval < 0) pushedThisInterval = 0;
    ++s_hist[pushedThisInterval < 4 ? pushedThisInterval : 3];

    // ---- selection ---------------------------------------------------------
    // Serve the push ONE frame old. The user's live A/B swept 0..3 and only
    // delay 1 glued the overlays to the world (0 = leading, 2+ = trailing) —
    // the presented buffer's world was rendered one writeMatrices before the
    // Present that shows it. Falls back to the newest push while the queue
    // is shallow. The BB scan stays diagnostics-only.
    const long avail = tail - g_qHead;
    long pick = tail - 1;
    if (avail > 1) pick = tail - 2;
    if (bbNow != 0xFFFFFFFFu) {
        // Diagnostic-only BB scan (does NOT drive the selection).
        const long scan = (avail < kBbScanDepth) ? avail : kBbScanDepth;
        for (long k = 0; k < scan; ++k) {
            const long i = tail - 1 - k;
            const FrameTransform& t = g_transformRing[i % kTransformRing];
            if (t.valid && t.bb == bbNow) { ++s_winBbMatch; break; }
        }
    } else {
        ++s_winBbUnknown;
    }
    // Trim history so the ring can never wrap live data.
    if (avail > kQueueHistory) g_qHead = tail - kQueueHistory;

    bool swapped = false;
    FrameTransform next = g_servedTransform;   // work copy for diagnostics
    if (avail > 0) {
        const FrameTransform& t = g_transformRing[pick % kTransformRing];
        if (t.valid) { next = t; swapped = true; }
    }

    if (swapped) {
        // ---- served-transform diagnostics --------------------------------
        // AGE = how many pushes the served entry sits behind the newest one
        // (0 in auto mode by design; >0 only in fixed-delay A/B mode).
        const long age = tail - 1 - pick;
        s_ageSum += (float)age;
        if (age > s_ageMax) s_ageMax = age;

        // Rotation delta between consecutive SERVED matrices: any spread
        // (max-min) while turning smoothly is residual jitter in the making.
        if (s_havePrevServed) {
            const float* a = s_prevServed.view;   // column-major mvp
            const float* b = next.view;
            // Camera-space z axis of the view half lives in mvp row 2
            // (elements m[2], m[6], m[10]) — proj scales it but keeps direction.
            float ax = a[2], ay = a[6], az = a[10];
            float bx = b[2], by = b[6], bz = b[10];
            float al = std::sqrt(ax * ax + ay * ay + az * az);
            float bl = std::sqrt(bx * bx + by * by + bz * bz);
            if (al > 1e-6f && bl > 1e-6f) {
                float dot = (ax * bx + ay * by + az * bz) / (al * bl);
                if (dot > 1.0f) dot = 1.0f;
                if (dot < -1.0f) dot = -1.0f;
                float deg = std::acos(dot) * 57.2957795f;
                s_srvRotSum += deg; if (deg > s_srvRotMax) s_srvRotMax = deg;
                if (deg < s_srvRotMin) s_srvRotMin = deg; ++s_srvRotN;
                if (s_srvPrevRot >= 0.0f) {
                    const float j = std::fabs(deg - s_srvPrevRot);
                    s_jerkSum += j; if (j > s_jerkMax) s_jerkMax = j; ++s_jerkN;
                }
                s_srvPrevRot = deg;
            }
            // Pixel metric: how far the SAME synthetic world point (10 m ahead
            // of the camera + 3 m to its right) moves on screen between the
            // previous and current served transform. Smooth turning ->
            // steady per-frame px; spikes = a served matrix that does not
            // belong to the presented frame. The forward sign is resolved by
            // testing which direction lands in front of the camera (cw > 0).
            float fl2 = std::sqrt(bx * bx + by * by + bz * bz);
            float rx = b[0], ry = b[4], rz = b[8];
            float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
            if (fl2 > 1e-6f && rl > 1e-6f) {
                const float scrW = 1920.0f, scrH = 1080.0f;
                bool done = false;
                for (int sign = -1; sign <= 1 && !done; sign += 2) {
                    const float fwx = sign * bx / fl2, fwy = sign * by / fl2, fwz = sign * bz / fl2;
                    const float rgx = rx / rl, rgy = ry / rl, rgz = rz / rl;
                    const float wp[3] = { next.origin.x + fwx * 10.0f + rgx * 3.0f,
                                          next.origin.y + fwy * 10.0f + rgy * 3.0f,
                                          next.origin.z + fwz * 10.0f + rgz * 3.0f };
                    float sx[2] = {0, 0}, sy[2] = {0, 0};
                    bool ok = true;
                    for (int m = 0; m < 2 && ok; ++m) {
                        const float* mm = (m == 0) ? a : b;
                        const Vec3& org = (m == 0) ? s_prevServed.origin : next.origin;
                        const float pxx = wp[0] - org.x, pyy = wp[1] - org.y, pzz = wp[2] - org.z;
                        const float cx = mm[0] * pxx + mm[4] * pyy + mm[8]  * pzz + mm[12];
                        const float cy = mm[1] * pxx + mm[5] * pyy + mm[9]  * pzz + mm[13];
                        const float cw = mm[3] * pxx + mm[7] * pyy + mm[11] * pzz + mm[15];
                        if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cw) || cw < 0.1f) {
                            ok = false;
                            break;
                        }
                        const float inv = 1.0f / cw;
                        sx[m] = (cx * inv + 1.0f) * 0.5f * scrW;
                        sy[m] = (1.0f - cy * inv) * 0.5f * scrH;
                    }
                    if (ok) {
                        const float dx = sx[1] - sx[0], dy = sy[1] - sy[0];
                        const float d = std::sqrt(dx * dx + dy * dy);
                        s_srvPxSum += d; if (d > s_srvPxMax) s_srvPxMax = d; ++s_srvPxN;
                        done = true;   // one sample per frame — stop after the first forward sign that lands in front
                    }
                }
            }
        }
        s_prevServed = next;
        s_havePrevServed = true;

        g_servedTransform = next;
        g_servedTimerAlpha = next.timerAlpha;
        // ---- entity-lerp info of the SERVED push -----------------------------
        // Actor pairs are now captured AT PUSH TIME (GameHooks walks the actor
        // list right after writeMatrices and stores each actor's posOld/pos
        // under the push's serial). The pair AND the alpha below therefore
        // describe the SAME instant — the frame the presented world drew its
        // models from — so lerp(pair@push, alpha@push) is exact by
        // construction. (The old Present-time pair capture paired a FRESH
        // pair with a one-frame-old alpha: static actors cancelled the
        // mismatch, moving actors visibly stepped at tick rate.)
        g_servedFrameAlpha = next.timerAlpha;
        g_servedValid = true;
        ++s_winServes;
    } else {
        if (avail <= 0) ++s_winStarved;
    }

    ++s_winPresents;

    const unsigned long long nowMs = GetTickCount64();

    // ---- source cross-check (POD-only impl lives after the SDK include) ----
    // Compares the SERVED (writeMatrices @ frame B) rotation with the LIVE
    // GameRenderer::lastViewMatrix read at Present — the definitive answer to
    // "is our captured matrix the one the world actually renders with?".
    srcDeltaCrossCheck(nowMs);

    if (nowMs - s_lastWinMs >= 2000ull) {
        const float winSec = (float)(nowMs - s_lastWinMs) / 1000.0f;
        const unsigned serves = s_winServes;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "drain: %u presents in %.2fs (%.0f fps) serves=%u starved=%u | "
                      "bbDiag match/miss/unknown=%u/%u/%u age avg=%.2f max=%ld | "
                      "hist 0/1/2/3+=%u/%u/%u/%u | delay=1 qDepth=%ld | "
                      "servedRot avg=%.2f max=%.2f min=%.2f deg (n=%u) | "
                      "jerk avg=%.2f max=%.2f deg/f (n=%u) | "
                      "srcDeltaR avg=%.2f max=%.2f deg (n=%u) | "
                      "probePt avg=%.1f max=%.1f px/f (n=%u) | servedAlpha=%.2f",
                      s_winPresents, winSec, (float)s_winPresents / winSec,
                      serves, s_winStarved,
                      s_winBbMatch, s_winBbMiss, s_winBbUnknown,
                      serves ? s_ageSum / serves : 0.0f, s_ageMax,
                      s_hist[0], s_hist[1], s_hist[2], s_hist[3],
                      g_qTail - g_qHead,
                      s_srvRotN ? s_srvRotSum / s_srvRotN : 0.0f, s_srvRotMax,
                      s_srvRotN ? s_srvRotMin : 0.0f, s_srvRotN,
                      s_jerkN ? s_jerkSum / s_jerkN : 0.0f, s_jerkMax, s_jerkN,
                      g_srcDeltaAvg, g_srcDeltaMax, g_srcDeltaN,
                      s_srvPxN ? s_srvPxSum / s_srvPxN : 0.0f, s_srvPxMax, s_srvPxN,
                      g_servedTimerAlpha);        LOG_INFO(buf, "FrameView");
        s_winPresents = 0; s_winServes = 0; s_winStarved = 0;
        s_winBbMatch = 0; s_winBbMiss = 0; s_winBbUnknown = 0;
        s_ageSum = 0; s_ageMax = 0;
        s_hist[0] = s_hist[1] = s_hist[2] = s_hist[3] = 0;
        s_srvRotSum = 0; s_srvRotMax = 0; s_srvRotMin = 1e9f; s_srvRotN = 0;
        s_jerkSum = 0; s_jerkMax = 0; s_jerkN = 0; s_srvPrevRot = -1.0f;
        s_srvPxSum = 0; s_srvPxMax = 0; s_srvPxN = 0;
        s_lastWinMs = nowMs;
    }
}

// ---------------------------------------------------------------------------
// Camera MVP acquisition — shared by every world-space Visual module
// (ESP/Tracers/NameTags/ChinaHat/BlockESP/BlockTracers/JumpCircles).
//
// Returns false when the matrices/renderer aren't available yet. On success,
// mvpOut[16] and originOut hold a ready-to-use projection. Requires a resolved
// SDK::GameContext (ci must be valid).
#include "../SDK/Client/ClientInstance.h"
#include "../SDK/Client/LevelRendererPlayer.h"

// POD-only frame (SEH requires it): compares the served frame-B matrix with
// the live GameRenderer::lastViewMatrix, publishing the window stats.
static void srcDeltaCrossCheck(unsigned long long nowMs) {
    static unsigned long long s_lastFlushMs = 0;
    static float s_sum = 0.0f, s_max = 0.0f;
    static unsigned s_n = 0;
    __try {
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        if (ci && sdk_safe_ptr(ci, sizeof(void*))) {
            SDK::MinecraftGame* mcg = ci->getMinecraftGame();
            if (mcg && sdk_safe_ptr(mcg, sizeof(void*))) {
                SDK::GameRenderer* gr = mcg->getGameRenderer();
                if (gr && sdk_safe_ptr(gr, sizeof(SDK::GameRenderer))) {
                    const float* lv = gr->lastViewMatrix.m;
                    // The live matrix stores the OPPOSITE Z convention vs the
                    // writeMatrices view (srcDeltaR measured a constant 180deg
                    // on a static camera — a handedness flip, not a timing
                    // gap). Negate Z so the metric measures actual camera
                    // drift between the two sources; ~servedRot per frame is
                    // the expected healthy value.
                    const float lx = -lv[2], ly = -lv[6], lz = -lv[10];
                    const float* sv = g_servedTransform.view;
                    const float sx = sv[2], sy = sv[6], sz = sv[10];
                    const float ll = std::sqrt(lx*lx + ly*ly + lz*lz);
                    const float sl = std::sqrt(sx*sx + sy*sy + sz*sz);
                    if (ll > 1e-6f && sl > 1e-6f) {
                        float dot = (lx*sx + ly*sy + lz*sz) / (ll * sl);
                        if (dot > 1.0f) dot = 1.0f;
                        if (dot < -1.0f) dot = -1.0f;
                        const float deg = std::acos(dot) * 57.2957795f;
                        s_sum += deg;
                        if (deg > s_max) s_max = deg;
                        ++s_n;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (nowMs - s_lastFlushMs >= 2000ull) {
        g_srcDeltaAvg = s_n ? s_sum / s_n : 0.0f;
        g_srcDeltaMax = s_max;
        g_srcDeltaN = s_n;
        s_sum = 0.0f; s_max = 0.0f; s_n = 0;
        s_lastFlushMs = nowMs;
    }
}

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
            // TWO rescale rules, keyed on the push's OWN source flag (mixing
            // them across frames was the double-ESP flicker):
            //
            // fromLrp=true  — the push carries the LRP copies which already
            //   include module FOV scaling; correct only by the live/captured
            //   ratio (a late module write after the capture).
            //
            // fromLrp=false — the push carries the camera-object copies the
            //   game's writeMatrices itself stored (NEVER touched by
            //   Zoom/Sprint: both modules write the LRP elements instead).
            //   Apply the modules' own multipliers — recorded IN the push
            //   (fovScale) at capture time, so this read races nothing. The
            //   earlier live-read version disagreed with the render thread
            //   frame to frame and the overlays ghosted at the unzoomed
            //   position while zoomed.
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
                // The push carries the camera-object matrices the game's own
                // writeMatrices stored (NEVER touched by Zoom/Sprint: both
                // publish multipliers that the renderLevel hook writes into
                // the LRP projection elements). Apply the PUSH'S OWN recorded
                // module total — the exact scale the world render used on
                // these matrices. (A live read here raced the render thread
                // and showed the overlays at the wrong scale whenever zoom/
                // sprint were mid-animation.)
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
