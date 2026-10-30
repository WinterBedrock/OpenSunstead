#include "Scaffold.h"
#include "../../SDK/Client/GameIcon.h"
#include "../../Utils/RotationUtil.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Client/BlockTextures.h"
#include "../../SDK/Network/PacketUtils.h"
#include "../Setting.h"
#include "../../Core/Logger.h"
#include "../../Render/HudEditor.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <Windows.h>
#include <imgui.h>
using SDK::IVec3;
using SDK::sehGetPos;
namespace GM = SDK::GameMode;

// GetTickCount64 resolution. Any requested interval at or below this is timer
// noise rather than a rate, so the gate opens every frame instead of stuttering
// against the clock's granularity.
static const unsigned long long kScaffoldTimerGranularityMs = 16;

// Ceiling for the Speed setting, in placement passes per second.
//
// The previous ceiling was one pass per game tick (20/s), justified by "queuing
// more than one buildBlock per tick corrupts the outbound batch". That is not
// what the batch limit is: the risk is issuing many placements in a single
// pass, and the pass already bounds itself with the look-ahead count. Passes
// themselves are just calls, and the game coalesces whatever lands in a tick.
//
// 100/s keeps a sane bound (so a runaway frame rate cannot spam the server)
// while being fast enough that the bridge stays under a sprint-jumping player,
// which 20/s demonstrably was not. The Silent cap was removed together with the
// silent-swap-per-pass cost: the arm swing is now a plain AnimatePacket (no
// per-pass MobEquipment burst that tick-alignment was protecting against).
static const float kScaffoldMaxPassesPerSec = 100.0f;

// Placement reach for one placement, measured from the eye to the CLOSEST POINT
// of the clicked block's cube (see the gate comment in OnUpdate).
//
// v3 (anti-detection): the old 7.0 was measured eye -> block CENTRE, and both
// halves of that were wrong. The centre of a cube sits up to 0.87 blocks further
// away than the surface the server actually measures, and 7.0 is far past the
// vanilla pick range — so the module happily sent click transactions that the
// server itself rejects. A cancelled placement is a flag on most anticheats, not
// just a lost block. The legit range below still covers the whole sprint-jump
// arc (eye +1.25 at the apex, ~1.6 down to the bridge, 2-3 cells ahead).
static const float kScaffoldPlaceReach = 5.0f;

// Helper: is a virtual key currently held (high bit set)? Used to decide whether
// the player is actively moving (bridge forward) vs standing still (fill hole),
// and to publish the live WASD frame next to a silent aim (see scfPublishSilent).
static bool GetKeyStateDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ===========================================================================
// HUMAN AIM / PLACEMENT LAYER (anti-detection)
// ===========================================================================
//
// Everything below exists because a SERVER-side heuristic can observe it.
// Four things about the previous behaviour were machine-perfect, and each of
// them is a check that real clients die to:
//
//   1. INSTANT ROTATION. The head was written straight onto the exact angle of
//      the block in a single pass. A hand cannot do that: a view moves in a
//      series of small, quantised steps. Fixed by scfAimStep() — a
//      frame-rate-independent maximum speed plus a randomised per-step travel.
//   2. OFF-GRID ROTATION. Bedrock quantises every mouse delta by the player's
//      sensitivity, so consecutive human yaw values are whole multiples of a
//      small constant. An arbitrary float is precisely what a GCD-spread check
//      looks for. Fixed by rounding every step to kScfRotGcdDeg.
//   3. PLACING WHILE NOT LOOKING. The transaction went out regardless of where
//      the head was (the aim was queued asynchronously for the next packet), so
//      rotation and click ray in the same tick described different actions. Any
//      check that reconstructs the click from the rotation in the packet catches
//      that. Fixed by the convergence gate in OnUpdate: no placement until the
//      head actually sits on the block.
//   4. MACHINE-CLICKED POINT + UNIFORM CADENCE. The hit point was the block's
//      dead centre (identical on every placement, and not even ON the clicked
//      face), and placements came out at the pass rate — up to 100/s. Fixed by
//      scfClickPointOnFace() (random in-face point, re-used while the face stays
//      the target) and by the paced, jittered click gate.
//
// The layer is deliberately unconditional and exposes NO new setting: this
// module's settings live in Scaffold.h and are read positionally (mSettings[i]),
// so new entries may only be APPENDED there, which a .cpp-only change cannot
// do. "Speed" keeps its meaning as the pass rate and now additionally bounds the
// PLACEMENT rate inside a human window (see kScfMinPlacePerSec).

// Mouse-sensitivity step ("GCD"). Bedrock quantises every mouse-driven view
// delta by the player's sensitivity; around the default that grid is ~0.15
// degrees, so all consecutive yaw values a human produces are whole multiples of
// it. Every step taken here is rounded to the same grid, which keeps the
// module's output inside the noise floor real mouse input produces.
static const float kScfRotGcdDeg = 0.15f;

    // Fastest the head may move, in degrees per second. A human flick peaks around
    // 600 deg/s for an instant and sustained tracking is far slower. Applied to the
    // measured pass delta, so the turn looks identical at 30 and 300 fps (a
    // per-pass constant would make the aim 10x faster on a 300 fps client). 380
    // was too slow to acquire the first block of a burst while sprint-bridging -
    // the click gate below then held placements back ("places sometimes, sometimes
    // not"). 620 left the head ~1 pass behind the bridge on wide flicks (the
    // placement gate then ate the block); 700 converges within the first
    // placement window of a burst while staying inside a human flick envelope.
    static const float kScfAimDegPerSec = 700.0f;

// The head must be within this many degrees of the aim point before a placement
// is allowed out (point 3 above).
static const float kScfAimToleranceDeg = 24.0f;

// How long the placement rotation stays latched after the last placement before
// the head is eased back to the player's own view. Reference clients do the
// same (Solstice clears its scaffold rotation 500 ms after the last block):
// a head that keeps staring at the bridge for a whole session is itself a tell.
static const unsigned long long kScfAimHoldMs = 500;

// Placement pace, in blocks per second. Vanilla gates placement at 100-300 ms
// per block; a hand holding right click cannot beat that, and a burst of 20-100
// placements per second (what the pace-free version produced at high Speed) is
// the single clearest scaffold signature there is. The Speed slider still sets
// the pass rate, but the CLICK rate it may produce is clamped into this window.
// The window is tuned so a sprint-jump bridge can never outrun the placements:
// a sprinter crosses ~4.3 cells/s, so anything below ~6/s falls behind and the
// "easy to fall on defaults" report was exactly that starvation. 6-18 jittered
// is still inside what a fast legit bridger produces (10-16 clicks/s with
// bursts), while the jitter and the longer beat keep the train non-periodic.
static const float kScfMinPlacePerSec = 6.0f;
static const float kScfMaxPlacePerSec = 18.0f;

// Random in-face offset of the click point, in blocks (0.5 = the whole face).
// Large enough to look hand-picked, small enough that the crosshair is
// unambiguously on the block the transaction names.
static const float kScfClickInset = 0.26f;

// A player who notices the ground coming clicks as fast as they can; that is the
// only "burst" this module allows, and even then it is one block per click.
// The floor is what the falling path uses (55-95 ms): a player about to eat the
// bridge clicks harder than the standing pace, so the emergency gap is shorter.
static const float kScfFallPlaceGapMinMs = 55.0f;
static const float kScfFallPlaceGapMaxMs = 85.0f;

// Sprint guard: while sprinting, never place into a cell whose direction is more
// than this far from the direction the player is actually moving. Sprint is a
// state the server replicates, so "sprinting + placing the block you are running
// away from" is a pattern it can flag; a real player cannot do it with one hand.
static const float kScfBackPlaceGuardDeg = 110.0f;

// ---------------------------------------------------------------------------
// Aim / pace state
// ---------------------------------------------------------------------------

// Where the SILENT aim currently points (degrees). Default mode needs no state:
// there the LIVE head is the aim (re-read every pass), which also means a player
// moving their own mouse always wins — the module only ever adds its step.
static float g_scfAimYaw = 0.0f;
static float g_scfAimPitch = 0.0f;
static bool  g_scfAimArmed = false;     // true while a placement burst drives the head

// The click point last handed to the game. Re-used while the same face is the
// target so the crosshair does not hop between two random spots every frame —
// one hand rests on one spot for a whole bridge.
static bool  g_scfClickValid = false;
static int   g_scfClickCell[3] = { 0, 0, 0 };
static unsigned char g_scfClickFace = 0;
static Vec3  g_scfClickPoint{};

// Pacing clocks: the next moment a placement may go out, the last placement
// (drives the aim hold), and the previous pass (drives the frame-rate
// independent aim step).
static unsigned long long g_scfNextPlaceMs = 0;
static unsigned long long g_scfLastPlaceMs = 0;
static unsigned long long g_scfLastPassMs = 0;

// Per-second counters for the new skip reasons, printed on the existing Debug
// line. File-static on purpose: the header's debug members are read positionally
// elsewhere and a .cpp-only change must not touch that layout.
static int g_scfSkipPhys = 0, g_scfSkipAim = 0, g_scfSkipSprint = 0;

// SEH-guarded read of the live head rotation (degrees).
static bool scfReadRot(SDK::Actor* a, float* yawOut, float* pitchOut) {
    if (!a || !yawOut || !pitchOut) return false;
    __try {
        auto* rc = a->getRotation();
        if (!rc) return false;
        *pitchOut = rc->rotation.x;
        *yawOut   = rc->rotation.y;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Round a travel magnitude to whole sensitivity steps — never to zero, so a step
// always moves at least one mouse unit.
static float scfGridSteps(float magnitudeDeg) {
    if (magnitudeDeg <= kScfRotGcdDeg) return kScfRotGcdDeg;
    return std::floor(magnitudeDeg / kScfRotGcdDeg + 0.5f) * kScfRotGcdDeg;
}

// One mouse-sized step from `cur` toward `target` (shortest arc for yaw), or the
// target itself when it is already closer than a single grid step. Now consumes
// the module's smoothMode/maxAnglePerTick/jitter settings via RotationUtil so
// Scaffold, Aura, OreMiner and ChestAura all move identically given equal
// parameters.
static float scfHumanStep(float cur, float target, float maxStepDeg, bool isYaw,
                           const RotationUtil::Params* rp = nullptr) {
    RotationUtil::Params p = rp ? *rp : RotationUtil::Params{};
    if (p.rotationSpeed <= 0.0f) p.rotationSpeed = kScfAimDegPerSec;
    const float diff = isYaw ? angleDelta(cur, target) : (target - cur);
    const float mag  = std::fabs(diff);
    if (mag <= kScfRotGcdDeg) return target;   // on it — a hand stops there too
    // Hand speed is not constant from frame to frame. Drawing the travel from
    // 45..100% of the allowed maximum turns the perfectly linear ramp a machine
    // produced into a jittery, human-looking one — while every value stays an
    // exact multiple of the sensitivity grid.
    float travel = std::min(mag, maxStepDeg * SDK::randRange(0.45f, 1.0f));
    // smoothMode + maxAnglePerTick + jitter routed through RotationUtil.
    if (rp) {
        travel = std::min(mag, RotationUtil::stepOnce(cur, target, maxStepDeg, p, isYaw));
    }
    // Optional micro-jitter: ±jitter deg per step. Applied AFTER the base travel
    // so a no-jitter setting (0) is an exact no-op, and a jitter setting stays
    // bounded (never >2.5) so it can't pull the head past the aim.
    if (rp && rp->microJitterDeg > 0.0f) {
        const float jit = SDK::randRange(-rp->microJitterDeg, rp->microJitterDeg);
        travel = std::max(0.01f, travel + jit);
    }
    travel = scfGridSteps(travel);
    if (travel > mag) travel = mag;            // never overshoot the aim
    const float out = cur + (diff > 0.0f ? travel : -travel);
    return isYaw ? wrapAngle180(out) : out;
}

// Unit normal of a Bedrock block face: 0 down (-Y), 1 up (+Y), 2 north (-Z),
// 3 south (+Z), 4 west (-X), 5 east (+X) — the same face ids passGetPlaceFace
// emits, i.e. the face of the SOLID neighbour that looks at the target cell.
static void scfFaceNormal(unsigned char face, float* nx, float* ny, float* nz) {
    switch (face) {
        case 0:  *nx =  0.0f; *ny = -1.0f; *nz =  0.0f; break;   // click its bottom
        case 1:  *nx =  0.0f; *ny =  1.0f; *nz =  0.0f; break;   // click its top
        case 2:  *nx =  0.0f; *ny =  0.0f; *nz = -1.0f; break;   // click its north face
        case 3:  *nx =  0.0f; *ny =  0.0f; *nz =  1.0f; break;   // click its south face
        case 4:  *nx = -1.0f; *ny =  0.0f; *nz =  0.0f; break;   // click its west face
        default: *nx =  1.0f; *ny =  0.0f; *nz =  0.0f; break;   // east
    }
}

// A random point INSIDE the clicked face, kept for as long as that face stays
// the target.
//
// This is what the placement's click position is derived from, so it is the
// replacement for the machine-perfect dead centre the module used to report (the
// old code primed the hit point at the block CENTRE — identical on every
// placement, and not even lying on the clicked face). Solstice randomises
// exactly this in its own scaffold ("Click Position") and in its Disabler: every
// 0.5 component of the reported click position becomes a random in-face value.
static Vec3 scfClickPointOnFace(const IVec3& block, unsigned char face) {
    if (!g_scfClickValid || g_scfClickFace != face ||
        g_scfClickCell[0] != block.x || g_scfClickCell[1] != block.y ||
        g_scfClickCell[2] != block.z) {
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
        scfFaceNormal(face, &nx, &ny, &nz);
        float ox = SDK::randRange(-kScfClickInset, kScfClickInset);
        float oy = SDK::randRange(-kScfClickInset, kScfClickInset);
        float oz = SDK::randRange(-kScfClickInset, kScfClickInset);
        // Project the offset onto the face plane: the coordinate ALONG the face
        // normal has to stay exactly on the plane (that is the 0/1 the server
        // checks the reported click position against), only the two tangential
        // axes move.
        const float along = ox * nx + oy * ny + oz * nz;
        ox -= nx * along; oy -= ny * along; oz -= nz * along;
        g_scfClickPoint = Vec3((float)block.x + 0.5f + nx * 0.5f + ox,
                               (float)block.y + 0.5f + ny * 0.5f + oy,
                               (float)block.z + 0.5f + nz * 0.5f + oz);
        g_scfClickCell[0] = block.x;
        g_scfClickCell[1] = block.y;
        g_scfClickCell[2] = block.z;
        g_scfClickFace = face;
        g_scfClickValid = true;
    }
    return g_scfClickPoint;
}

// Distance from the eye to the CLOSEST POINT of a block's cube — the quantity a
// server-side reach check measures. A centre distance is up to 0.87 blocks
// larger, which both inflates the module's effective reach and lets through
// clicks the server rejects.
static float scfDistToBlockAabb(const Vec3& p, const IVec3& b) {
    const float dx = std::max(0.0f, std::max((float)b.x - p.x, p.x - (float)(b.x + 1)));
    const float dy = std::max(0.0f, std::max((float)b.y - p.y, p.y - (float)(b.y + 1)));
    const float dz = std::max(0.0f, std::max((float)b.z - p.z, p.z - (float)(b.z + 1)));
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// One aim step toward `aimPoint`. yaw/pitch in-out start wherever the caller's
// aim currently is (the live head in Default mode, the published latch in Silent
// mode), so both modes share the same human profile.
//
// Two details that matter:
//   * a nearly VERTICAL aim (tower / hole fill directly under the eye) has no
//     meaningful yaw — atan2(0,0) would snap the head to an arbitrary cardinal
//     direction. The yaw the player already has is kept (the same choice
//     Solstice's RotateMode::Normal makes: keep the player yaw, force only the
//     pitch down), so towering never produces a bogus sideways snap.
//   * the remaining angular error is returned, which is what the placement gate
//     in OnUpdate uses to hold the click back until the head is on the block.
static float scfAimStep(const Vec3& eye, const Vec3& aimPoint, float maxStepDeg,
                        float* yawInOut, float* pitchInOut,
                        const RotationUtil::Params* rotP = nullptr) {
    const float curYaw = *yawInOut;
    const Vec2 want = calcLookAngles(eye, aimPoint);          // x = pitch, y = yaw
    float wantYaw = want.y;
    if (vecHorizontalDistance(eye, aimPoint) < 0.35f) wantYaw = curYaw;
    *yawInOut   = scfHumanStep(curYaw, wantYaw, maxStepDeg, true, rotP);
    *pitchInOut = scfHumanStep(*pitchInOut, want.x, maxStepDeg, false, rotP);
    return std::max(std::fabs(angleDelta(*yawInOut, wantYaw)),
                    std::fabs(*pitchInOut - want.x));
}

// Apply an aim step to the LIVE head through the game's own turn pipeline.
//
// Historical note: this used to be the "Default mode" rotation writer. It
// called LocalPlayer::applyTurnDelta (the function a physical mouse drag ends
// up in) or, when the signature was unresolved, wrote rotationOld/rotation
// directly as a fallback. Both paths MOVED THE LOCAL CAMERA — scoping
// rotations to "silent" was not possible through them. Both modes now aim
// through the packet-only scfPublishSilent channel (see OnUpdate), so this
// helper is no longer invoked anywhere. Kept because the fallback comment
// documents why hand-writing the rotation pair is worse than the game's own
// applyTurnDelta — a trap that must not be re-introduced.
static void scfApplyRealHead(SDK::Actor* lp, float curYaw, float curPitch,
                             float aimYaw, float aimPitch) {
    // Intentionally a no-op: rotation goes out via scfPublishSilent only.
    // If you re-enable camera writes, re-read the header comment of this
    // function first — it is the exact camera-movement bug being fixed.
    (void)lp; (void)curYaw; (void)curPitch; (void)aimYaw; (void)aimPitch;
}

// Publish a silent rotation AND the movement frame it has to be read in.
//
// Silent mode leaves the camera alone, so the rotation the packet carries and
// the direction bits no longer describe the same motion. The send hook
// re-expresses the movement in the spoofed frame (Scaffold::ConsumeSilentMove),
// which needs the REAL view yaw the client integrated against plus the live
// WASD; without it the server lands the player somewhere the client never went
// and corrects the position — the "Scaffold shoves me off the bridge" bug.
//
// A tiny pitch sway (±0.25 deg, ~1.6 Hz) rides on every published packet. A
// head held at EXACTLY the same angle for the whole run is a textbook aimbot
// tell at high-tick servers; real hands always tremble. The sway is below the
// sensitivity quantisation of most sens settings, so it reads as noise, and it
// never perturbs the convergence gate (it is applied at publish time only).
static void scfPublishSilent(SDK::Actor* lp, float aimYaw, float aimPitch) {
    const float sway =
        std::sinf((float)GetTickCount64() * 0.0102f) * 0.25f;
    Scaffold::NotifySilentRot(aimYaw, aimPitch + sway);
    float realYaw = aimYaw, realPitch = 0.0f;
    if (!scfReadRot(lp, &realYaw, &realPitch)) realYaw = aimYaw;
    const int fwd    = (GetKeyStateDown('W') ? 1 : 0) - (GetKeyStateDown('S') ? 1 : 0);
    const int strafe = (GetKeyStateDown('D') ? 1 : 0) - (GetKeyStateDown('A') ? 1 : 0);
    Scaffold::NotifySilentMove(realYaw, fwd, strafe);
}

// Hand the head back to the player once a placement burst is over.

// Hand the head back to the player once a placement burst is over.
//
// The aim is EASED toward the live view (a hand does not teleport back up
// either) and afterwards the latch keeps mirroring the real rotation, which
// turns the send hook's rewrite into an identity: the server sees the player's
// own head between placements instead of a permanently down-looking one. That
// matters because "always looking at the bridge while sprinting" is itself a
// heuristic, independent of how the rotation got there.
static void scfReleaseSilentAim(SDK::Actor* lp, float maxStepDeg) {
    float realYaw = g_scfAimYaw, realPitch = g_scfAimPitch;
    if (!scfReadRot(lp, &realYaw, &realPitch)) return;
    if (!g_scfAimArmed) {
        // Back on the player's own view: stop publishing and let the latch go
        // stale — ConsumeSilentRot expires it within kSilentLatchTtlMs, after
        // which the send hook leaves packets fully untouched (zero
        // server-visible footprint between runs).
        return;
    }
    const float y = scfHumanStep(g_scfAimYaw, realYaw, maxStepDeg, true);
    const float p = scfHumanStep(g_scfAimPitch, realPitch, maxStepDeg, false);
    g_scfAimYaw = y;
    g_scfAimPitch = p;
    if (std::fabs(angleDelta(y, realYaw)) <= kScfRotGcdDeg &&
        std::fabs(p - realPitch) <= kScfRotGcdDeg)
        g_scfAimArmed = false;      // back on the player's own view — released
    scfPublishSilent(lp, y, p);
}

// Fix the ray half of the primed HitResult.
//
// GM::prepBlockHit() pins mRayDir to (0,-1,0) no matter which face is being
// clicked: for a side placement that is a ray that cannot have produced the hit,
// and anything that reconstructs the click from the ray (the server's own
// validation, or a plugin) sees an impossible pair. Writing the honest origin
// and direction costs nothing and is what a real crosshair ray looks like.
static void sehFixHitRay(SDK::Level* level, const Vec3& eye, const Vec3& contact) {
    if (!level) return;
    __try {
        SDK::HitResult* hr = level->getHitResult();
        if (!hr || !sdk_safe_ptr(hr, 0x60)) return;
        float dx = contact.x - eye.x, dy = contact.y - eye.y, dz = contact.z - eye.z;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len > 1e-4f) { dx /= len; dy /= len; dz /= len; }
        else { dx = 0.0f; dy = -1.0f; dz = 0.0f; }
        hr->mRayDir   = Vec3(dx, dy, dz);
        hr->mStartPos = eye;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static unsigned long long g_scfDbgLast = 0;
static void scfDbg(bool enabled, const char* fmt, ...) {
    if (!enabled) return;
    unsigned long long now = GetTickCount64();
    if (now - g_scfDbgLast < 250) return;
    g_scfDbgLast = now;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    LOG_INFO(buf, "Scaffold");
}

static bool sehGetYaw(SDK::Actor* a, float* out) {
    __try {
        auto* rc = a->getRotation();
        if (!rc) return false;
        *out = rc->rotation.y;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The two aim helpers that used to live here (sehAimAtPoint / sehSilentAimAtPoint)
// wrote the head / queued the packet rotation in ONE pass, straight onto the
// block angle. Both are replaced by the human aim layer above:
//   * sehAimAtPoint        -> scfAimStep + scfApplyRealHead  (Default mode)
//   * sehSilentAimAtPoint  -> scfAimStep + scfPublishSilent  (Silent / Rotation off)
// The single-pass version is exactly one of the four machine-perfect tells the
// header comment of that layer lists, so it is gone rather than kept as a
// fallback: a "legacy" branch would still be an instant snap whenever it ran.

// Ensure SOME hotbar slot holds a placeable block. Returns the slot to use
// without touching the player's selection: -1 when the active slot already
// holds a block, otherwise the first block slot found (or -2 when the hotbar
// has no blocks at all). Read-only placeable-block counts still fill totalOut
// for the HUD/debug path.
static int sehFindBlockSlot(SDK::Actor* lp, int* totalOut) {
    if (totalOut) *totalOut = 0;
    if (!lp) return -2;
    __try {
        SDK::PlayerInventory* inv = lp->getSupplies();
        if (!inv) return -2;
        SDK::Container* c = inv->getContainer();
        if (!c) return -2;
        int total = 0, firstBlockSlot = -1;
        for (int slot = 0; slot < 9; ++slot) {
            SDK::ItemStack* is = c->getItem(slot);
            if (is && is->isPlaceableBlock()) {
                total += is->getCount();
                if (firstBlockSlot < 0) firstBlockSlot = slot;
            }
        }
        if (totalOut) *totalOut = total;
        if (firstBlockSlot < 0) return -2; // no blocks anywhere in the hotbar
        int cur = inv->getSelectedSlot();
        SDK::ItemStack* held = (cur >= 0) ? c->getItem(cur) : nullptr;
        if (held && held->isPlaceableBlock()) return -1; // already holding a block
        return firstBlockSlot;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

// Legacy wrapper (Default mode + HUD count): full behaviour — scan the hotbar,
// count total placeable blocks, and switch to the first block slot if the held
// item isn't one. Returns false when the hotbar has no blocks.
static bool sehEnsureBlockSlot(SDK::Actor* lp, int* totalOut) {
    int slot = sehFindBlockSlot(lp, totalOut);
    if (slot == -2) return false;
    if (slot < 0) return true; // already a block in hand
    __try {
        SDK::PlayerInventory* inv = lp->getSupplies();
        if (inv) inv->setSelectedSlot(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return true;
}

// Client-side POD-only slot flip for the silent swap.
static bool sehSetSelectedSlotPOD(SDK::Actor* lp, int slot) {
    if (!lp) return false;
    __try {
        SDK::PlayerInventory* inv = lp->getSupplies();
        if (!inv) return false;
        inv->setSelectedSlot(slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Read-only placeable-block counts for the HUD (no slot side effects).
static int sehCountHotbarBlocks(SDK::Actor* lp) {
    if (!lp) return 0;
    __try {
        SDK::PlayerInventory* inv = lp->getSupplies();
        if (!inv) return 0;
        SDK::Container* c = inv->getContainer();
        if (!c) return 0;
        int total = 0;
        for (int slot = 0; slot < 9; ++slot) {
            SDK::ItemStack* is = c->getItem(slot);
            if (is && is->isPlaceableBlock()) total += is->getCount();
        }
        return total;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Silent-mode rotation handoff: OnUpdate records the placement facing it wants
// the SERVER to see; the PlayerAuthInput send hook (GameHooks) swaps it into
// the packet's rotation fields. The local first-person camera never turns —
// that is the whole point of "Silent". Writer and reader both run on the main
// game thread, so a plain bool latch is as safe as the atomics the comment in
// SilentRotation.h asks for.
static float g_silentYaw   = 0.f;
static float g_silentPitch = 0.f;
static bool  g_silentRotValid = false;
// Freshness of the latch (Solstice mShouldRotate semantics): the hook may only
// rewrite a packet while OnUpdate is ACTIVELY aiming. The moment passes stop
// publishing (no target, throttled, module idle) the latch goes stale and the
// hook leaves packets untouched — a permanently latched down-look plus stale
// WASD is what the server corrected with teleports "even when not placing".
static unsigned long long g_silentPublishMs = 0;
// Freshness horizon: passes publish every 8-70 ms while a run is active, so
// 250 ms without a publish unambiguously means "no active aim".
static constexpr unsigned long long kSilentLatchTtlMs = 250;
// Movement frame published next to the silent aim (see sehSilentAimAtPoint).
static float g_silentRealYaw = 0.f;
static int   g_silentFwd = 0;
static int   g_silentStrafe = 0;

bool Scaffold::ConsumeSilentRot(float& yaw, float& pitch) {
    // LATCHED while fresh, expiring when stale (see g_silentPublishMs). The
    // server validates a block-use transaction against the rotation from the
    // PREVIOUS PlayerAuthInput — so the aim stays latched across the packets
    // that surround each transaction. But once OnUpdate stops publishing (no
    // placement target / idle / Rotation off) the latch MUST die: rewriting
    // every packet with a stale down-look is what got corrected/teleported
    // "even when not placing" (Solstice clears its scaffold rotation 500 ms
    // after the last block for the same reason).
    if (!g_silentRotValid) return false;
    const unsigned long long now = GetTickCount64();
    if (now - g_silentPublishMs > kSilentLatchTtlMs) {
        g_silentRotValid = false;
        g_scfAimArmed = false;
        return false;
    }
    yaw = g_silentYaw;
    pitch = g_silentPitch;
    return true;
}

void Scaffold::NotifySilentRot(float yaw, float pitch) {
    g_silentYaw = yaw;
    g_silentPitch = pitch;
    g_silentRotValid = true;
    g_silentPublishMs = GetTickCount64();
}

void Scaffold::InvalidateSilentRot() {
    g_silentRotValid = false;
    g_scfAimArmed = false;
}

bool Scaffold::ConsumeSilentMove(float& realYaw, int& fwd, int& strafe) {
    if (!g_silentRotValid) return false;
    realYaw = g_silentRealYaw;
    fwd = g_silentFwd;
    strafe = g_silentStrafe;
    return true;
}

void Scaffold::NotifySilentMove(float realYaw, int fwd, int strafe) {
    g_silentRealYaw = realYaw;
    g_silentFwd = fwd;
    g_silentStrafe = strafe;
}

// ---- Solstice bridge line (Y spoof) ----------------------------------------
// The Y of the last placed block, running-min while a run stays fresh. The
// send hook reports the player STANDING on that line (never lower than the
// real reported height), so the server's movement prediction never sees the
// in-between-blocks dip that CorrectPlayerMovePrediction rewinds.
static float g_bridgeY = 0.0f;
static bool  g_bridgeValid = false;
static unsigned long long g_bridgeMs = 0;
// The bridge line retires shortly after the last placement: holding it
// forever would lift the player after they deliberately walked off the end.
static constexpr unsigned long long kBridgeTtlMs = 1200;

bool Scaffold::ConsumeYSpoof(float& eyeY) {
    if (!g_bridgeValid) return false;
    const unsigned long long now = GetTickCount64();
    if (now - g_bridgeMs > kBridgeTtlMs) {
        g_bridgeValid = false;
        return false;
    }
    // Eye height of a player standing on the placed block:
    // blockTop + 1.62 eye - a hair (never exactly on the seam).
    eyeY = std::floor(g_bridgeY) + 1.0f + 1.62f - 0.01f;
    return true;
}

void Scaffold::NotifyYBridge(float placedBlockY) {
    // Running MIN: an ascending bridge leaves the old (lower) line behind but
    // the hook only ever LIFTS the reported height, so a stale low line is
    // inert; a descending bridge follows the line down.
    if (!g_bridgeValid || placedBlockY < g_bridgeY) g_bridgeY = placedBlockY;
    g_bridgeValid = true;
    g_bridgeMs = GetTickCount64();
}

void Scaffold::ResetYBridge() {
    g_bridgeValid = false;
}

static int sehCountHeldBlocks(SDK::Actor* lp) {
    if (!lp) return 0;
    __try {
        SDK::PlayerInventory* inv = lp->getSupplies();
        if (!inv) return 0;
        SDK::Container* c = inv->getContainer();
        if (!c) return 0;
        int cur = inv->getSelectedSlot();
        if (cur < 0 || cur > 8) return 0;
        SDK::ItemStack* is = c->getItem(cur);
        if (is && is->isPlaceableBlock()) return is->getCount();
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// the reference client getBlockPlaceFace: for an AIR target cell, find a neighbour cell that
// is NOT air (solid). Returns the Bedrock face ID to click on that solid
// neighbour so the game places the new block back into `target`, and outputs the
// solid neighbour's coordinate to click.
//
// Mapping (neighbour direction -> face of the solid neighbour that faces the
// target air cell):
//   neighbour below  (0,-1,0) -> click its TOP    face = 1 (up)
//   neighbour above  (0,+1,0) -> click its BOTTOM face = 0 (down)
//   neighbour -Z     (0,0,-1) -> click its +Z     face = 3 (south)
//   neighbour +Z     (0,0,+1) -> click its -Z     face = 2 (north)
//   neighbour -X     (-1,0,0) -> click its +X     face = 5 (east)
//   neighbour +X     (+1,0,0) -> click its -X     face = 4 (west)
// (The neighbour walk itself lives in passGetPlaceFace below, which additionally
// treats blocks placed earlier in the same pass as solid.)

// Is the given cell air (placeable target)?
static bool sehIsAir(SDK::BlockSource* src, const IVec3& p) {
    if (!src) return true;
    __try { return src->isAir(SDK::BlockPos(p.x, p.y, p.z)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

// ---------------------------------------------------------------------------
// Cells placed earlier in THIS pass.
//
// This is what makes multi-block look-ahead actually work, and it is the fix for
// "Scaffold places one block at a time even at max Speed".
//
// buildBlock is a request: the block does not appear in the client's BlockSource
// until the chunk is updated, which happens after this function returns. So when
// the loop below asked "does cell k have a solid neighbour?", every neighbour it
// had just placed still read as AIR. Only the first cell adjacent to pre-existing
// geometry ever found support; cells k+1.. were skipped, and the whole pass laid
// exactly ONE block no matter what "Blocks" was set to. Sprinting outran that
// immediately, which is the stutter/gap the user sees.
//
// Treating our own just-issued placements as solid restores the support chain, so
// a pass lays the full look-ahead run the way the reference client does.
// ---------------------------------------------------------------------------
static IVec3 g_passPlaced[40];
static int   g_passPlacedCount = 0;

static void passPlacedReset() { g_passPlacedCount = 0; }

static void passPlacedAdd(const IVec3& p) {
    if (g_passPlacedCount < (int)(sizeof(g_passPlaced) / sizeof(g_passPlaced[0])))
        g_passPlaced[g_passPlacedCount++] = p;
}

static bool passPlacedHas(const IVec3& p) {
    for (int i = 0; i < g_passPlacedCount; ++i) {
        if (g_passPlaced[i].x == p.x && g_passPlaced[i].y == p.y && g_passPlaced[i].z == p.z)
            return true;
    }
    return false;
}

// isAir that also accounts for this pass's own placements.
static bool passIsAir(SDK::BlockSource* src, const IVec3& p) {
    if (passPlacedHas(p)) return false; // we already put a block there
    return sehIsAir(src, p);
}

// Support-face search that can also click a block we placed earlier this pass.
static bool passGetPlaceFace(SDK::BlockSource* src, const IVec3& target,
                             IVec3* clickBlockOut, unsigned char* faceOut) {
    static const struct { int dx, dy, dz; unsigned char face; } neigh[6] = {
        {  0, -1,  0, 1 }, // below  -> up
        {  0,  0,  1, 2 }, // +Z     -> north
        {  0,  0, -1, 3 }, // -Z     -> south
        {  1,  0,  0, 4 }, // +X     -> west
        { -1,  0,  0, 5 }, // -X     -> east
        {  0,  1,  0, 0 }, // above  -> down
    };
    for (const auto& n : neigh) {
        IVec3 np{ target.x + n.dx, target.y + n.dy, target.z + n.dz };
        if (passIsAir(src, np)) continue; // neighbour must be solid to click
        if (clickBlockOut) *clickBlockOut = np;
        if (faceOut) *faceOut = n.face;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Facing grid walk — THE THROUGHPUT / FALLING FIX.
//
// The probe used to be `target = floor(pos + lead + fwd*k)`, i.e. sample the
// facing vector once per integer step. That has two failure modes on anything
// but an exactly axis-aligned run, and both of them leave a whole pass placing
// NOTHING, which is the "slow, and I fall off as soon as I run" report:
//
//   * LANE OFFSET. The 0.5-block "lead" along the facing vector crosses a cell
//     boundary in the MINOR axis as soon as the player stands near one —
//     observed live: pos.z=-353.79 (player cell z=-354), lead=-0.23 pushed the
//     probe to floor(-354.02)=-355. The entire look-ahead then ran one block
//     parallel to the bridge, every probed cell was air-with-no-face-neighbour
//     (support skip x81, placed 0), the pass contributed nothing and the player
//     outran the bridge.
//
//   * DIAGONAL CHAIN. For a diagonal facing, consecutive sampled cells are
//     DIAGONAL to each other. Diagonal cells share no face, so
//     passGetPlaceFace can never find a block to click — only an axis-aligned
//     run ever places more than one block.
//
// This is a 2D DDA (Amanatides-Woo) over the horizontal plane at a fixed Y.
// It emits every cell the ray enters, starting from the cell the player stands
// in. Two properties make it the right tool:
//   * it starts ON the player's own cell, so the probe can never leave the
//     bridge's lane — no offset, no parallel run;
//   * on a diagonal it also emits the CORNER cell the ray clips, so consecutive
//     emitted cells are always FACE-adjacent and each new block has somewhere
//     to be clicked against.
// ---------------------------------------------------------------------------
struct FacingWalk {
    int x = 0, z = 0;
    int stepX = 0, stepZ = 0;
    double tMaxX = 0.0, tMaxZ = 0.0;
    double tDeltaX = 0.0, tDeltaZ = 0.0;
};

// Begin a walk from (x, z) in unit direction (dx, dz). Both must be finite;
// a zero component simply never advances that axis.
static void walkStart(FacingWalk& w, double x, double z, double dx, double dz) {
    w.x = (int)std::floor(x);
    w.z = (int)std::floor(z);
    w.stepX = (dx > 0.0) ? 1 : ((dx < 0.0) ? -1 : 0);
    w.stepZ = (dz > 0.0) ? 1 : ((dz < 0.0) ? -1 : 0);
    // t = distance along the ray. Cell width is 1 / |dir| in t, and tMax is the
    // distance to the first grid line we cross on each axis. An idle axis gets
    // +inf so it never wins the advance comparison.
    w.tDeltaX = (dx != 0.0) ? 1.0 / std::fabs(dx) : 1e300;
    w.tDeltaZ = (dz != 0.0) ? 1.0 / std::fabs(dz) : 1e300;
    w.tMaxX = (dx > 0.0) ? ((double)(w.x + 1) - x) * w.tDeltaX
              : (dx < 0.0) ? ((double)x - w.x) * w.tDeltaX : 1e300;
    w.tMaxZ = (dz > 0.0) ? ((double)(w.z + 1) - z) * w.tDeltaZ
              : (dz < 0.0) ? ((double)z - w.z) * w.tDeltaZ : 1e300;
}

// Emit the current cell, then advance to the next. Returns false once the walk
// can advance no further (both axes idle).
static bool walkNext(FacingWalk& w, int* cx, int* cz) {
    *cx = w.x;
    *cz = w.z;
    if (!w.stepX && !w.stepZ) return false;
    // The tiny X bias is deliberate: on an exact 45-degree ray tMaxX == tMaxZ
    // every step and a naive tie-break would jump diagonally forever (the very
    // diagonal-chain bug this walk exists to fix). Biasing toward X makes the
    // walk step X then Z, so the corner cell between them is emitted and every
    // consecutive pair of cells stays face-adjacent.
    if (w.tMaxX - 1e-7 < w.tMaxZ) {
        w.x += w.stepX;
        w.tMaxX += w.tDeltaX;
    } else {
        w.z += w.stepZ;
        w.tMaxZ += w.tDeltaZ;
    }
    return true;
}

void Scaffold::OnUpdate() {
    if (!IsEnabled()) return;

    // Telemetry only fires when the user turns the Debug setting on — there is
    // no unconditional enable-window logging any more.
    const bool dbg = GetDebug();
    const bool silentMode = (GetType() == 1);
    // "Rotation" is the on/off switch for the aim: OFF means the module places
    // with the player's own head and never touches an outgoing packet (the
    // toggle was previously read nowhere, so rotations leaked even with it
    // off). Rotations are ALWAYS packet-only — no camera writes ever happen.
    const bool wantRot = GetRotation();
    if (!wantRot) InvalidateSilentRot();

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx, false)) {
        scfDbg(dbg, "ctx resolve FAIL");
        return;
    }
    SDK::ClientInstance* ci = ctx.ci;
    SDK::Actor* lp = ctx.lp;

    Vec3 pos{};
    if (!sehGetPos(lp, &pos)) {
        scfDbg(dbg, "getPos FAIL");
        return;
    }

    // RATE GATE — wall-clock, in placement passes per second.
    //
    // This used to gate on Actor::ticksExisted, i.e. at most ONE pass per 20 Hz
    // game tick. That is the "even at max Speed it is slower than vanilla"
    // complaint, and it was a hard ceiling the Speed slider could not lift:
    // at its maximum the stride was already 1, so the setting had nothing left
    // to give. Two separate costs came out of it:
    //
    //   * 50 ms of dead time between passes. A player manually spamming
    //     right-click places faster than that, which is exactly what was
    //     observed.
    //   * When NOT bridging the look-ahead is 1, so one tick really did mean
    //     one block — the pass could not amortise the wait over several cells.
    //
    // Nothing about buildBlock actually requires tick alignment; the reason the
    // tick gate existed was to avoid stuffing one outbound batch with placements.
    // A millisecond gate achieves that just as well while letting the slider
    // reach genuinely fast rates, so Speed is now what it claims to be: passes
    // per second, up to kScaffoldMaxPassesPerSec.
    // The old Silent-mode cap (speed <= 20) is gone: silent mode no longer does
    // a MobEquipment pair per pass, which was the batch load that cap existed
    // for. The look-ahead below still bounds placements per pass.
    float speed = GetSpeed();
    if (speed < 1.0f) speed = 1.0f;
    if (speed > kScaffoldMaxPassesPerSec) speed = kScaffoldMaxPassesPerSec;

    // ---- FOOTING SAFETY (the reference client's "never fall") --------------
    // Is the cell the player is standing on / about to land on a HOLE? When it
    // is, this pass is an emergency: it runs regardless of the pass pacing,
    // the click window opens immediately, and the aim gate lets the footing
    // click through with the aim still travelling (the aim IS published
    // first, in the same batch — the reference scaffolds do exactly this).
    // This is what makes the module unable to drop the player while
    // sprint-bridging on default settings.
    bool footingVoid = false;
    {
        const int feetCellNow = (int)std::floor(pos.y - 1.62f + 0.001f);
        const int floorY = mBridgeLevelValid ? mBridgeLevel : feetCellNow - 1;
        // Only while the player hugs the bridge plane: high above it (a
        // tower jump) or far below (already fell through) must not trigger
        // the emergency path.
        if (std::fabs(pos.y - (float)(floorY + 1 + 1.62f)) < 1.3f) {
            SDK::BlockSource* bs = ci->getBlockSource();
            if (bs) {
                footingVoid = sehIsAir(bs, IVec3{ (int)std::floor(pos.x), floorY,
                                                 (int)std::floor(pos.z) });
                // Diagonal assist: ALSO probe the cell the player is ABOUT to
                // step into (velocity lead). A diagonal sprint crosses into
                // the next cell while the current one still has support, so
                // the emergency must fire one cell EARLY there.
                if (!footingVoid && GetDiagonal()) {
                    Vec3 v{};
                    __try {
                        ::StateVectorComponent* sv = lp->getStateVector();
                        if (sv) v = sv->velocity;
                    } __except (EXCEPTION_EXECUTE_HANDLER) { v = Vec3(); }
                    const float h2 = v.x * v.x + v.z * v.z;
                    if (h2 > 0.0025f) {
                        const float inv = 0.42f / std::sqrt(h2);   // ~75 ms lead
                        const int ax = (int)std::floor(pos.x + v.x * inv);
                        const int az = (int)std::floor(pos.z + v.z * inv);
                        if (ax != (int)std::floor(pos.x) || az != (int)std::floor(pos.z))
                            footingVoid = sehIsAir(bs, IVec3{ ax, floorY, az });
                    }
                }
            }
        }
    }

    {
        const unsigned long long nowGate = GetTickCount64();
        // GetTickCount64 has ~15 ms granularity, so a computed interval below
        // that would gate on timer noise rather than on the requested rate.
        // Clamping to 0 there lets every frame through, which IS the intent at
        // the top of the slider.
        unsigned long long passIntervalMs = (unsigned long long)(1000.0f / speed);
        if (passIntervalMs <= kScaffoldTimerGranularityMs) passIntervalMs = 0;
        // Jitter the pass spacing by +-10%. The pass rate itself is not
        // observable, but it drives the placement gate below; two independently
        // jittered clocks are what keeps the outbound cadence from being
        // periodic. An exactly even 1/rate spacing is a signature on its own.
        if (passIntervalMs) {
            passIntervalMs = (unsigned long long)((float)passIntervalMs *
                                                  SDK::randRange(0.9f, 1.1f));
            if (passIntervalMs == 0) passIntervalMs = 1;
        }
        if (passIntervalMs && !footingVoid && nowGate - mLastPassMs < passIntervalMs) return;
        mLastPassMs = nowGate;
    }

    void** vt = nullptr;
    void* gm = GM::resolve(lp, &vt);
    if (!gm || !vt) {
        scfDbg(dbg, "GameMode resolve FAIL (lp=%p)", (void*)lp);
        return;
    }
    SDK::BlockSource* blockSource = ci->getBlockSource();
    SDK::Level* level = nullptr;
    {
        SDK::Minecraft* mc = ci->getMinecraft();
        if (mc && sdk_safe_ptr(mc, 0xC0)) level = mc->getLevel();
    }

    // ---- PLACEMENT PACE (human click gate) --------------------------------
    //
    // "Speed" has always been the PASS rate (how often this function may look at
    // the world), never the click rate — but until now nothing bounded the
    // actual placement rate, so a high Speed produced 20-100 ItemUse
    // transactions per second. Nothing a hand can do looks like that: vanilla
    // itself gates placement at 100-300 ms per block, and servers/anticheats
    // track exactly this rate. The window the slider may reach is therefore
    // clamped to a human one (4-11 blocks/s, jittered per click, with the
    // occasional longer beat), and a pass only ever issues ONE click.
    //
    // A player who notices they are falling is the one exception: they click as
    // fast as they can, so a fall shortens the next gap instead of queuing a
    // burst of blocks into the same tick.
    const unsigned long long now = GetTickCount64();
    // Footing emergency (see the safety block above): the click window is
    // forced open so the hole under the feet closes THIS pass.
    const bool placeWindowOpen = footingVoid || (now >= g_scfNextPlaceMs);

    // Per-pass aim budget, in degrees. Frame-rate independent (a constant
    // per-pass step would make the head 10x faster on a 300 fps client), with a
    // hitch guard so a stalled frame cannot whip the view around.
    float dtSec = 1.0f / 60.0f;
    if (g_scfLastPassMs && now > g_scfLastPassMs) {
        dtSec = (float)(now - g_scfLastPassMs) / 1000.0f;
        if (dtSec > 0.10f) dtSec = 0.10f;
    }
    g_scfLastPassMs = now;
    float aimStepDeg = dtSec * kScfAimDegPerSec;
    if (aimStepDeg < kScfRotGcdDeg) aimStepDeg = kScfRotGcdDeg;

    int blockCount = 0;
    // Swap-on-place slot handling. When "Build" (auto-pick) is ON, the block
    // slot is auto-resolved from the hotbar and flipped around the placement.
    // When "Build" is OFF, only the currently-held item is used: if it is not
    // a placeable block, nothing happens (no auto-pick, no placement).
    const bool autoPick = GetBuild();
    int silentSwapSlot = -2;
    int silentPrevSlot = -1;
    {
        if (autoPick) {
            silentSwapSlot = sehFindBlockSlot(lp, &blockCount);
        } else {
            // Auto-pick off: examine the active slot only; if it doesn't hold
            // a placeable block, we have nothing to place with.
            __try {
                SDK::PlayerInventory* inv = lp->getSupplies();
                SDK::Container* c = inv ? inv->getContainer() : nullptr;
                int cur = inv ? inv->getSelectedSlot() : -1;
                SDK::ItemStack* held = (c && cur >= 0 && cur <= 8) ? c->getItem(cur) : nullptr;
                if (held && held->isPlaceableBlock()) {
                    silentSwapSlot = -1;   // already holding
                } else {
                    silentSwapSlot = -2;   // no placeable block in hand
                }
                blockCount = 0;
                if (c) for (int slot = 0; slot < 9; ++slot) {
                    SDK::ItemStack* is = c->getItem(slot);
                    if (is && is->isPlaceableBlock()) blockCount += is->getCount();
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { silentSwapSlot = -2; }
        }
        if (silentSwapSlot == -2) {
            scfDbg(dbg, "no placeable block in hand / hotbar (build=%d) — skip", (int)autoPick);
            return;
        }
        if (silentSwapSlot >= 0 && placeWindowOpen && autoPick) {
            __try {
                SDK::PlayerInventory* inv = lp->getSupplies();
                silentPrevSlot = inv ? inv->getSelectedSlot() : -1;
            } __except (EXCEPTION_EXECUTE_HANDLER) { silentPrevSlot = -1; }
            if (!sehSetSelectedSlotPOD(lp, silentSwapSlot)) return;
            if (silentSwapSlot != mServerSlot) {
                if (SDK::PacketUtils::sendMobEquipment(lp, silentSwapSlot))
                    mServerSlot = silentSwapSlot;
            }
        } else if (silentSwapSlot < 0) {
            int cur = -1;
            __try {
                SDK::PlayerInventory* inv = lp->getSupplies();
                cur = inv ? inv->getSelectedSlot() : -1;
            } __except (EXCEPTION_EXECUTE_HANDLER) { cur = -1; }
            if (cur >= 0 && cur != mServerSlot) mServerSlot = cur;
        } else {
            // Block slot found but placement is throttled this pass (or Build
            // is off and we reused the held slot): skip the flip.
        }
    }

    // classic placement:
    //   baseY  = stand-block cell (eye - height - 1)
    //   target = air cell under / ahead of the player at baseY
    //   click  = solid neighbour of target, face toward the air
    //   prime HitResult as BLOCK, then buildBlock(click, face, true)
    //
    // BRIDGE LEVEL PINNING — the throughput fix.
    //
    // baseY used to be recomputed from the live eye position every pass:
    // floor(pos.y - 1.62 - 1). While SPRINT-JUMPING the eye rises ~1.25 blocks
    // at the apex, so floor() flipped baseY up one level mid-jump and back on
    // landing. Every pass in the airborne phase then probed the WRONG level:
    // targets landed in the air column above the bridge (no support -> with
    // neighbour-only placement nothing sent), or inside the bridge slab (not
    // air -> skipped), so the pass placed nothing for most of the jump cycle —
    // the "still slow, I outrun it and fall" behaviour. The reference client
    // pins the build level (startY) and keeps it while the player is airborne.
    //
    // Grounded detection v2: the vertical velocity PLUS a still-frames
    // counter. The first revision followed the feet level whenever |vy| < 0.3,
    // but a jump ARC passes through vy ≈ 0 at its apex (~2 frames) — so the
    // pin snapped UP to the air cell above the bridge mid-jump and right back,
    // flipping the probe level exactly during sprint-jumps. Now the feet level
    // is followed only after ~4 consecutive near-still passes: standing gives
    // many, a jump apex gives at most 2-3 (|vy| sweeps 0.42 -> -0.42 at
    // ~0.08/tick), so an apex can never move the pin.
    const float PLAYER_HEIGHT = 1.62f;
    const int feetCell = (int)std::floor(pos.y - PLAYER_HEIGHT + 0.001f);
    // Live velocity, read ONCE: the level pin below uses its Y, and the two
    // placement plausibility guards (sprint direction, falling click rate) use
    // the horizontal components.
    Vec3 vel{};
    __try {
        ::StateVectorComponent* sv = lp->getStateVector();
        if (sv) vel = sv->velocity;
    } __except (EXCEPTION_EXECUTE_HANDLER) { vel = Vec3(); }
    // "Falling" is strictly downward motion, not "airborne": a jump's rising
    // half must not unlock the fast click rate.
    const bool falling = (vel.y < -0.30f);
    {
        const float vy = vel.y;
        if (!mBridgeLevelValid) {
            mBridgeLevel = feetCell - 1;   // first pass: assume current level
            mBridgeLevelValid = true;
            mStillFrames = 0;
        } else if (vy > -0.1f && vy < 0.1f) {
            if (++mStillFrames >= 4) mBridgeLevel = feetCell - 1;
        } else {
            mStillFrames = 0;              // airborne / falling: keep the pin
        }
    }
    int baseY = mBridgeLevel;
    int px = (int)std::floor(pos.x);
    int pz = (int)std::floor(pos.z);

    // Facing as a CONTINUOUS unit vector, not a quantised cardinal direction.
    //
    // This is the fix for "Scaffold cannot keep up when I run and jump".
    //
    // The old code collapsed the look direction to whichever axis dominated
    // (`dirX = ±1` OR `dirZ = ±1`) and then stepped the look-ahead along that one
    // axis. Bridging at any angle other than exactly N/S/E/W therefore probed a
    // line that DIVERGES from where the player is actually travelling: at 45° the
    // probe walks along X while the player moves diagonally, so after two cells
    // the probe is off the player's path entirely, finds solid ground or no
    // support, and the pass places nothing. The result is the observed behaviour —
    // it only lays a block once the player has already walked over the edge, one
    // at a time, always a step behind.
    //
    // the reference client (Scaffold::getRotBasedPos) instead advances a float position along
    // the yaw vector and floors it per step, which is what this now does. A
    // 45° run then probes the true diagonal, each step is a cell the player is
    // genuinely about to occupy, and the whole look-ahead run gets placed in one
    // pass.
    float yaw = 0.0f;
    float fwdX = 0.0f, fwdZ = 0.0f;
    const bool moving = (GetKeyStateDown('W') || GetKeyStateDown('A') ||
                         GetKeyStateDown('S') || GetKeyStateDown('D'));
    if (GetBridge() && moving && sehGetYaw(lp, &yaw)) {
        // Bedrock yaw: 0 = -Z, growing clockwise. the reference client uses (yaw+90) in
        // radians with cos->X, sin->Z, which is the same basis as this.
        const float D2R = 0.01745329252f;
        const float r = (yaw + 90.0f) * D2R;
        fwdX = std::cos(r);
        fwdZ = std::sin(r);
    }
    const bool bridging = (fwdX != 0.0f || fwdZ != 0.0f);

    // Per-step place budget. In Bridge mode the user's "Blocks" setting governs
    // exactly how many cells the run places ahead each pass — the old forced
    // kScaffoldProbeMin(9) bump ignored Blocks and dropped a huge run per step
    // ("places too many blocks"). Placement speed itself comes from the gm+0x88
    // cooldown bypass, so the cap simply limits reach, it no longer gates
    // throughput. Non-bridging (tower/hole-fill) keeps one under the feet.
    int count = bridging ? GetPlaces() : 1;
    if (count < 1) count = 1;
    if (count > 32) count = 32;

    // The probe is normally a DDA grid walk from the player's own cell. With
    // "Clutch" on and the player falling it instead walks a 5x5x5 candidate
    // list sorted by distance from the feet (Horion's tryClutchScaffold),
    // because the DDA line can't help once you've fallen past the bridge.
    FacingWalk walk;
    bool useClutch = falling && GetClutch();
    static IVec3 g_clutchSorted[125];   // 5^3
    static int    g_clutchCount = 0;
    if (useClutch) {
        g_clutchCount = 0;
        int cx = (int)std::floor(pos.x);
        int cz = (int)std::floor(pos.z);
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dz = -2; dz <= 2; ++dz) {
                    IVec3 v{ dx, dy, dz };
                    // sorted by 3D distance by precomputed index; simple bubble
                    // once per pass is still free (125 items).
                    g_clutchSorted[g_clutchCount++] = v;
                }
        // insertion-sort by squared distance (small n)
        for (int i = 1; i < g_clutchCount; ++i) {
            IVec3 key = g_clutchSorted[i];
            int k2 = key.x*key.x + key.y*key.y + key.z*key.z;
            int j = i - 1;
            while (j >= 0 && (g_clutchSorted[j].x*g_clutchSorted[j].x
                             + g_clutchSorted[j].y*g_clutchSorted[j].y
                             + g_clutchSorted[j].z*g_clutchSorted[j].z) > k2) {
                g_clutchSorted[j + 1] = g_clutchSorted[j];
                --j;
            }
            g_clutchSorted[j + 1] = key;
        }
    } else if (bridging) walkStart(walk, pos.x, pos.z, fwdX, fwdZ);

    bool anyOk = false;
    int placed = 0;
    int lastX = 0, lastY = 0, lastZ = 0;

    // Plausibility guards, resolved once per pass (see the per-cell comments
    // below): sprint state (only trusted when the flag component is readable),
    // whether the player is actually moving, and the direction they are moving
    // in — Bedrock yaw, where forward = (-sin yaw, 0, cos yaw), so the yaw of a
    // velocity vector is atan2(-vx, vz).
    int  sprintFlag = -1;
    __try { sprintFlag = lp->getStatusFlag(ActorFlags::Sprinting); }
    __except (EXCEPTION_EXECUTE_HANDLER) { sprintFlag = -1; }
    const bool  sprinting  = (sprintFlag == 1);
    const bool  movingFast = ((vel.x * vel.x + vel.z * vel.z) > 0.0025f);
    const float moveYaw    = std::atan2(-vel.x, vel.z) * 57.2957795f;

    // Reach gate v3 — measured EYE -> CLOSEST POINT of the clicked cube.
    //
    // v1 (pick range from the eye to the cell) stalled the run during the
    // sprint-jump apex. v2 (7 blocks to the block CENTRE) over-corrected: it
    // both over-measured by up to 0.87 blocks and exceeded the vanilla pick
    // range, so part of every burst was transactions the server rejects. The
    // closest-point metric is the quantity a reach check actually computes and
    // it beats the old 7.0-block centre metric while staying inside the legit
    // range: at the apex the bridge surface below-front is ~3.5-4.5 away.
    const float reach = kScaffoldPlaceReach;

    // the reference client loops `places` times, each placing the next air hole. We do the
    // same: walk k = 0..count, skip solid cells, place every air cell that has
    // a solid neighbour (so each successive block can support the next).
    // Fresh ledger every pass: placements only count as support within the pass
    // that issued them, since by the next pass the real BlockSource has caught up.
    passPlacedReset();

    // One hand click per pass. The walk below still runs the whole look-ahead
    // (it is how the nearest placeable cell is found, and how a cell placed
    // earlier in the pass can support the next one), but the click rate itself
    // is what the pace gate at the top of OnUpdate owns.
    // FALLING is the exception: at the emergency click pace (~70 ms gap) one
    // block per pass still cannot out-place gravity from a jump apex — the
    // "very easy to fall" defect. Two blocks in the emergency pass close the
    // hole now AND the next, which is exactly what a panicking hand does.
    const int burstCap = falling ? 2 : 1;
    int placedThisPass = 0;

    for (int k = 0; k < count; ++k) {
        // Target the next candidate. Falls into the Clutch 5^3 scan while
        // falling (if Clutch is on), otherwise follows the DDA walk.
        IVec3 target;
        if (useClutch && g_clutchCount > 0 && k < g_clutchCount) {
            IVec3 off = g_clutchSorted[k];
            target = IVec3{ px + off.x, baseY + off.y, pz + off.z };
        } else if (bridging) {
            int cx = px, cz = pz;
            if (!walkNext(walk, &cx, &cz)) break;
            target = IVec3{ cx, baseY, cz };
        } else {
            target = IVec3{ px, baseY, pz };
        }

        // The walk never revisits a cell, but keep the guard: a tower-mode pass
        // re-enters OnUpdate many times a second and the pass ledger is
        // per-pass, so a repeat here still wastes look-ahead budget.
        if (k > 0 && passPlacedHas(target)) { ++mDbgSkipRepeat; if (useClutch) continue; }

        if (blockSource && !passIsAir(blockSource, target)) {
            ++mDbgSkipSolid;
            if (bridging || useClutch) continue;
            break;
        }

        IVec3 clickBlock = target;
        unsigned char face = 1;
        // NEIGHBOUR-ONLY placement (server-safe): the outbound InventoryTrans-
        // action carries the position of the block being CLICKED and the face.
        // The server replays its block-use math against that neighbour; if the
        // neighbour is air the placement is invalid and most anticheats/server
        // implementations silently CANCEL the transaction (or flag it), so the
        // block never appears and the pass was burned. The old fallback here
        // ("no support found -> click the air cell below with face=1") did
        // exactly that, which is why batches sometimes poured into thin air.
        //
        // Neighbours placed EARLIER IN THIS PASS count as solid (passGetPlace-
        // Face consults the passPlaced ledger), so a look-ahead run chains
        // block-on-block just like the reference client.
        IVec3 support{};
        unsigned char supportFace = 0;
        bool haveSupport = false;
        if (blockSource) {
            haveSupport = passGetPlaceFace(blockSource, target, &support, &supportFace);
        }
        if (!haveSupport) {
            // No solid neighbour: send nothing for this cell. While bridging
            // the next look-ahead cell may have support; standing still (tower
            // / hole fill) there is nothing to build on at all, so stop.
            ++mDbgSkipSupport;
            if (bridging) continue;
            break;
        }
        clickBlock = support;
        face = supportFace;

        // ---- placement plausibility ----------------------------------------
        // Reach: eye -> closest point of the SOLID block being clicked, the
        // quantity the server's own check measures (`pos` is the eye height on
        // the local player — the same reference the aim uses).
        const float dist = scfDistToBlockAabb(pos, clickBlock);
        if (dist > reach) {
            ++mDbgSkipReach;
            if (bridging) continue;
            break;
        }
        // A block can only be clicked from OUTSIDE it. The old centre-distance
        // gate let through cells the player's own eye was inside, which is an
        // impossible click and a guaranteed server reject.
        if (dist <= 0.0f) {
            ++g_scfSkipPhys;
            if (bridging) continue;
            break;
        }
        // The cell the block would LAND in must not be the space the player is
        // occupying right now — a block inside your own body is physically
        // impossible and is exactly what a "place under self" check looks for.
        if (target.y >= feetCell && target.y <= feetCell + 1 &&
            target.x == px && target.z == pz) {
            ++g_scfSkipPhys;
            if (bridging) continue;
            break;
        }

        // The click point: a random spot INSIDE the clicked face, re-used while
        // that face stays the target (see scfClickPointOnFace). Everything below
        // — the aim, the primed HitResult, and therefore the click position the
        // transaction reports — is derived from this ONE point, so the packet is
        // internally consistent and per-placement different, like a hand.
        const Vec3 contact = scfClickPointOnFace(clickBlock, face);

        // Sprint guard: never place the block the sprinting player is running
        // away from. Sprint is replicated server-side, so "sprinting + placing
        // behind yourself" is a pattern a state check can flag; no single hand
        // produces it. Skipping the placement (instead of dropping the sprint)
        // keeps the movement the server simulates untouched — a sprint-flag
        // fight would desync the speed instead, which is worse.
        if (sprinting && movingFast && bridging) {
            const float blockYaw = calcLookAngles(pos, contact).y;
            if (std::fabs(angleDelta(moveYaw, blockYaw)) > kScfBackPlaceGuardDeg) {
                ++g_scfSkipSprint;
                break;
            }
        }

        // ---- human rotation ------------------------------------------------
        // TWO different things are checked here, in this order:
        //   1. whether the head has ARRIVED at the block yet (aim gate), and
        //   2. whether this pass is allowed to CLICK at all (pace gate).
        // Both must hold before a transaction is allowed out; the old code
        // checked neither and simply fired the click with whatever rotation the
        // packet happened to carry.
        //
        // The aim runs on EVERY pass that found a target — open click-window
        // or not. A hand looks first and clicks later, and publishing every
        // pass is what keeps the send-hook latch fresh for the whole run
        // (ConsumeSilentRot expires a latch with no publish, so aiming only
        // inside the click window would let it die between slow clicks and
        // the server would cancel the placement against the un-aimed head).

        // RotationUtil::Params resolved from the settings: rotationSpeed (#17),
        // maxAnglePerTick (#18), smoothMode (#19), predictPlacement (#20),
        // jitter (#21). Compiled ONCE per pass.
        RotationUtil::Params rotP;
        rotP.rotationSpeed = GetRotationSpeed();
        rotP.maxAnglePerTick = GetMaxAnglePerTick();
        rotP.smoothMode = (int)GetSmoothMode();
        rotP.predictPlacementAngle = GetPredictPlacement();
        rotP.microJitterDeg = GetJitter();
        if (rotP.rotationSpeed <= 0.0f) rotP.rotationSpeed = kScfAimDegPerSec;

        bool aimOnBlock = false;
        // BOTH modes are silent now: the aim never touches the local camera.
        // The previous Default branch wrote the head through applyTurnDelta /
        // ActorRotationComponent, which visibly snapped the player's view toward
        // the bridge — exactly the camera movement this module is not allowed to
        // make. Routing every aim through the same latched publish path Aura /
        // OreMiner use keeps the rotation server-side only: the outgoing
        // PlayerAuthInput carries the aimed yaw/pitch (ConsumeSilentRot on the
        // send hook) while first-person camera stays mouse-driven. GetRotation()
        // remains the on/off switch for the aim; it no longer chooses between
        // "camera writes" and "packet only".
        //
        // Rotation OFF: place with the player's own head — no aim, no publish,
        // the latch was already invalidated at the top of OnUpdate so the hook
        // leaves every packet alone.
        if (!wantRot) {
            aimOnBlock = true;
        } else
        {
            if (!g_scfAimArmed) {
                // New burst: the chain starts where the head actually is, so the
                // first step is a normal mouse-sized move and not a jump.
                float hy = 0.0f, hp = 0.0f;
                if (scfReadRot(lp, &hy, &hp)) { g_scfAimYaw = hy; g_scfAimPitch = hp; }
            }
            float cy = g_scfAimYaw, cp = g_scfAimPitch;
            // When Predict Placement is on, the aim point is biased toward the
            // placement surface normal so convergence lands steady on the face
            // (see RotationUtil::adjustPlacementPredict).
            Vec3 adjContact = rotP.predictPlacementAngle
                ? RotationUtil::adjustPlacementPredict(pos, contact, face)
                : contact;
            const float err = scfAimStep(pos, adjContact, aimStepDeg, &cy, &cp, &rotP);
            g_scfAimYaw = cy;
            g_scfAimPitch = cp;
            g_scfAimArmed = true;
            scfPublishSilent(lp, cy, cp);   // camera untouched, packet aimed
            aimOnBlock = (err <= kScfAimToleranceDeg);
        }
        if (!aimOnBlock && !footingVoid) {
            // The crosshair is still travelling toward the block. Sending the
            // click now would put a rotation and a click ray into the same tick
            // that describe different actions — the check most scaffolds are
            // actually caught by. A human also waits for the head before
            // clicking; the next pass is ~8-16 ms away. (The aim above already
            // published, so the head keeps travelling and the latch stays fresh.)
            // EXCEPTION: the footing emergency above closes the hole under the
            // feet even with the aim still mid-flight — the aim WAS published
            // this pass, so the packet that carries the transaction is already
            // aimed; that is exactly how the reference scaffold never drops
            // the player.
            ++g_scfSkipAim;
            break;
        }
        if (!placeWindowOpen) break;   // hand has not clicked again yet

        // HitResult priming is required for reliable server placement (the
        // reference client). The hit POINT is the same random in-face contact
        // point the head is now on, never the block centre (see
        // scfClickPointOnFace), and the ray is repaired to match so the primed
        // hit describes one real crosshair ray.
        GM::prepBlockHit(level, clickBlock, face, contact.x, contact.y, contact.z);
        sehFixHitRay(level, pos, contact);

        if (silentMode) {
            // Vanilla ordering: the arm-swing AnimatePacket goes out BEFORE
            // the block-use transaction, inside the same outbound batch.
            // Sending it after (the old order) left servers that require a
            // pre-use swing cancelling the placement.
            SDK::PacketUtils::sendSwing(lp);
        }

        // Clear the engine's built-in placement cooldown (gm+0x88, set inside
        // GameMode::buildBlock to ~100-300ms per placement). Vanilla places once
        // per click so the cooldown never binds; the module issues its own
        // clicks and without this the engine would swallow roughly half of them
        // — the "slow, and I fall while sprinting" symptom. This is a CLIENT
        // field: it is not sent anywhere, and the pace that actually reaches the
        // server is now the human one enforced above (kScfMin/MaxPlacePerSec).
        __try {
            *(volatile unsigned long long*)((uintptr_t)gm + ::Layout::GameMode::BuildCooldownLastNs) = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (GM::buildBlock(gm, vt, clickBlock, face)) {
            anyOk = true;
            ++placed;
            ++placedThisPass;
            lastX = target.x; lastY = target.y; lastZ = target.z;
            // Record it so the NEXT cell in this same pass can use it as support
            // (the client's BlockSource will not show it until after this frame).
            passPlacedAdd(target);
            // Bridge line for the Y spoof (Solstice bypass, see ConsumeYSpoof).
            NotifyYBridge((float)target.y);

            // ---- schedule the next human click ---------------------------------
            // A hand does not click at the pass rate. The interval is drawn per
            // click and jittered, with an occasional longer beat: an exactly
            // periodic click train is as unnatural as a 100 Hz one, and both are
            // trivial for a timing check to fingerprint.
            g_scfLastPlaceMs = now;
            unsigned long long gap;
            if (falling) {
                // Falling: a player clicks as fast as they can to save
                // themselves — still one block per click, just a shorter gap.
                gap = (unsigned long long)SDK::randRange(kScfFallPlaceGapMinMs,
                                                         kScfFallPlaceGapMaxMs);
            } else {
                float rate = speed;                     // "Speed" = the user's pace knob
                if (rate < kScfMinPlacePerSec) rate = kScfMinPlacePerSec;
                if (rate > kScfMaxPlacePerSec) rate = kScfMaxPlacePerSec;
                gap = (unsigned long long)(1000.0f / rate * SDK::randRange(0.80f, 1.30f));
                if (SDK::randUnit() < 0.10f)            // nobody keeps perfect time
                    gap += (unsigned long long)SDK::randRange(30.0f, 110.0f);
            }
            if (gap < 1) gap = 1;
            g_scfNextPlaceMs = now + gap;
        } else {
            ++mDbgBuildFail;
        }
        // One click per hand. The look-ahead above (count) decides WHICH cell is
        // clicked, never how many clicks go out in one pass.
        if (!bridging) break;
        if (placedThisPass >= burstCap) break;
    }

    // Slot flip teardown (BOTH modes): put the original slot back CLIENT-SIDE
    // only. The server still believes the block slot is selected (serverhold)
    // — no MobEquipment restore. The next real (vanilla) slot switch informs
    // the server on its own, and the next pass re-syncs mServerSlot.
    if (silentSwapSlot >= 0) {
        if (silentPrevSlot >= 0) {
            sehSetSelectedSlotPOD(lp, silentPrevSlot);
        }
    }
    if (anyOk) mLastPlace = now;

    // ---- rotation release --------------------------------------------------
    // Unconditional now (both modes inject silently). The aim is handed back to
    // the player only when the bridge has genuinely STOPPED: no placement for
    // kScfAimHoldMs AND the player is no longer moving in bridge mode. While
    // the run is active the head stays down and micro-tracks the next block —
    // which is what a real bridger's head does too. The old per-placement
    // release (hold window alone, 260 ms) fired BETWEEN blocks at the human
    // click pace: the packet rotation oscillated up to the player's view and
    // back down for every block, and each re-acquisition delayed the next
    // click by the travel time — the "places sometimes, sometimes not" defect.
    {
        const bool bridgeStillActive =
            (GetBridge() && moving) || ((now - g_scfLastPlaceMs) <= kScfAimHoldMs);
        if (!bridgeStillActive)
            scfReleaseSilentAim(lp, aimStepDeg);
    }

    // 1-second placement telemetry (Debug setting): passes, blocks, and the
    // skip-reason split, so a throttled bridge shows WHERE the passes die.
    // phys/aim/sprint are the new anti-detection gates: "aim" firing every pass
    // means the head cannot keep up with the placement direction (which is also
    // what the user sees as the bridge lagging behind them).
    {
        mDbgPasses++;
        mDbgPlaced += placed;
        if (now - mDbgLastMs >= 1000) {
            scfDbg(dbg, "1s: passes=%d placed=%d solid=%d support=%d reach=%d repeat=%d fail=%d phys=%d aim=%d sprint=%d baseY=%d",
                   mDbgPasses, mDbgPlaced, mDbgSkipSolid, mDbgSkipSupport,
                   mDbgSkipReach, mDbgSkipRepeat, mDbgBuildFail,
                   g_scfSkipPhys, g_scfSkipAim, g_scfSkipSprint, baseY);
            mDbgLastMs = now;
            mDbgPasses = 0; mDbgPlaced = 0; mDbgSkipSolid = 0; mDbgSkipSupport = 0;
            mDbgSkipReach = 0; mDbgSkipRepeat = 0; mDbgBuildFail = 0;
            g_scfSkipPhys = 0; g_scfSkipAim = 0; g_scfSkipSprint = 0;
        }
    }

    // Swing policy per mode (after the pass, so one arm motion covers the run):
    //   Default — the local GameMode::swing() (function), which animates the
    //             first-person arm AND carries the server swing via the
    //             game's own outbound path. The source is Build, the tag the
    //             game's own placement path uses (Attack is what a melee swing
    //             carries) — the arm then reads as a block placement to
    //             anything that looks at the animation state.
    //   Silent  — NO local swing function. The server already got its
    //             AnimatePacket per placement above (sendSwing); calling
    //             GM::swing() here would play the first-person animation and
    //             defeat the point of silent mode.
    // One placement per pass means one swing per placement in both modes, which
    // is what vanilla does — a swing with no placement (or several swings per
    // block) is its own flag.
    if (anyOk && !silentMode) GM::swing(lp, GM::SwingSource::Build);

    scfDbg(dbg, "gm=%p blocks=%d baseY=%d count=%d placed=%d last=(%d,%d,%d) fwd=(%.2f,%.2f) yaw=%.1f moving=%d step=%.1f",
           gm, blockCount, baseY, count, placed, lastX, lastY, lastZ, fwdX, fwdZ, yaw,
           (int)moving, aimStepDeg);
}

// ---------------------------------------------------------------------------
// Show Blocks HUD counter (modern counter plate).
//
// Layout is ALWAYS "[icon] Blocks N": "Blocks" stays plain white and the COUNT
// is drawn digit by digit in the live theme gradient (the ColorUtils
// getThemedColor sweep the FPS/BPS counters use) — in BOTH modes. With "Block
// Icon" on the held block's real texture is drawn INSIDE the plate, to the LEFT
// of the label. It used to get a slot of its own OUTSIDE the panel, which made
// the HUD footprint wider than the visible plate (and the game-render icon pass
// could only ever paint UNDER the blur); now the footprint IS the plate.
//
// Look is the CounterStyle.h language: frosted plate (backdrop blur, dim,
// optional drop shadow, thin white hairline), crisp glyphs (no baked text
// shadow), a per-digit odometer roll when the count changes, and a scale-up
// appearance (0.85 -> 1 with an ease-out-back overshoot, fading out down to
// 0.9 while the alpha follows). Every one of those knobs is a setting appended
// at the END of the ctor and resolved BY NAME here, so no existing mSettings
// index the getters use can shift.
// ---------------------------------------------------------------------------

// Block name for the texture db key. getName() returns a std::string so it
// cannot sit inside __try (C2712); the pointers are probed with sdk_safe_ptr
// before the call instead (the same discipline BlockInfo uses).
static std::string sehBlockNameOf(SDK::ItemStack* pick) {
    if (!pick || !sdk_safe_ptr(pick, 0x20)) return {};
    void* bp = pick->getBlockPtr();
    if (!bp || !sdk_safe_ptr(bp, 0x10)) return {};
    SDK::Block* b = static_cast<SDK::Block*>(bp);
    SDK::BlockLegacy* leg = b->getLegacy();
    if (!leg || !sdk_safe_ptr(leg, 0x10)) return {};
    return leg->getName();
}

// Ease-out-back (the ArrayList / ClickGUI spring): overshoots ~10% past 1 and
// settles back, which is what makes a spawning HUD element "pop" instead of
// decelerating into place and reading as a stutter.
static float scfEaseOutBack(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

void Scaffold::OnRender() {
    if (!GetShowBlocks()) return;
    // HUD screens include chat/pause, so the counter stays visible (and
    // draggable with the mouse) there — not just in-game.
    if (!HudEditor::HudVisible()) return;

    // Default position: centered, slightly below the middle. Applied on the
    // first render while the editor/config hasn't moved the plate yet.
    OnFirstLayout(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);

    // ---- counter-style settings, resolved BY NAME ---------------------------
    // They are appended AFTER every pre-existing setting in the ctor (the
    // positional mSettings indexes the getters use must never move), so they can
    // only be found by name — exactly how CounterStyle.h does it.
    BoolSetting*   setTextShadow = nullptr;   // default OFF: glyphs stay crisp
    NumberSetting* setShadowOp   = nullptr;   // plate drop-shadow opacity
    NumberSetting* setBlur       = nullptr;   // backdrop blur strength
    NumberSetting* setDim        = nullptr;   // plate fill opacity
    NumberSetting* setRounding   = nullptr;   // plate corner radius
    BoolSetting*   setGradient   = nullptr;   // count in the theme gradient
    BoolSetting*   setNumAnim    = nullptr;   // per-digit odometer roll
    for (Setting* s : mSettings) {
        if (!s) continue;
        if      (s->mName == "Text Shadow")         setTextShadow = static_cast<BoolSetting*>(s);
        else if (s->mName == "Shadow Opacity")      setShadowOp   = static_cast<NumberSetting*>(s);
        else if (s->mName == "Background Blur")     setBlur       = static_cast<NumberSetting*>(s);
        else if (s->mName == "Background Dim")      setDim        = static_cast<NumberSetting*>(s);
        else if (s->mName == "Background Rounding") setRounding   = static_cast<NumberSetting*>(s);
        else if (s->mName == "Gradient Numbers")    setGradient   = static_cast<BoolSetting*>(s);
        else if (s->mName == "Number Animation")    setNumAnim    = static_cast<BoolSetting*>(s);
    }

    float dt = ImRenderUtils::getDeltaTime();
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;

    // Appearance driver (0 = hidden, 1 = fully shown). The exponential approach
    // is kept (instead of a one-shot timer) because it keeps producing frames
    // while the value sits inside (0,1) — that is what renders the fade-out
    // after the module is switched off. The plate's SCALE is derived from it
    // below, so this is no longer an alpha-only fade.
    const bool shown = IsEnabled();
    mBlocksAnim += ((shown ? 1.0f : 0.0f) - mBlocksAnim) * std::min(1.0f, dt * 10.0f);
    if (mBlocksAnim < 0.002f) {
        mBlocksAnim = 0.0f;
        mIconWanted = false;
        return;
    }
    const float a = mBlocksAnim;

    // Live inventory only — never invent / optimistic-decrement the count.
    // (Optimistic look-ahead used to subtract ~PlacesPerPass and show -10 garbage.)
    if (IsEnabled()) {
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
        mBlocksShown = (GetCountMode() == 1) ? sehCountHeldBlocks(lp) : sehCountHotbarBlocks(lp);
    }

    // Roll transition: when the REAL count changes, the old number slides up
    // and out while the new one slides up into place (0.28 s, ease-out). The
    // old scheme lerped the displayed value toward the target, which on a
    // falling count just ticked 5, 4, 3, 2, 1 with no visible motion at all.
    if (mBlocksShown != mShownValue) {
        mSlideFrom = mShownValue;
        mShownValue = mBlocksShown;
        mSlideT = 0.0f;
    }
    if (mSlideT < 1.0f) {
        mSlideT = std::min(1.0f, mSlideT + dt / 0.28f);
        if (mSlideT >= 1.0f) mSlideFrom = mShownValue;
    }
    const float slideEase = 1.0f - (1.0f - mSlideT) * (1.0f - mSlideT) * (1.0f - mSlideT);

    int dispInt = mShownValue;
    if (dispInt < 0) dispInt = 0;
    int prevInt = mSlideFrom;
    if (prevInt < 0) prevInt = 0;

    // The label is ALWAYS the word "Blocks" — the icon mode only ADDS the
    // texture in front of it, it never replaces the text.
    const bool useIcon = GetBlockIcon();
    const std::string label = "Blocks";
    const std::string number = std::to_string(dispInt);
    const std::string prevNumber = std::to_string(prevInt);

    const float scale = mScale > 0.01f ? mScale : 1.0f;
    const float fontSize = 24.0f * scale;
    const float padX = 12.0f * scale, padY = 6.0f * scale;
    const float iconSize = useIcon ? fontSize * 1.15f : 0.0f;   // inside the plate
    const float iconGap  = useIcon ? 5.0f * scale : 0.0f;
    const float labelGap = 5.0f * scale;                        // "Blocks" -> count

    const float labelW = ImRenderUtils::getTextWidth(&label, fontSize);
    // Panel width covers the WIDER of the two numbers so the per-digit roll
    // never resizes the plate mid-animation.
    const float numW = std::max(ImRenderUtils::getTextWidth(&number, fontSize),
                                ImRenderUtils::getTextWidth(&prevNumber, fontSize));
    // A block cube is taller than a run of uppercase glyphs, so the row — and
    // with it the plate — has to fit the icon when it is on.
    const float rowH = useIcon ? std::max(fontSize, iconSize) : fontSize;
    const float width  = padX * 2.0f + iconSize + iconGap + labelW + labelGap + numW;
    const float height = rowH + padY * 2.0f;

    // Footprint = the plate. The icon lives inside it now, so the HUD editor box
    // matches the visible panel exactly (and a disabled counter reports nothing
    // to drag, since the render below early-outs).
    mWidth = width;
    mHeight = height;

    const ImVec4 rect(mPosX, mPosY, mPosX + width, mPosY + height);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;

    // ---- appear / disappear: SCALE-UP + fade --------------------------------
    // Coming up, the plate springs from 0.85 to its resting size with an
    // ease-out-back overshoot (it crosses ~1.015 before settling). Going away it
    // eases down to 0.90 while the fill/shadow alpha follows mBlocksAnim to
    // zero, so the panel reads as retracting rather than dissolving in place.
    const float zoom = shown
        ? 0.85f + 0.15f * scfEaseOutBack(a)
        : 0.90f + 0.10f * (a * a * (3.0f - 2.0f * a));   // smoothstep down

    // Center-pivot zoom: the plate, the icon and every glyph position are mapped
    // through these, so nothing crawls out of the plate while it animates.
    const float pivotX = rect.x + width * 0.5f;
    const float pivotY = rect.y + height * 0.5f;
    auto zx = [&](float x) { return pivotX + (x - pivotX) * zoom; };
    auto zy = [&](float y) { return pivotY + (y - pivotY) * zoom; };

    const ImVec4 plate(zx(rect.x), zy(rect.y), zx(rect.z), zy(rect.w));
    float rounding = (setRounding ? setRounding->mValue : 6.0f) * scale * zoom;
    if (rounding < 0.0f) rounding = 0.0f;

    // ---- frosted plate (the same plate CounterStyle.h paints) ---------------
    // Full strength as soon as the plate is meaningfully visible (a > 1/3) and
    // ramped below that: a full-strength blur patch must never flash while the
    // plate itself is still ~invisible. D2D quantizes the strength to 0.5 steps
    // and only re-runs its full-screen Gaussian when the value CHANGES, so this
    // ramp costs 2-3 bakes instead of one per frame of the transition.
    const float blur = (setBlur ? setBlur->mValue : 3.0f) * std::min(1.0f, a * 3.0f);
    if (blur > 0.01f)
        D2D::addBlurOptimized(drawList, blur, std::optional<ImVec4>(plate), rounding);

    const float shadowA = setShadowOp ? setShadowOp->mValue : 0.28f;
    if (shadowA > 0.01f)
        ImRenderUtils::fillShadowRectangle(plate, ImColor(0, 0, 0), shadowA * a,
                                           rounding, 12.0f * scale, 0.6f, drawList);

    const float dim = setDim ? setDim->mValue : 0.38f;
    ImRenderUtils::fillRectangle(plate, ImColor(12, 12, 15, 255), dim * a, rounding, drawList);
    // Thin white hairline: it is what separates the plate from a dark backdrop
    // without the heavy black inner border the old panel carried.
    drawList->AddRect(ImVec2(plate.x, plate.y), ImVec2(plate.z, plate.w),
                      ImColor(255, 255, 255, (int)(25.0f * a)), rounding, 0, 1.0f);

    // ---- "[icon] Blocks 123" ------------------------------------------------
    // The glyph size zooms with the plate (fs = fontSize * zoom) so the text
    // keeps its intended inset from the plate edges at every point of the
    // animation — scaling only the POSITIONS let a full-size number poke out of
    // a 0.85 plate.
    const float fs = fontSize * zoom;
    const float rowCenterY = zy(rect.y + height * 0.5f);   // row is plate-centred
    const float textY = rowCenterY - fs * 0.5f;
    const float labelScaledW = ImRenderUtils::getTextWidth(&label, fs);

    const bool gradient = !setGradient || setGradient->mValue;      // default ON
    const bool doShadow = setTextShadow && setTextShadow->mValue;   // default OFF
    const uint64_t nowMs = ColorUtils::GetTimeMs();
    const ImColor labelCol(255, 255, 255, 255);

    float x = zx(rect.x + padX);
    if (useIcon) {
        // The icon is drawn INSIDE the plate, left of the label, with the ImGui
        // pass — SDK::drawBlockTexture3D lands ON TOP of the plate/blur, which
        // the game-side ItemRenderer slot outside the panel never could.
        //
        // The texture db is keyed by BLOCK names ("planks"), not ITEM names
        // ("oak_planks") — the block's own BlockLegacy::getName() is the key
        // that resolves. Item name is only the last-ditch fallback.
        std::string iconName;
        SDK::ClientInstance* ici = SDK::ClientInstance::get();
        SDK::Actor* ilp = ici ? ici->getLocalPlayer() : nullptr;
        if (ilp) {
            SDK::PlayerInventory* inv = ilp->getSupplies();
            SDK::Container* c = inv ? inv->getContainer() : nullptr;
            if (inv && c) {
                int cur = inv->getSelectedSlot();
                SDK::ItemStack* held = (cur >= 0 && cur <= 8) ? c->getItem(cur) : nullptr;
                SDK::ItemStack* pick = (held && held->isPlaceableBlock()) ? held : nullptr;
                if (!pick) {
                    for (int slot = 0; slot < 9; ++slot) {
                        SDK::ItemStack* is = c->getItem(slot);
                        if (is && is->isPlaceableBlock()) { pick = is; break; }
                    }
                }
                if (pick) {
                    // getBlockPtr returns the raw Block* (SDK type is void);
                    // SEH lives in a helper frame because iconName/std::string
                    // cannot sit next to __try (C2712).
                    iconName = sehBlockNameOf(pick);
                    if (iconName.empty()) iconName = pick->getItemName();
                    else SDK::GameIcon::noteBlock(iconName, pick->getBlockPtr());
                }
            }
        }
        mIconSize = iconSize * zoom;
        mIconX = x;
        mIconY = rowCenterY - mIconSize * 0.5f;
        mIconWanted = a > 0.05f;   // geometry is live whenever the plate shows
        SDK::drawBlockTexture3D(drawList, mIconX, mIconY, mIconSize, iconName.c_str(), a);
        x += mIconSize + iconGap * zoom;
    } else {
        mIconWanted = false;
    }

    if (!label.empty())
        ImRenderUtils::drawText(ImVec2(x, textY), label, labelCol, fs, a, doShadow, drawList);
    x += labelScaledW + labelGap * zoom;

    // Per-digit odometer — the same mechanic the FPS/BPS counters use: digits
    // that did not change stay put, only the CHANGED ones roll (the outgoing
    // glyph slides up and fades while the new one rises into its cell). Digits
    // are compared RIGHT-aligned, so 51 -> 56 rolls just the trailing digit and
    // 51 -> 60 rolls both. "Number Animation" off snaps instead of rolling.
    const bool rolling = (!setNumAnim || setNumAnim->mValue) &&
                         mSlideT < 1.0f && mSlideFrom != mShownValue;
    const float roll = fs * 0.55f;
    const int newLen = (int)number.size();
    const int oldLen = (int)prevNumber.size();
    for (int i = 0; i < newLen; ++i) {
        const std::string glyph(1, number[i]);
        const float gw = ImRenderUtils::getTextWidth(&glyph, fs);
        // "Gradient Numbers" off falls back to the plain white label colour.
        const ImColor col = gradient
            ? ColorUtils::getThemedColor(60.0f + (float)i * 72.0f, nowMs)
            : labelCol;
        const int oldIdx = i - (newLen - oldLen);   // right-aligned comparison
        const bool unchanged = !rolling ||
            (oldIdx >= 0 && oldIdx < oldLen && prevNumber[oldIdx] == number[i]);
        if (unchanged) {
            ImRenderUtils::drawText(ImVec2(x, textY), glyph, col, fs, a, doShadow, drawList);
        } else {
            const std::string oldGlyph = (oldIdx >= 0 && oldIdx < oldLen)
                ? std::string(1, prevNumber[oldIdx]) : std::string();
            if (!oldGlyph.empty())
                ImRenderUtils::drawText(ImVec2(x, textY - slideEase * roll), oldGlyph,
                                        col, fs, a * (1.0f - slideEase), doShadow, drawList);
            ImRenderUtils::drawText(ImVec2(x, textY + (1.0f - slideEase) * roll), glyph,
                                    col, fs, a, doShadow, drawList);
        }
        x += gw;
    }
}

// The icon is drawn by the ImGui pass (OnRender) with the block's REAL
// texture through SDK::drawBlockTexture3D — on top of the plate/blur, which
// the game-side ItemRenderer pass could never manage.
void Scaffold::DrawGameIcon(void* mcuirc, const std::string& screenName) {
    (void)mcuirc; (void)screenName;
}
