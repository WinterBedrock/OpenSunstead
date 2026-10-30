#include "Aura.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Actor/RenderPos.h"
#include "../../SDK/SDK.h"
#include "../../Render/FrameView.h"
#include "../../Render/HudEditor.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../Setting.h"
#include "../ModuleManager.h"
#include "../Misc/AntiBot.h"
#include "CombatSignal.h"
#include "SilentRotation.h"
#include "../../Core/Logger.h"
#include <vector>
#include <cmath>
#include <cstdarg>
#include <algorithm>
#include <Windows.h>

using SDK::sehGetPos;
using SDK::sehGetActorList;
using SDK::sehIsPlayer;
using SDK::sehGetActorBox;
namespace GM = SDK::GameMode;

SUNSTEAD_DEFINE_DBG(auraDbg, "Aura", 250)

static Vec3 closestPointOnBox(const Vec3& mn, const Vec3& mx, const Vec3& p) {
    Vec3 r = p;
    if (r.x < mn.x) r.x = mn.x; else if (r.x > mx.x) r.x = mx.x;
    if (r.y < mn.y) r.y = mn.y; else if (r.y > mx.y) r.y = mx.y;
    if (r.z < mn.z) r.z = mn.z; else if (r.z > mx.z) r.z = mx.z;
    return r;
}

// Small, smooth aim jitter inside the hitbox so rotations don't hard-lock onto
// one fixed point every tick (anti-cheat heuristic). Magnitude stays tiny; the
// randomness comes from the shared SDK generator.
static Vec3 jitterAimPoint(const Vec3& base, const Vec3& mn, const Vec3& mx, bool haveBox) {
    // ~2-4 cm world jitter, clamped inside AABB when available.
    Vec3 out{ base.x + SDK::randSigned(0.035f),
              base.y + SDK::randSigned(0.045f),
              base.z + SDK::randSigned(0.035f) };
    if (haveBox) {
        const float eps = 0.02f;
        if (out.x < mn.x + eps) out.x = mn.x + eps;
        if (out.x > mx.x - eps) out.x = mx.x - eps;
        if (out.y < mn.y + eps) out.y = mn.y + eps;
        if (out.y > mx.y - eps) out.y = mx.y - eps;
        if (out.z < mn.z + eps) out.z = mn.z + eps;
        if (out.z > mx.z - eps) out.z = mx.z - eps;
    }
    return out;
}

// Aim at the given world point through the silent-rotation channel. The angle
// math itself is SDK::calcLookAngles (Bedrock convention, pitch clamped, yaw
// wrapped); this only adds the sub-degree noise that keeps the silent channel
// from being a perfectly frozen ray.
static void aimAt(const Vec3& eye, const Vec3& aimPoint, bool flick) {
    const float dy = aimPoint.y - eye.y;
    if (vecHorizontalDistance(aimPoint, eye) < 1e-4f && std::fabs(dy) < 1e-4f) return;

    Vec2 look = calcLookAngles(eye, aimPoint);   // x = pitch, y = yaw
    float yaw   = look.y + SDK::randSigned(0.35f);
    float pitch = look.x + SDK::randSigned(0.25f);
    if (pitch >  89.9f) pitch =  89.9f;
    if (pitch < -89.9f) pitch = -89.9f;
    yaw = wrapAngle180(yaw);

    if (flick) SilentRotation::SetOnce(yaw, pitch);
    else       SilentRotation::SetContinuous(yaw, pitch);

    // Movement-frame restatement: publish the live WASD state + the real yaw
    // the client integrated movement against, so the send hook can re-express
    // the move vectors in the spoofed frame. Without this the server sees the
    // direction bits against the aim yaw ("running backwards") and corrects.
    const bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
    const bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
    const bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
    SilentRotation::SetMoveInput((w ? 1 : 0) - (s ? 1 : 0), (d ? 1 : 0) - (a ? 1 : 0));
    float realYaw = yaw;
    __try {
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
        if (lp) realYaw = lp->getRot().y;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    SilentRotation::PublishMoveFrame(realYaw);
}

// Segment-vs-AABB slab test for the Raycast bypass: does the eye->target
// segment pass through the given hitbox?
static bool segIntersectsAABB(const Vec3& a, const Vec3& b, const Vec3& mn, const Vec3& mx) {
    float t0 = 0.f, t1 = 1.f;
    const float* pa = &a.x; const float* pb = &b.x;
    const float* pmn = &mn.x; const float* pmx = &mx.x;
    for (int i = 0; i < 3; ++i) {
        const float d = pb[i] - pa[i];
        if (std::fabs(d) < 1e-8f) {
            if (pa[i] < pmn[i] || pa[i] > pmx[i]) return false;
        } else {
            float ta = (pmn[i] - pa[i]) / d, tb = (pmx[i] - pa[i]) / d;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta); t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
    }
    return true;
}

// Interact(left-click) packet right before the attack transaction — the real
// client's input pipeline pairs them, and some anticheats check for it
// (Prax/Solstice "Interact"). Layout reversed from this build's factory
// (sub_1416B3EA0): action u8 @ +0x30, targetRuntimeId u64 @ +0x38, pos @ +0x40.
static bool sendInteractAttack(SDK::Actor* target, const Vec3& pos) {
    if (!g_MinecraftPackets_createPacket || !target) return false;
    // SEH + shared_ptr don't mix (C2712) — read the runtime id via the POD
    // helper the packet utilities already use.
    uint64_t rid = SDK::PacketUtils::sehRuntimeIdPOD(target);
    if (rid == 0) return false;
    std::shared_ptr<SDK::Packet> pkt =
        SDK::MinecraftPackets::createPacket((SDK::PacketID)33 /*Interact*/);
    if (!pkt) return false;
    uint8_t* p = (uint8_t*)pkt.get();
    *(uint8_t*)(p + 0x30) = 2;             // action = left-click / attack
    *(uint64_t*)(p + 0x38) = rid;           // target runtime id
    *(float*)(p + 0x40) = pos.x;
    *(float*)(p + 0x44) = pos.y;
    *(float*)(p + 0x48) = pos.z;
    return SDK::PacketUtils::sendToServer(pkt.get());
}

// Weapon auto-switch
static int weaponScore(const std::string& lowerName, bool swordOnly) {
    bool isSword = lowerName.find("sword") != std::string::npos;
    bool isAxe   = lowerName.find("axe")   != std::string::npos;
    if (swordOnly && !isSword) return -1;
    if (!isSword && !isAxe)    return -1;
    int tier = 0;
    if      (lowerName.find("netherite") != std::string::npos) tier = 6;
    else if (lowerName.find("diamond")   != std::string::npos) tier = 5;
    else if (lowerName.find("iron")      != std::string::npos) tier = 4;
    else if (lowerName.find("stone")     != std::string::npos) tier = 3;
    else if (lowerName.find("golden")    != std::string::npos) tier = 2;
    else if (lowerName.find("gold")      != std::string::npos) tier = 2;
    else if (lowerName.find("wood")      != std::string::npos) tier = 1;
    else tier = 1; // unknown material still a weapon
    int score = tier * 2;
    if (isSword) score += 1; // prefer sword at equal tier
    return score;
}

// Safely fetch the local player's inventory container (no C++ objects created,
// so this can live inside a __try without tripping C2712).
// NOTE: no outer __try here — getItemName()/Container::getItem() are each
// internally SEH-guarded, and this loop builds std::string objects (C2712
// forbids that inside a __try frame that would need C++ unwinding).
static int sehGetBestWeaponSlot(SDK::Actor* lp, bool swordOnly) {
    SDK::Container* c = SDK::Inv::container(lp);
    if (!c) return -1;

    // Name-based scoring — distinguishes sword > axe, honours swordOnly mode, and
    // only ever matches ACTUAL weapons (weaponScore returns -1 for anything that
    // isn't a sword/axe). Now that Item::mName resolves reliably, this is the
    // sole path: if no real weapon is in the hotbar we return -1 and the caller
    // leaves the held slot untouched (no more switching to food/blocks/pearls,
    // which the old isNonBlockItem fallback caused when names were empty).
    int bestSlot = -1, bestScore = -1;
    for (int slot = 0; slot < 9; ++slot) {
        std::string n = SDK::Inv::lowerNameAt(c, slot);
        if (n.empty()) continue;
        int s = weaponScore(n, swordOnly);
        if (s > bestScore) { bestScore = s; bestSlot = slot; }
    }
    return bestSlot; // -1 when the hotbar holds no sword/axe
}


void Aura::OnUpdate() {
    if (!IsEnabled()) return;

    bool dbg = GetDebug();

    // Clear the continuous silent-rotation latch at the top of every update. If
    // we have a target this tick the branches below re-arm it; otherwise no aim
    // is injected and the local view stays purely mouse-driven. A pending Flick
    // one-shot survives this (see SilentRotation::Clear) so it still lands.
    SilentRotation::Clear(false);

    // Head-rotation smoothing (Solstice-style eased turn). Flick still snaps
    // (handled inside SilentRotation::Consume); this only affects Normal mode.
    SilentRotation::SetSmoothing(GetSmooth());

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx, true)) { auraDbg(dbg, "ctx resolve FAIL"); return; }
    SDK::Level* level = ctx.level;
    SDK::Actor* lp = ctx.lp;

    // DEAD-LOCAL GATE: on servers that show a death screen the local player
    // actor is half-torn-down while the screen is up — attacking through it
    // from here crashed the game. Health 0 (a valid read) means dead: stop
    // everything for this frame.
    {
        const float hp = sehGetHealth(lp);
        if (hp >= 0.0f && hp <= 0.0f) return;
    }

    Vec3 myPos{};
    if (!sehGetPos(lp, &myPos)) { auraDbg(dbg, "getPos FAIL"); return; }

    void** vt = nullptr;
    void* gm = GM::resolve(lp, &vt);
    if (!gm || !vt) { auraDbg(dbg, "GameMode resolve FAIL (lp=%p)", (void*)lp); return; }
    auraDbg(dbg, "gm=%p vt=%p attack(vt[%d])=%p pick(vt[%d])=%p",
            gm, (void*)vt, GM::kAttack, vt[GM::kAttack],
            GM::kGetPickRange, vt[GM::kGetPickRange]);

    std::vector<SDK::Actor*> actors;
    if (!sehGetActorList(level, &actors)) { auraDbg(dbg, "actorList FAIL"); return; }

    float range  = GetRange();
    float range2 = range * range;

    unsigned long long now = GetTickCount64();
    // Attack throttle. With Randomize APS the interval is re-drawn after every
    // hit from [APS Min..APS Max] — a perfectly constant click rate is itself
    // an anticheat heuristic (Prax/Solstice parity).
    bool throttled;
    if (GetRandomizeAPS()) {
        throttled = mNextAttackAfterMs != 0 && now < mNextAttackAfterMs;
    } else {
        throttled = (GetDelayMs() > 0.0f) && ((now - mLastBurst) < (unsigned long long)GetDelayMs());
    }

    // TICK GATE: OnUpdate is driven by the camera/render update (per frame, up to
    // hundreds of Hz), NOT the 20 Hz game tick. The vanilla attack transaction is
    // queued into the once-per-tick PlayerAuthInputPacket; queuing it more than
    // once per tick (or mid-flush) corrupts the outbound batch and the server
    // kicks us ("Failed to decode batch packet ... malformed packet"). We read
    // Actor::ticksExisted and refuse to swing/attack more than once per tick.
    long long curTick = lp->getTicksExisted();
    bool sameTick = (curTick >= 0 && curTick == mLastAttackTick);
    // NOTE: throttling gates only the actual swing/attack below — we still scan
    // and emit diagnostics every tick so the Debug feed stays informative even
    // at small delays (default 50ms would otherwise suppress most frames).

    // AntiBot: skip fake/decoy entities. Resolved once per burst; the module
    // returns false for everything while it is disabled, so this is a no-op
    // unless the user has AntiBot on.
    AntiBot* antiBot = static_cast<AntiBot*>(ModuleManager::GetInstance().GetModuleByName("AntiBot"));

    int rotMode = GetRotateMode(); // 0=None 1=Normal (continuous tracking) 2=Flick

    int attacked = 0;
    int players = 0, inRange = 0, posFail = 0, botSkip = 0;
    float nearest2 = 1e30f;

    // SINGLE-TARGET selection (Solstice/Latite "Single" mode). The previous
    // build swung + GameMode::attack()'d EVERY in-range player on the SAME game
    // tick. On a crowded server that stuffed many ItemUseOnEntity attack
    // transactions + many arm-swing AnimatePackets into one outbound RakNet
    // batch; the server's batch decoder then rejected the frame
    // ("Failed to decode batch packet ... Sent malformed packet") and kicked
    // us. Legitimate combat is one swing + one attack per tick, so we now pick
    // the CLOSEST in-range player and hit only that one.
    SDK::Actor* bestTarget = nullptr;
    float bestT2 = 1e30f;
    Vec3  bestTPos{};
    for (SDK::Actor* a : actors) {
        if (!a || a == lp) continue;
        if (!sdk_safe_ptr(a, Layout::Probe::Actor)) continue;
        if (!sehIsPlayer(a)) continue;
        players++;
        if (antiBot && antiBot->IsBot(a)) { botSkip++; continue; }

        // Skip the dead: their actors are on their way out, and the game's
        // attack path is not safe against them (death-screen crash).
        {
            const float hp = sehGetHealth(a);
            if (hp >= 0.0f && hp <= 0.0f) continue;
        }
        Vec3 p{};
        if (!sehGetPos(a, &p)) { posFail++; continue; }
        float dx = p.x - myPos.x, dy = p.y - myPos.y, dz = p.z - myPos.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < nearest2) nearest2 = d2;
        if (d2 > range2) continue;
        inRange++;
        if (d2 < bestT2) { bestT2 = d2; bestTPos = p; bestTarget = a; }
    }

    // RAYCAST BYPASS (Solstice "Raycast"): only hit what the view ray actually
    // reaches. If another in-range PLAYER's hitbox blocks the eye->target ray,
    // that player becomes the target (attacking what a real crosshair would
    // hit); a ray blocked by anything else means no attack this tick —
    // attacking through walls is the classic flag.
    if (bestTarget && GetRaycast()) {
        for (SDK::Actor* a : actors) {
            if (!a || a == lp || a == bestTarget) continue;
            if (!sdk_safe_ptr(a, Layout::Probe::Actor)) continue;
            if (!sehIsPlayer(a)) continue;
            if (antiBot && antiBot->IsBot(a)) continue;
            Vec3 p{};
            if (!sehGetPos(a, &p)) continue;
            float dx = p.x - myPos.x, dy = p.y - myPos.y, dz = p.z - myPos.z;
            if (dx * dx + dy * dy + dz * dz > range2) continue;
            Vec3 mn, mx;
            if (!sehGetActorBox(a, &mn, &mx)) continue;
            if (segIntersectsAABB(myPos, bestTPos, mn, mx)) {
                // This player's body is between us and the chosen target — the
                // vanilla crosshair would land on them.
                bestTarget = a;
                bestTPos = p;
                break;
            }
        }
    }

    // HURT TIME (Prax): vanilla invulnerability frames last 10 ticks (500 ms);
    // hitting the same target faster than any player could is both wasted
    // swings and a heuristic. While the target is in i-frames we hold the
    // attack (rotation keeps tracking).
    bool hurtGated = false;
    if (bestTarget && GetHurtTime()) {
        const unsigned int eid = GM::actorEntityId(bestTarget);
        auto it = mLastHitPerTarget.find(eid);
        if (it != mLastHitPerTarget.end() &&
            now - it->second < (unsigned long long)GetHurtTimeMs())
            hurtGated = true;
    }

    // WEAPON AUTO-SWITCH (Solstice parity). Full = switch-and-stay while we have
    // a target; OnAttack is handled at the attack site below. When we have no
    // target (or the mode is off) we restore the slot the player originally held
    // so we never strand them on a weapon slot.
    int switchMode = GetSwitchMode();       // 0=None 1=Full 2=OnAttack
    bool swordOnly = (GetWeaponMode() == 0);

    // Debug: one-shot dump of hotbar item names so we can verify the Item-name
    // read (and hence why weapon switching may not be triggering). Rate-limited to
    // ~once/sec via a local timer, and logged directly (bypassing auraDbg's shared
    // throttle) so all 9 rows print together.
    static unsigned long long s_hotbarDumpLast = 0;
    if (dbg && bestTarget && switchMode != 0 && (GetTickCount64() - s_hotbarDumpLast) > 1000) {
        s_hotbarDumpLast = GetTickCount64();
        SDK::Container* c = SDK::Inv::container(lp);
        if (c) {
            for (int s = 0; s < 9; ++s) {
                SDK::ItemStack* is = c->getItem(s);
                std::string nm = is ? is->getItemName() : std::string();
                if (is && nm.empty()) {
                    // Name resolution failed for this slot - dump raw memory
                    // layout so the correct Item::mName offset is visible.
                    LOG_INFO("hotbar[" + std::to_string(s) + "] " + is->debugNameInfo(), "Aura");
                } else {
                    LOG_INFO("hotbar[" + std::to_string(s) + "]='" + nm + "'", "Aura");
                }
            }
        }
        LOG_INFO("bestWeaponSlot=" + std::to_string(sehGetBestWeaponSlot(lp, swordOnly)) +
                     " selSlot=" + std::to_string(SDK::Inv::selectedSlot(lp)) +
                     " swordOnly=" + std::to_string((int)swordOnly), "Aura");
    }

    if (switchMode == 1 && bestTarget) {
        int best = sehGetBestWeaponSlot(lp, swordOnly);
        if (best >= 0) {
            int cur = SDK::Inv::selectedSlot(lp);
            if (cur != best) {
                if (!mDidFullSwitch) { mPrevSlot = cur; mDidFullSwitch = true; }
                SDK::Inv::setSelectedSlot(lp, best);
            }
        }
    } else if (mDidFullSwitch && (!bestTarget || switchMode != 1)) {
        // Lost target or Full mode turned off -> restore the original slot once.
        if (mPrevSlot >= 0 && mPrevSlot <= 8) SDK::Inv::setSelectedSlot(lp, mPrevSlot);
        mDidFullSwitch = false;
        mPrevSlot = -1;
    }

    // NORMAL (continuous) rotation: track the nearest target on EVERY update,
    // regardless of the attack throttle. This runs on the game-update thread
    // (UpdatePlayerFromCameraSystemUtil::_updatePlayer) — the SAME place Solstice
    // applies rotation just before the PlayerAuthInput flush.
    //
    // ROOT-CAUSE FIX for the "yaw jitters left/right when not facing the target":
    // the old build wrote the rotation from OnRender() (the D3D present thread),
    // which raced the game thread's own camera-rotation write every frame. When
    // your mouse yaw already matched the target the two writers agreed and it
    // looked fine; when you looked away they fought and the yaw oscillated. The
    // slower-changing pitch barely showed it, which is why "vertical was fine".
    // Applying rotation here (single writer, game thread) removes the race.
    if (rotMode == 1 && bestTarget) {
        // Closest hitbox point + small in-box jitter (not a hard single-point lock).
        Vec3 bmn, bmx, aim = bestTPos;
        bool haveBox = sehGetActorBox(bestTarget, &bmn, &bmx);
        if (haveBox) aim = closestPointOnBox(bmn, bmx, myPos);
        aim = jitterAimPoint(aim, bmn, bmx, haveBox);
        aimAt(myPos, aim, /*flick=*/false);
    }

    // Hit exactly ONE target this tick: only when the APS throttle allows AND we
    // have not already attacked on this game tick (tick gate above). The Hurt
    // Time gate holds the swing while the target is in invulnerability frames.
    if (bestTarget && !throttled && !sameTick && !hurtGated) {
        // Flick rotation: snap toward the target only at the instant of the hit.
        if (rotMode == 2) {
            Vec3 bmn, bmx, aim = bestTPos;
            bool haveBox = sehGetActorBox(bestTarget, &bmn, &bmx);
            if (haveBox) aim = closestPointOnBox(bmn, bmx, myPos);
            aim = jitterAimPoint(aim, bmn, bmx, haveBox);
            aimAt(myPos, aim, /*flick=*/true);
        }

        // OnAttack switch (Solstice Spoof-style): switch to the best weapon just
        // for this hit, then restore the previously-held slot right after. Only
        // in OnAttack mode; Full mode already switched-and-stays above.
        int onAtkPrevSlot = -1;
        bool onAtkSwitched = false;
        if (switchMode == 2) {
            int best = sehGetBestWeaponSlot(lp, swordOnly);
            if (best >= 0) {
                onAtkPrevSlot = SDK::Inv::selectedSlot(lp);
                if (onAtkPrevSlot != best) { SDK::Inv::setSelectedSlot(lp, best); onAtkSwitched = true; }
            }
        }

        // Prime the Level HitResult as an ENTITY hit so the serialized attack
        // transaction is consistent, swing FIRST (arm-swing animation), then
        // attack — the order Latite and Solstice use so the server registers
        // the hit rather than just a look. All gated to once per game tick.
        //
        // The click position handed to attack() is the point ON the target we
        // are nominally hitting. It is a REQUIRED third argument (see
        // GameMode::attack) — the game copies it into the outbound
        // ItemUseOnEntity transaction — so it is computed here rather than left
        // to whatever junk the register happened to hold.
        Vec3 clickMn, clickMx, clickPos = bestTPos;
        if (sehGetActorBox(bestTarget, &clickMn, &clickMx))
            clickPos = closestPointOnBox(clickMn, clickMx, myPos);

        // Interact bypass: the real input pipeline pairs an Interact(left-
        // click) with the attack transaction — send it first (Prax/Solstice).
        if (GetInteract()) sendInteractAttack(bestTarget, clickPos);

        GM::prepEntityHit(level);
        GM::swing(lp);
        if (GM::attack(gm, vt, bestTarget, &clickPos)) {
            attacked++;
            if (curTick >= 0) mLastAttackTick = curTick; // consume this tick
            else mLastAttackTick = -1;
            // Broadcast the landed hit so HUD modules (TargetHUD) can flash /
            // spawn hit particles.
            CombatSignal::registerHit(bestTarget);
            // Hurt-time bookkeeping: remember when this target was last hit.
            if (GetHurtTime()) {
                mLastHitPerTarget[GM::actorEntityId(bestTarget)] = now;
                if (mLastHitPerTarget.size() > 64) mLastHitPerTarget.clear();
            }
        }

        // Restore the pre-attack slot for OnAttack mode (silent client-side
        // switch — server sees the weapon for the attack transaction only).
        if (onAtkSwitched && onAtkPrevSlot >= 0 && onAtkPrevSlot <= 8) {
            SDK::Inv::setSelectedSlot(lp, onAtkPrevSlot);
        }
    }

    if (attacked > 0) {
        mLastBurst = now;
        // Re-draw the randomized attack interval for the NEXT hit.
        if (GetRandomizeAPS()) {
            float lo = GetAPSMin(), hi = GetAPSMax();
            if (hi < lo) std::swap(lo, hi);
            if (lo < 1.0f) lo = 1.0f;
            const float aps = SDK::randRange(lo, hi);
            mNextAttackAfterMs = now + (unsigned long long)(1000.0f / aps);
        } else {
            mNextAttackAfterMs = 0;
        }
    }

    auraDbg(dbg, "players=%d inRange=%d attacked=%d tick=%lld sameTick=%d nearest=%.1f range=%.1f posFail=%d botSkip=%d",
            players, inRange, attacked, curTick, (int)sameTick,
            (nearest2 < 1e29f ? std::sqrt(nearest2) : -1.0f), GetRange(), posFail, botSkip);
}

// The CORNER target visual: a bold diamond standing on its corner, centred on
// (cx, cy), with a SMALL GAP at the middle of every edge. Corners are drawn as
// JOINTED paths (mid-of-prev-edge -> vertex -> mid-of-next-edge) so the stroke
// fills the vertex itself, and each vertex is rounded by a tiny quadratic
// bezier through it. The stroke carries a flowing THEME GRADIENT (tinted per
// vertex like the ESP boxes) plus a soft two-pass GLOW hugging the edges, and
// the whole diamond swings on a slow bounce-rotation (left, a beat of rest,
// right, repeat).
// ---------------------------------------------------------------------------
// Bounce-rotation curve: rest at -1, ease to +1, rest, ease back. The result
// reads as the diamond swinging left, pausing, swinging right.
static float auraCornerSwing(float t) {
    const float period = 2.8f;
    float u = std::fmod(t, period);
    if (u < 0.0f) u += period;
    u /= period;
    const float kA = 10.0f * 3.14159265358979f / 180.0f;   // +/-10 degrees
    auto ease = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); };
    float s;
    if      (u < 0.15f) s = -1.0f;                          // rest, left
    else if (u < 0.40f) s = -1.0f + 2.0f * ease((u - 0.15f) / 0.25f);
    else if (u < 0.55f) s =  1.0f;                          // rest, right
    else if (u < 0.80f) s =  1.0f - 2.0f * ease((u - 0.55f) / 0.25f);
    else                 s = -1.0f;
    return kA * s;
}

static void drawCornerDiamond(ImDrawList* dl, float cx, float cy, float R, float th,
                              float alpha, float rot, uint64_t ms) {
    const float cs = std::cos(rot), sn = std::sin(rot);
    auto rotP = [&](ImVec2 p) {
        return ImVec2(cx + (p.x - cx) * cs - (p.y - cy) * sn,
                      cy + (p.x - cx) * sn + (p.y - cy) * cs);
    };
    const ImVec2 V[4] = {
        rotP(ImVec2(cx,     cy - R)),
        rotP(ImVec2(cx + R, cy)),
        rotP(ImVec2(cx,     cy + R)),
        rotP(ImVec2(cx - R, cy)),
    };
    // Tiny corner rounding: each vertex becomes a quadratic bezier with a
    // control point ON the vertex and end points a hair down each edge.
    const float rr = std::min(th * 0.75f, R * 0.16f);

    // One edge->vertex->edge path, emitted WHITE so its vertices can be
    // tinted in place with the vertical theme gradient (the ESP-box recipe:
    // scale each vertex's coverage alpha, never replace it).
    auto buildPath = [&](int k) {
        const ImVec2& P = V[(k + 3) & 3];
        const ImVec2& A = V[k];
        const ImVec2& B = V[(k + 1) & 3];
        const float exP = A.x - P.x, eyP = A.y - P.y;
        const float exN = B.x - A.x, eyN = B.y - A.y;
        const float lenP = std::sqrt(exP * exP + eyP * eyP);
        const float lenN = std::sqrt(exN * exN + eyN * eyN);
        if (lenP < 1.0f || lenN < 1.0f) return false;
        const float gapHalf = std::min(std::max(1.6f, th * 0.16f), std::min(lenP, lenN) * 0.10f);
        const ImVec2 M1((P.x + A.x) * 0.5f + exP / lenP * gapHalf,
                        (P.y + A.y) * 0.5f + eyP / lenP * gapHalf);
        const ImVec2 M2((A.x + B.x) * 0.5f - exN / lenN * gapHalf,
                        (A.y + B.y) * 0.5f - eyN / lenN * gapHalf);
        // Entry / exit points hugging the vertex, and the fall-off fast enough
        // to stay a whisper (rr is already clamped tiny).
        const float dIn  = std::min(rr, lenP * 0.45f);
        const float dOut = std::min(rr, lenN * 0.45f);
        const ImVec2 Pin (A.x - exP / lenP * dIn,  A.y - eyP / lenP * dIn);
        const ImVec2 Pout(A.x + exN / lenN * dOut, A.y + eyN / lenN * dOut);
        dl->PathLineTo(M1);
        dl->PathLineTo(Pin);
        dl->PathBezierQuadraticCurveTo(A, Pout, 8);
        dl->PathLineTo(M2);
        return true;
    };

    // Stroke every path once at `width` in white, then tint the freshly
    // emitted vertices with the theme gradient sampled at their own y.
    const float span = ColorUtils::getThemedPeriod() * 0.35f;
    const float yTop = cy - R, yBot = cy + R;
    auto strokeGradient = [&](float width, float aMul) {
        const int v0 = dl->VtxBuffer.Size;
        for (int k = 0; k < 4; ++k)
            if (buildPath(k)) dl->PathStroke(IM_COL32_WHITE, 0, width);
        const int v1 = dl->VtxBuffer.Size;
        for (int i = v0; i < v1; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            const float f = std::clamp((v.pos.y - yTop) / std::max(1.0f, yBot - yTop), 0.0f, 1.0f);
            const ImColor c = ColorUtils::getThemedColor(f * span, ms);
            const float cov = (float)((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
            v.col = ImGui::ColorConvertFloat4ToU32(
                ImVec4(c.Value.x, c.Value.y, c.Value.z, cov * aMul * alpha));
        }
    };

    // Edge glow: three gentle overdraw passes under the solid core — wider
    // and fainter each, so the falloff reads as a smooth halo, never a band.
    strokeGradient(th * 3.0f,  0.05f);
    strokeGradient(th * 2.1f,  0.08f);
    strokeGradient(th * 1.45f, 0.12f);
    strokeGradient(th,         1.00f);
}

// The shared ZOOM-OUT exit: replays the last cached geometry of the active
// visual mode, scaled DOWN into its own centre by the remaining mVisAlpha
// driver. Used both when the target is lost and when the module is toggled
// off (the always-render loop keeps calling OnRender for exactly this).
void Aura::renderExitZoom() {
    if (mVisAlpha <= 0.01f) { mVisTarget = nullptr; return; }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const float zoom = mVisAlpha;
    const float drawA = std::clamp(mVisAlpha * 4.0f, 0.0f, 1.0f);
    const uint64_t themeMs = ColorUtils::GetTimeMs();
    const float tNow = (float)ImGui::GetTime();
    if (mVisMode == 1 && mLastTh > 0.0f) {
        drawCornerDiamond(dl, mLastCenter.x, mLastCenter.y,
                          mLastR * zoom, std::max(mLastTh * zoom, 1.2f),
                          drawA, auraCornerSwing(tNow), themeMs);
    } else if (mVisMode == 2 && mRingSeg > 0) {
        const float span = ColorUtils::getThemedPeriod() * 0.5f;
        const float flow = (float)(themeMs % 60000ull) * 0.05f;
        auto sc = [&](const ImVec2& p) {
            return ImVec2(mRingCenter.x + (p.x - mRingCenter.x) * zoom,
                          mRingCenter.y + (p.y - mRingCenter.y) * zoom);
        };
        for (int i = 0; i < mRingSeg; ++i) {
            if (!mRingVis[i] || !mRingVis[i + 1]) continue;
            const ImVec2 a = sc(mRingTop[i]), b = sc(mRingTop[i + 1]);
            const ImVec2 c1 = sc(mRingBot[i]), d1 = sc(mRingBot[i + 1]);
            ImColor col = ColorUtils::getThemedColor(
                ((float)i / mRingSeg) * span + flow, themeMs);
            col.Value.w = (63.f / 255.f) * drawA;
            dl->AddQuadFilled(a, b, d1, c1, (ImU32)col);
            col.Value.w = 0.18f * drawA; dl->AddLine(a, b, (ImU32)col, 3.4f);
            col.Value.w = drawA;         dl->AddLine(a, b, (ImU32)col, 1.2f);
        }
    }
    if (mMorphT < 1.0f) mMorphT = 1.0f;
    if (mVisAlpha <= 0.015f) { mVisTarget = nullptr; mRingSeg = 0; mLastTh = 0.0f; }
}


// Rotation (both Normal continuous tracking and Flick) is now applied inside
// OnUpdate(), which runs on the game-update thread
// (UpdatePlayerFromCameraSystemUtil::_updatePlayer) — the correct single-writer
// point, matching Solstice. Writing rotation from here (the D3D present thread)
// raced the game thread and caused the horizontal-yaw jitter, so OnRender never
// touches rotation. What OnRender DOES draw is the optional target visual.
void Aura::OnRender() {
    const int visMode = GetTargetVisual();
    if (visMode == 0) {                       // 0 = None
        mVisAlpha = 0.0f;
        mVisTarget = nullptr;
        return;
    }
    if (visMode != mVisMode) {                // style switch: restart the fade
        mVisMode = visMode;
        mVisAlpha = 0.0f;
        mVisTarget = nullptr;
        mMorphT = 1.0f;
    }
    // Disabled: the visual still plays its fast zoom-out exit from the cached
    // geometry instead of popping away (Aura is in the always-render loop
    // precisely for this).
    if (!IsEnabled()) {
        if (mVisAlpha > 0.001f) {
            float dtD = ImGui::GetIO().DeltaTime;
            if (dtD <= 0.0f || dtD > 0.25f) dtD = 1.0f / 60.0f;
            mVisAlpha += (0.0f - mVisAlpha) * std::min(1.0f, dtD * 20.0f);
            if (mVisAlpha < 0.002f) mVisAlpha = 0.0f;
            renderExitZoom();
        } else {
            mVisTarget = nullptr;
        }
        return;
    }
    if (!SDK::isGameplayInputActive()) return;

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;

    SDK::ClientInstance* ci = nullptr; SDK::Level* level = nullptr; SDK::Actor* lp = nullptr;
    if (!SDK::resolveContext(&ci, &level, &lp)) return;

    // Re-select the nearest in-range player HERE (render thread) instead of
    // caching an Actor* across threads — a stale pointer is a crash waiting.
    const float range = GetRange();
    const float range2 = range * range;
    Vec3 myPos{};
    if (!sehGetPos(lp, &myPos)) return;
    std::vector<SDK::Actor*> actors;
    if (!sehGetActorList(level, &actors)) return;
    SDK::Actor* target = nullptr;
    float bestT2 = range2;
    // Same AntiBot filter as OnUpdate — otherwise the visual draws on bots
    // the attack logic correctly ignores.
    AntiBot* antiBot = static_cast<AntiBot*>(ModuleManager::GetInstance().GetModuleByName("AntiBot"));
    for (SDK::Actor* a : actors) {
        if (!a || a == lp) continue;
        if (!sdk_safe_ptr(a, Layout::Probe::Actor)) continue;
        if (!sehIsPlayer(a)) continue;
        if (antiBot && antiBot->IsBot(a)) continue;
        Vec3 p{};
        if (!sehGetPos(a, &p)) continue;
        float dx = p.x - myPos.x, dy = p.y - myPos.y, dz = p.z - myPos.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestT2) { bestT2 = d2; target = a; }
    }

    // ---- target-visual animation state machine ----------------------------
    // Acquire -> fade in. Lost -> fade out (drawing the cached geometry).
    // Switch -> morph the anchor from the old target's position to the new
    // one over 0.22 s instead of teleporting.
    if (target != mVisTarget) {
        if (target && mVisTarget && mVisAlpha > 0.05f) {
            // Switch: capture the previous target's anchor (screen-space for
            // Corner, world-space for Rings) and blend from it.
            mMorphFrom = mLastCenter;
            mMorphFromR = mLastR;
            mMorphFromWp = mLastWorldRp;
            mMorphT = 0.0f;
        } else if (target) {
            mMorphT = 1.0f;   // fresh acquire from nothing
        }
        mVisTarget = target;
    }
    const float alphaGoal = target ? 1.0f : 0.0f;
    // Acquire fades in gently; LOSE drives a fast ZOOM-OUT instead of a fade
    // — the driver doubles as the scale, the visual collapses into its centre
    // over ~130 ms and vanishes.
    const float visEase = std::min(1.0f, dt * (target ? 11.0f : 20.0f));
    mVisAlpha += (alphaGoal - mVisAlpha) * visEase;
    if (mVisAlpha < 0.002f) mVisAlpha = 0.0f;
    if (mMorphT < 1.0f) mMorphT = std::min(1.0f, mMorphT + dt / 0.22f);
    const float morphEase = 1.0f - (1.0f - mMorphT) * (1.0f - mMorphT) * (1.0f - mMorphT);

    // Collapsed fully -> nothing left to draw.
    if (mVisAlpha <= 0.01f) { mVisTarget = nullptr; return; }

    // ---- ZOOM-OUT exit: target lost (or the module just toggled off) ->
    //      replay the last cached geometry, scaled DOWN into its own centre
    //      by the remaining driver. The alpha stays full until the shape is
    //      nearly gone (a crisp shrink, not a fade-out).
    if (!target) {
        renderExitZoom();
        return;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;

    Vec3 mn, mx;
    if (!sehGetActorBox(target, &mn, &mx)) return;
    // Interp rebase (the ESP recipe): the raw AABB is server-tick-stepped, so
    // projecting it directly jitters against the ~60 fps camera — the ESP
    // boxes are smooth because they shift the box by the interpolated
    // render-position delta first. Same fix here.
    {
        Vec3 apos{}, rp = SDK::interpRenderPos(target);
        if (sehGetPos(target, &apos)) {
            const float dx = rp.x - apos.x, dy = rp.y - apos.y, dz = rp.z - apos.z;
            mn = Vec3(mn.x + dx, mn.y + dy, mn.z + dz);
            mx = Vec3(mx.x + dx, mx.y + dy, mx.z + dz);
        }
    }
    const Vec3 corners[8] = {
        { mn.x, mn.y, mn.z }, { mn.x, mn.y, mx.z },
        { mn.x, mx.y, mn.z }, { mn.x, mx.y, mx.z },
        { mx.x, mn.y, mn.z }, { mx.x, mn.y, mx.z },
        { mx.x, mx.y, mn.z }, { mx.x, mx.y, mx.z },
    };
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (int i = 0; i < 8; ++i) {
        ImVec2 s;
        if (!worldToScreen(corners[i], mvp, origin, screen, &s)) continue;
        if (s.x < minX) minX = s.x;
        if (s.y < minY) minY = s.y;
        if (s.x > maxX) maxX = s.x;
        if (s.y > maxY) maxY = s.y;
    }
    if (maxX - minX < 2.0f || maxY - minY < 2.0f) return;

    // ---- RINGS (Rise SigmaRing, verbatim parameters) -----------------------
    // A flowing gradient ring around the target's body, sweeping bottom→top.
    // Fades with mVisAlpha (acquire / lose / switch) and MORPHS its world
    // anchor when the target jumps to another player.
    if (visMode == 2) {
        const double nowMs = (double)GetTickCount64();
        const float width = std::max(mx.x - mn.x, 0.1f);
        const float height = std::clamp(mx.y - mn.y, 1.5f, 2.2f);
        // Anchor: the target's interpolated render position (push-time pair +
        // served alpha — the frame the presented model was drawn at) so the
        // ring glues to the model even while it moves.
        Vec3 rp = SDK::interpRenderPos(target);
        const float delta = rp.y - mn.y;
        if (delta > 0.8f && delta < 2.2f) rp.y -= 1.62f;
        // Switch morph: blend the world anchor from the previous target.
        if (mMorphT < 1.0f) {
            rp.x = mMorphFromWp.x + (rp.x - mMorphFromWp.x) * morphEase;
            rp.y = mMorphFromWp.y + (rp.y - mMorphFromWp.y) * morphEase;
            rp.z = mMorphFromWp.z + (rp.z - mMorphFromWp.z) * morphEase;
        }
        const float radius = width * 1.1f;
        const float bob = (float)std::sin(nowMs / 200.0);
        // Rise: d3 = height*0.5445 + sin(t/200) — the ring line sweeps from
        // the feet (sin=-1) up over the head (sin=+1). Feet→head, "снизу вверх".
        const float yTop = rp.y + height * 0.5445f + bob;
        const float stripH = 0.25f + 0.25f * (float)std::cos(nowMs / 200.0);   // 0..0.5, always below
        const float yBot = yTop - stripH;

        const int SEG = 64;
        bool vis[SEG + 1];
        int visible = 0;
        for (int i = 0; i <= SEG; ++i) {
            const double a = (double)i * ((3.14159265358979323846 * 2.0) / SEG);
            const Vec3 pt(rp.x + radius * (float)std::cos(a), yTop,
                         rp.z + radius * (float)std::sin(a));
            const Vec3 pb(rp.x + radius * (float)std::cos(a), yBot,
                         rp.z + radius * (float)std::sin(a));
            ImVec2 st, sb;
            vis[i] = worldToScreen(pt, mvp, origin, screen, &st) &&
                     worldToScreen(pb, mvp, origin, screen, &sb);
            mRingTop[i] = st; mRingBot[i] = sb; mRingVis[i] = vis[i];
            if (vis[i]) ++visible;
        }
        mRingSeg = SEG;
        mLastWorldRp = rp;
        // Screen-space centre of the ring (the zoom-out exit scales the
        // cached points toward it).
        worldToScreen(Vec3(rp.x, (yTop + yBot) * 0.5f, rp.z), mvp, origin, screen, &mRingCenter);
        if (visible < 8) return;

        // Colour: the live theme palette swept AROUND the ring (a half cycle
        // per revolution) plus a slow flow, so the whole circumference reads
        // as one continuous gradient instead of a two-tone wobble.
        const uint64_t themeMs = ColorUtils::GetTimeMs();
        const float span = ColorUtils::getThemedPeriod() * 0.5f;
        const float flow = (float)(themeMs % 60000ull) * 0.05f;
        for (int i = 0; i < SEG; ++i) {
            if (!vis[i] || !vis[i + 1]) continue;
            ImColor col = ColorUtils::getThemedColor(
                ((float)i / SEG) * span + flow, themeMs);
            // Curtain fill: the strip below the line, fading with mVisAlpha.
            col.Value.w = (63.f / 255.f) * mVisAlpha;
            dl->AddQuadFilled(mRingTop[i], mRingTop[i + 1], mRingBot[i + 1], mRingBot[i], (ImU32)col);
        }
        // Bright outline along the top edge wrapped in a soft three-layer
        // glow — wider and fainter each pass, so the falloff reads smooth.
        for (int i = 0; i < SEG; ++i) {
            if (!vis[i] || !vis[i + 1]) continue;
            ImColor col = ColorUtils::getThemedColor(
                ((float)i / SEG) * span + flow, themeMs);
            col.Value.w = 0.16f * mVisAlpha;
            dl->AddLine(mRingTop[i], mRingTop[i + 1], (ImU32)col, 5.5f);
            col.Value.w = 0.22f * mVisAlpha;
            dl->AddLine(mRingTop[i], mRingTop[i + 1], (ImU32)col, 3.6f);
            col.Value.w = 0.28f * mVisAlpha;
            dl->AddLine(mRingTop[i], mRingTop[i + 1], (ImU32)col, 2.2f);
            col.Value.w = mVisAlpha;
            dl->AddLine(mRingTop[i], mRingTop[i + 1], (ImU32)col, 1.6f);
        }
        return;
    }

    // ---- Shared screen anchor for the Corner / Orbit modes -----------------
    const float t = (float)ImGui::GetTime();
    const float breathe = 1.0f + 0.02f * std::sin(t * 3.0f);
    float cx = (minX + maxX) * 0.5f;
    float cy = (minY + maxY) * 0.5f;
    const float boxW = maxX - minX, boxH = maxY - minY;

    // ---- CORNER: a BOLD DIAMOND centred on the player ----------------------
    // One very thick square outline standing on its corner, centred on the
    // model, with a SMALL GAP at the MIDDLE of every edge. The stroke carries
    // a flowing theme GRADIENT, a soft edge GLOW, whisper-rounded corners, and
    // swings on the bounce-rotation.
    // SCALE WINDOW: clamp the diamond into [min, max] pixels. Up close the
    // projected box explodes (or collapses when the near-plane eats corners),
    // which read as a huge hair-thin diamond — the window keeps it a FAT,
    // consistent marker at every distance.
    float R = (std::max(boxW, boxH) * 0.5f * 0.52f + 2.0f) * breathe;
    if (R < 64.0f) R = 64.0f;
    if (R > 150.0f) R = 150.0f;
    // Switch morph: blend the screen anchor and size from the previous target.
    if (mMorphT < 1.0f) {
        cx = mMorphFrom.x + (cx - mMorphFrom.x) * morphEase;
        cy = mMorphFrom.y + (cy - mMorphFrom.y) * morphEase;
        R  = mMorphFromR + (R - mMorphFromR) * morphEase;
    }

    const uint64_t nowMs2 = ColorUtils::GetTimeMs();

    // Heavier stroke, scaling with the clamped radius, so the diamond stays
    // BOLD even at the window's top.
    const float th = std::clamp(R * 0.34f, 11.0f, 24.0f);
    drawCornerDiamond(dl, cx, cy, R, th, mVisAlpha,
                      auraCornerSwing(t), nowMs2);

    // Cache for the zoom-out exit and the next switch morph.
    mLastCenter = ImVec2(cx, cy);
    mLastR = R;
    mLastTh = th;
}

// Stop injecting the moment Aura is toggled off, otherwise the last continuous
// aim would keep being written into outgoing packets. Also restore the slot if
// Full weapon-switch left the player on a weapon.
void Aura::OnDisable() {
    SilentRotation::Clear(true);
    mNextAttackAfterMs = 0;
    mLastHitPerTarget.clear();
    if (mDidFullSwitch && mPrevSlot >= 0 && mPrevSlot <= 8) {
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
        if (lp) SDK::Inv::setSelectedSlot(lp, mPrevSlot);
    }
    mDidFullSwitch = false;
    mPrevSlot = -1;
}
