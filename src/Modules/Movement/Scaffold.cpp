#include "Scaffold.h"
#include "../Player/ChestStealer.h"
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
#include <cstring>
#include <string>
#include <Windows.h>
#include <imgui.h>
using SDK::IVec3;
using SDK::sehGetPos;
namespace GM = SDK::GameMode;

static const unsigned long long kScaffoldTimerGranularityMs = 16;

static const float kScaffoldMaxPassesPerSec = 100.0f;

static const float kScaffoldPlaceReach = 5.0f;

static bool GetKeyStateDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ===========================================================================

static const float kScfRotGcdDeg = 0.15f;

    static const float kScfAimDegPerSec = 700.0f;

// The head must be within this many degrees of the aim point before a placement
// is allowed out (point 3 above).
static const float kScfAimToleranceDeg = 24.0f;

static const unsigned long long kScfAimHoldMs = 500;

static const float kScfMinPlacePerSec = 6.0f;
static const float kScfMaxPlacePerSec = 18.0f;

static const float kScfClickInset = 0.26f;

static const float kScfFallPlaceGapMinMs = 55.0f;
static const float kScfFallPlaceGapMaxMs = 85.0f;

static const float kScfBackPlaceGuardDeg = 110.0f;

// ---------------------------------------------------------------------------
// Aim / pace state
// ---------------------------------------------------------------------------

static float g_scfAimYaw = 0.0f;
static float g_scfAimPitch = 0.0f;
static bool  g_scfAimArmed = false;     // true while a placement burst drives the head

static bool  g_scfClickValid = false;
static int   g_scfClickCell[3] = { 0, 0, 0 };
static unsigned char g_scfClickFace = 0;
static Vec3  g_scfClickPoint{};

static unsigned long long g_scfNextPlaceMs = 0;
static unsigned long long g_scfLastPlaceMs = 0;
static unsigned long long g_scfLastPassMs = 0;

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

static float scfHumanStep(float cur, float target, float maxStepDeg, bool isYaw,
                           const RotationUtil::Params* rp = nullptr) {
    RotationUtil::Params p = rp ? *rp : RotationUtil::Params{};
    if (p.rotationSpeed <= 0.0f) p.rotationSpeed = kScfAimDegPerSec;
    const float diff = isYaw ? angleDelta(cur, target) : (target - cur);
    const float mag  = std::fabs(diff);
    if (mag <= kScfRotGcdDeg) return target;   // on it — a hand stops there too
    float travel = std::min(mag, maxStepDeg * SDK::randRange(0.45f, 1.0f));
    // smoothMode + maxAnglePerTick + jitter routed through RotationUtil.
    if (rp) {
        travel = std::min(mag, RotationUtil::stepOnce(cur, target, maxStepDeg, p, isYaw));
    }
    if (rp && rp->microJitterDeg > 0.0f) {
        const float jit = SDK::randRange(-rp->microJitterDeg, rp->microJitterDeg);
        travel = std::max(0.01f, travel + jit);
    }
    travel = scfGridSteps(travel);
    if (travel > mag) travel = mag;            // never overshoot the aim
    const float out = cur + (diff > 0.0f ? travel : -travel);
    return isYaw ? wrapAngle180(out) : out;
}

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

static Vec3 scfClickPointOnFace(const IVec3& block, unsigned char face) {
    if (!g_scfClickValid || g_scfClickFace != face ||
        g_scfClickCell[0] != block.x || g_scfClickCell[1] != block.y ||
        g_scfClickCell[2] != block.z) {
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
        scfFaceNormal(face, &nx, &ny, &nz);
        float ox = SDK::randRange(-kScfClickInset, kScfClickInset);
        float oy = SDK::randRange(-kScfClickInset, kScfClickInset);
        float oz = SDK::randRange(-kScfClickInset, kScfClickInset);
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

static float scfDistToBlockAabb(const Vec3& p, const IVec3& b) {
    const float dx = std::max(0.0f, std::max((float)b.x - p.x, p.x - (float)(b.x + 1)));
    const float dy = std::max(0.0f, std::max((float)b.y - p.y, p.y - (float)(b.y + 1)));
    const float dz = std::max(0.0f, std::max((float)b.z - p.z, p.z - (float)(b.z + 1)));
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

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

static void scfApplyRealHead(SDK::Actor* lp, float curYaw, float curPitch,
                             float aimYaw, float aimPitch) {
    (void)lp; (void)curYaw; (void)curPitch; (void)aimYaw; (void)aimPitch;
}

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

static void scfReleaseSilentAim(SDK::Actor* lp, float maxStepDeg) {
    float realYaw = g_scfAimYaw, realPitch = g_scfAimPitch;
    if (!scfReadRot(lp, &realYaw, &realPitch)) return;
    if (!g_scfAimArmed) {
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

static bool sehGetYaw(SDK::Actor* a, float* out) {
    __try {
        auto* rc = a->getRotation();
        if (!rc) return false;
        *out = rc->rotation.y;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}


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

static float g_silentYaw   = 0.f;
static float g_silentPitch = 0.f;
static bool  g_silentRotValid = false;
static unsigned long long g_silentPublishMs = 0;
// Freshness horizon: passes publish every 8-70 ms while a run is active, so
// 250 ms without a publish unambiguously means "no active aim".
static constexpr unsigned long long kSilentLatchTtlMs = 250;
// Movement frame published next to the silent aim (see sehSilentAimAtPoint).
static float g_silentRealYaw = 0.f;
static int   g_silentFwd = 0;
static int   g_silentStrafe = 0;

bool Scaffold::ConsumeSilentRot(float& yaw, float& pitch) {
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


// Is the given cell air (placeable target)?
static bool sehIsAir(SDK::BlockSource* src, const IVec3& p) {
    if (!src) return true;
    __try { return src->isAir(SDK::BlockPos(p.x, p.y, p.z)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

// Does the block at this cell carry a block actor / open a container UI?
//
// 1.26.62 crash analysis (crash-20261110-011107.dmp): the game's block-actor
// neighborhood system walked a chest the client clicked while Scaffold was
// placing, advanced a stale cache ctrl-block (BlockSource vtable slot 41)
// and faulted reading its vtable (AV read at 0x8 in sub_1432CD4B0). A
// legitimate player NEVER clicks a chest to "place a block against it", so
// chest-like blocks must never be used as the clicked support.
//
// UPDATE (crash-20261110-094849.dmp, 3 identical dumps): the REAL root cause
// of the slot-41 stale-ctrl crashes was a thread race, not chests — our
// getBlock leader-capture promoted the SERVER thread's BlockSource (it has
// the hottest getBlock traffic) and Scaffold then used it from the main
// thread, corrupting its non-thread-safe chunk cache. Fixed in
// BlockSourceHooks.cpp (per-thread leaders + owner-thread gate at +0x18).
// The chest blacklist below stays: clicking a container block to place
// against it is still illegitimate and triggers its own server-side issues.
static bool sehIsBlockActorBlock(SDK::BlockSource* src, const IVec3& p) {
    if (!src) return false;
    __try {
        SDK::Block* b = src->getBlock(SDK::BlockPos(p.x, p.y, p.z));
        SDK::BlockLegacy* leg = b ? b->getLegacy() : nullptr;
        if (!leg) return false;
        // POD-only: no std::string local inside __try (C2712). BlockLegacy's
        // mName std::string lives at +0xE8 (see BlockLegacy::getName()).
        char nbuf[128] = {};
        size_t nlen = 0;
        if (!sdk_copy_cstr((void*)((uintptr_t)leg + 0xE8), nbuf, sizeof(nbuf), &nlen))
            return false;
        auto endsWith = [&](const char* suf) {
            const size_t sl = strlen(suf);
            return nlen >= sl && memcmp(nbuf + nlen - sl, suf, sl) == 0;
        };
        // "chest", "trapped_chest", "ender_chest" all end in "chest".
        if (endsWith("chest")) return true;
        if (endsWith("barrel")) return true;
        if (endsWith("shulker_box")) return true;
        if (endsWith("furnace")) return true;   // covers blast_furnace too
        if (endsWith("hopper")) return true;
        if (endsWith("dropper") || endsWith("dispenser")) return true;
        if (endsWith("lectern")) return true;
        if (endsWith("brewing_stand")) return true;
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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
        if (passIsAir(src, np)) continue;    // neighbour must be solid to click
        if (sehIsBlockActorBlock(src, np)) continue;  // never click chests & co.
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

    // Vanilla can't click in-world while a container UI is open, neither can we.
    // Crash guard for 1.26.62 (crash-20261110-011107.dmp): a chest-screen
    // flip-flop racing a placement left the game's block-actor cache stale
    // and the render-thread resolver faulted on a freed ctrl-block.
    if (ChestStealer::IsContainerScreen(SDK::ScreenView::currentScreenName())) return;

    const bool silentMode = (GetType() == 1);
    const bool wantRot = GetRotation();
    if (!wantRot) InvalidateSilentRot();

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx, false)) return;
    SDK::ClientInstance* ci = ctx.ci;
    SDK::Actor* lp = ctx.lp;

    Vec3 pos{};
    if (!sehGetPos(lp, &pos)) return;

    float speed = GetSpeed();
    if (speed < 1.0f) speed = 1.0f;
    if (speed > kScaffoldMaxPassesPerSec) speed = kScaffoldMaxPassesPerSec;

    bool footingVoid = false;
    {
        const int feetCellNow = (int)std::floor(pos.y - 1.62f + 0.001f);
        const int floorY = mBridgeLevelValid ? mBridgeLevel : feetCellNow - 1;
        if (std::fabs(pos.y - (float)(floorY + 1 + 1.62f)) < 1.3f) {
            SDK::BlockSource* bs = ci->getBlockSource();
            if (bs) {
                footingVoid = sehIsAir(bs, IVec3{ (int)std::floor(pos.x), floorY,
                                                 (int)std::floor(pos.z) });
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
        unsigned long long passIntervalMs = (unsigned long long)(1000.0f / speed);
        if (passIntervalMs <= kScaffoldTimerGranularityMs) passIntervalMs = 0;
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
    if (!gm || !vt) return;
    SDK::BlockSource* blockSource = ci->getBlockSource();
    SDK::Level* level = nullptr;
    {
        SDK::Minecraft* mc = ci->getMinecraft();
        if (mc && sdk_safe_ptr(mc, 0xC0)) level = mc->getLevel();
    }

    const unsigned long long now = GetTickCount64();
    // Footing emergency (see the safety block above): the click window is
    // forced open so the hole under the feet closes THIS pass.
    const bool placeWindowOpen = footingVoid || (now >= g_scfNextPlaceMs);

    float dtSec = 1.0f / 60.0f;
    if (g_scfLastPassMs && now > g_scfLastPassMs) {
        dtSec = (float)(now - g_scfLastPassMs) / 1000.0f;
        if (dtSec > 0.10f) dtSec = 0.10f;
    }
    g_scfLastPassMs = now;
    float aimStepDeg = dtSec * kScfAimDegPerSec;
    if (aimStepDeg < kScfRotGcdDeg) aimStepDeg = kScfRotGcdDeg;

    int blockCount = 0;
    const bool autoPick = GetBuild();
    int silentSwapSlot = -2;
    int silentPrevSlot = -1;
    {
        // Track MANUAL hotbar switches: the vanilla UI announces every user
        // slot change to the server, so a visible-slot change means the
        // server's held slot moved with it (OreMiner tracks the same way).
        // Without this, mServerSlot goes stale after every manual switch and
        // we would silently skip equipment re-syncs the server still needs.
        // On the very first pass (mLastVisibleSlot==-1) we seed mServerSlot
        // from the visible slot: the server learned it from the spawn equip.
        {
            int vis = -1;
            __try {
                SDK::PlayerInventory* vinv = lp->getSupplies();
                vis = vinv ? vinv->getSelectedSlot() : -1;
            } __except (EXCEPTION_EXECUTE_HANDLER) { vis = -1; }
            if (vis >= 0 && vis != mLastVisibleSlot) {
                if (mLastVisibleSlot >= 0 && mServerSlot == mLastVisibleSlot)
                    mServerSlot = vis;              // vanilla re-synced the server
                else if (mLastVisibleSlot < 0 && mServerSlot == -1)
                    mServerSlot = vis;              // fresh enable / fresh world
                mLastVisibleSlot = vis;
            }
        }
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
        if (silentSwapSlot == -2) return;
        if (silentSwapSlot >= 0 && placeWindowOpen && autoPick) {
            // Server-first slot sync: the placement transaction is validated
            // against the SERVER's held slot. Right after world join the
            // server still believes the spawn slot is held — placing through
            // a silently swapped slot then produces a bare swing with no
            // block (the report: first placement after join never works
            // until you once hold a block manually, which is exactly the
            // vanilla sync thisdance replicates). So: announce the block
            // slot to the server BEFORE flipping, and if the equipment send
            // fails (packet sender not ready etc.) skip this pass entirely —
            // a blind BuildBlock against a desynced server is worthless.
            if (silentSwapSlot != mServerSlot) {
                if (!SDK::PacketUtils::sendMobEquipment(lp, silentSwapSlot))
                    return;                 // retry next pass; no flip, no swing
                mServerSlot = silentSwapSlot;
            }
            __try {
                SDK::PlayerInventory* inv = lp->getSupplies();
                silentPrevSlot = inv ? inv->getSelectedSlot() : -1;
            } __except (EXCEPTION_EXECUTE_HANDLER) { silentPrevSlot = -1; }
            if (!sehSetSelectedSlotPOD(lp, silentSwapSlot)) return;
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

    const float PLAYER_HEIGHT = 1.62f;
    const int feetCell = (int)std::floor(pos.y - PLAYER_HEIGHT + 0.001f);
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

    int count = bridging ? GetPlaces() : 1;
    if (count < 1) count = 1;
    if (count > 32) count = 32;

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

    int  sprintFlag = -1;
    __try { sprintFlag = lp->getStatusFlag(ActorFlags::Sprinting); }
    __except (EXCEPTION_EXECUTE_HANDLER) { sprintFlag = -1; }
    const bool  sprinting  = (sprintFlag == 1);
    const bool  movingFast = ((vel.x * vel.x + vel.z * vel.z) > 0.0025f);
    const float moveYaw    = std::atan2(-vel.x, vel.z) * 57.2957795f;

    const float reach = kScaffoldPlaceReach;

    passPlacedReset();

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

        if (k > 0 && passPlacedHas(target)) { if (useClutch) continue; }

        if (blockSource && !passIsAir(blockSource, target)) {
            if (bridging || useClutch) continue;
            break;
        }

        IVec3 clickBlock = target;
        unsigned char face = 1;
        IVec3 support{};
        unsigned char supportFace = 0;
        bool haveSupport = false;
        if (blockSource) {
            haveSupport = passGetPlaceFace(blockSource, target, &support, &supportFace);
        }
        if (!haveSupport) {
            if (bridging) continue;
            break;
        }
        clickBlock = support;
        face = supportFace;

        const float dist = scfDistToBlockAabb(pos, clickBlock);
        if (dist > reach) {
            if (bridging) continue;
            break;
        }
        if (dist <= 0.0f) {
            ++g_scfSkipPhys;
            if (bridging) continue;
            break;
        }
        if (target.y >= feetCell && target.y <= feetCell + 1 &&
            target.x == px && target.z == pz) {
            ++g_scfSkipPhys;
            if (bridging) continue;
            break;
        }

        const Vec3 contact = scfClickPointOnFace(clickBlock, face);

        if (sprinting && movingFast && bridging) {
            const float blockYaw = calcLookAngles(pos, contact).y;
            if (std::fabs(angleDelta(moveYaw, blockYaw)) > kScfBackPlaceGuardDeg) {
                ++g_scfSkipSprint;
                break;
            }
        }


        RotationUtil::Params rotP;
        rotP.rotationSpeed = GetRotationSpeed();
        rotP.maxAnglePerTick = GetMaxAnglePerTick();
        rotP.smoothMode = (int)GetSmoothMode();
        rotP.predictPlacementAngle = GetPredictPlacement();
        rotP.microJitterDeg = GetJitter();
        if (rotP.rotationSpeed <= 0.0f) rotP.rotationSpeed = kScfAimDegPerSec;

        bool aimOnBlock = false;
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
            ++g_scfSkipAim;
            break;
        }
        if (!placeWindowOpen) break;   // hand has not clicked again yet

        GM::prepBlockHit(level, clickBlock, face, contact.x, contact.y, contact.z);
        sehFixHitRay(level, pos, contact);

        if (silentMode) {
            SDK::PacketUtils::sendSwing(lp);
        }

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
        }
        // One click per hand. The look-ahead above (count) decides WHICH cell is
        // clicked, never how many clicks go out in one pass.
        if (!bridging) break;
        if (placedThisPass >= burstCap) break;
    }

    if (silentSwapSlot >= 0) {
        if (silentPrevSlot >= 0) {
            sehSetSelectedSlotPOD(lp, silentPrevSlot);
        }
    }
    if (anyOk) mLastPlace = now;

    {
        const bool bridgeStillActive =
            (GetBridge() && moving) || ((now - g_scfLastPlaceMs) <= kScfAimHoldMs);
        if (!bridgeStillActive)
            scfReleaseSilentAim(lp, aimStepDeg);
    }

    if (anyOk && !silentMode) GM::swing(lp, GM::SwingSource::Build);
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

static std::string sehBlockNameOf(SDK::ItemStack* pick) {
    if (!pick || !sdk_safe_ptr(pick, 0x20)) return {};
    void* bp = pick->getBlockPtr();
    if (!bp || !sdk_safe_ptr(bp, 0x10)) return {};
    SDK::Block* b = static_cast<SDK::Block*>(bp);
    SDK::BlockLegacy* leg = b->getLegacy();
    if (!leg || !sdk_safe_ptr(leg, 0x10)) return {};
    return leg->getName();
}

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

    mWidth = width;
    mHeight = height;

    const ImVec4 rect(mPosX, mPosY, mPosX + width, mPosY + height);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;

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

void Scaffold::DrawGameIcon(void* mcuirc, const std::string& screenName) {
    (void)mcuirc; (void)screenName;
}
