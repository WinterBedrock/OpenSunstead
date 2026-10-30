#include "OreMiner.h"
#include "../../SDK/Client/GameIcon.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Render/BoxOverlay.h"
#include "../Setting.h"
#include "../ModuleManager.h"
#include "../Combat/SilentRotation.h"
#include "../../Core/Logger.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../SDK/Client/BlockTextures.h"
#include <cstdio>
#include "../../Render/FrameView.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../SDK/Network/PacketUtils.h"
#include <cmath>
#include <cstring>
#include <cctype>
#include <cstdarg>
#include <string>
#include <vector>
#include <Windows.h>
#include <imgui.h>

using SDK::sehGetPos;
using SDK::IVec3;
namespace GM = SDK::GameMode;

SUNSTEAD_DEFINE_DBG(omDbg, "OreMiner", 300)

// Vanilla survival reach — the server rejects block interactions beyond this distance from the eye
static constexpr float kServerReach = 5.4f;

// True when the eye-to-block-center distance is within the vanilla reach band that the server actually accepts
static bool sehWithinServerReach(const Vec3& eye, const SDK::BlockPos& p) {
    const float cx = (float)p.x + 0.5f - eye.x;
    const float cy = (float)p.y + 0.5f - eye.y;
    const float cz = (float)p.z + 0.5f - eye.z;
    return (cx * cx + cy * cy + cz * cz) <= kServerReach * kServerReach;
}

// Ore classification by block name, 1.26 uses names, ids drift
// kind: 0 emerald 1 diamond 2 gold 3 iron 4 coal 5 redstone 6 lapis 7 debris
bool OreMiner::isOreName(const std::string& raw, int kind) const {
    std::string n = raw;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    // strip namespace
    auto colon = n.find(':');
    if (colon != std::string::npos) n = n.substr(colon + 1);
    switch (kind) {
    case 0: return n.find("emerald_ore") != std::string::npos;
    case 1: return n.find("diamond_ore") != std::string::npos;
    case 2: return n.find("gold_ore") != std::string::npos;
    case 3: return n.find("iron_ore") != std::string::npos;
    case 4: return n.find("coal_ore") != std::string::npos;
    case 5: return n.find("redstone_ore") != std::string::npos || n.find("lit_redstone_ore") != std::string::npos;
    case 6: return n.find("lapis_ore") != std::string::npos;
    case 7: return n.find("ancient_debris") != std::string::npos || n.find("debris") != std::string::npos;
    default: return false;
    }
}

// POD-only SEH helper
static int sehExposedFace(SDK::BlockSource* src, int x, int y, int z) {
    if (!src) return -1;
    static const int nd[6][4] = {
        { 0,-1, 0, 1 }, { 0, 1, 0, 0 },
        { 0, 0,-1, 3 }, { 0, 0, 1, 2 },
        {-1, 0, 0, 5 }, { 1, 0, 0, 4 },
    };
    __try {
        for (int i = 0; i < 6; ++i) {
            SDK::BlockPos np(x + nd[i][0], y + nd[i][1], z + nd[i][2]);
            if (src->isAir(np)) return nd[i][3];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

// The engine's break-progress accumulator (gm+0x24)
static bool sehWriteBreakProgress(void* gm, float v) {
    if (!gm) return false;
    __try {
        *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(gm) + ::Layout::GameMode::BreakProgress) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// GameMode::getDestroyRate / the destroy transactions -> Actor::getCarriedItem
// (vtable[77], sub_140203320 on 1.26.50.04) reads PlayerInventory::mSelectedSlot
// (supplies+0x10, a HOTBAR index 0..8) and returns container->getItem(that
// index). In SILENT mode the visible selection never moves, so the engine
// would compute rates and build destroy transactions for whatever the player
// visibly holds (fist speed / a sword - the server then rejects the break).
//
// Fix: point mSelectedSlot/mInHandSlot at the tool's hotbar slot for the
// duration of the engine calls (rate, startDestroy, destroyBlock, stopDestroy)
// and put the old values back right after. Unlike the old 0x80-byte ItemStack
// swap this keeps slot index AND item consistent with the MobEquipment the
// server got (transaction slot == server-held slot == tool), moves no item
// memory at all, and sends nothing. Game thread only, no render in between.
struct HeldSlotSave { int sel = -1; int inHand = -1; bool active = false; };

static bool sehHoldSlot(SDK::Actor* lp, int toolSlot, HeldSlotSave* save) {
    if (!save) return false;
    save->active = false;
    if (!lp || toolSlot < 0 || toolSlot > 8) return false;
    __try {
        const uintptr_t sup = (uintptr_t)lp + ::Layout::Actor::Supplies;
        if (!sdk_safe_ptr((void*)sup, sizeof(void*))) return false;
        const uintptr_t supplies = *(uintptr_t*)sup;
        if (!supplies || !sdk_safe_ptr((void*)supplies, ::Layout::PlayerInv::InHandSlot + sizeof(int)))
            return false;
        int* pSel = (int*)(supplies + ::Layout::PlayerInv::SelectedSlot);
        int* pHand = (int*)(supplies + ::Layout::PlayerInv::InHandSlot);
        if (*pSel < 0 || *pSel > 8) return false;
        if (*pSel == toolSlot && *pHand == toolSlot) return false;  // already held
        save->sel = *pSel;
        save->inHand = *pHand;
        *pSel = toolSlot;
        *pHand = toolSlot;
        save->active = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void sehRestoreSlot(SDK::Actor* lp, HeldSlotSave* save) {
    if (!save || !save->active || !lp) return;
    save->active = false;
    __try {
        const uintptr_t supplies = *(uintptr_t*)((uintptr_t)lp + ::Layout::Actor::Supplies);
        if (!supplies) return;
        *(int*)(supplies + ::Layout::PlayerInv::SelectedSlot) = save->sel;
        *(int*)(supplies + ::Layout::PlayerInv::InHandSlot) = save->inHand;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int OreMiner::getExposedFace(const SDK::BlockPos& pos) const {
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return -1;
    return sehExposedFace(ci->getBlockSource(), pos.x, pos.y, pos.z);
}

bool OreMiner::isValidOre(const SDK::BlockPos& pos, bool oreOnly, bool exposedOnly) const {
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return false;
    SDK::BlockSource* src = ci->getBlockSource();
    if (!src) return false;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return false;

    SDK::Block* block = src->getBlock(pos);
    if (!block) return false;
    SDK::BlockLegacy* leg = block->getLegacy();
    if (!leg || leg->isAir()) return false;

    if (oreOnly) {
        std::string name = leg->getName();
        if (name.empty()) return false;
        bool isOre = false;
        for (int kind = 0; kind < 8; ++kind) {
            if (!static_cast<BoolSetting*>(mSettings[4 + kind])->mValue) continue;
            if (isOreName(name, kind)) { isOre = true; break; }
        }
        if (!isOre) return false;
    }

    // Distance (closest point on block AABB to player pos)
    Vec3 eye{};
    if (!sehGetPos(lp, &eye)) return false;
    Vec3 bmn((float)pos.x, (float)pos.y, (float)pos.z);
    Vec3 bmx = bmn + Vec3(1, 1, 1);
    Vec3 closest = eye;
    if (closest.x < bmn.x) closest.x = bmn.x; else if (closest.x > bmx.x) closest.x = bmx.x;
    if (closest.y < bmn.y) closest.y = bmn.y; else if (closest.y > bmx.y) closest.y = bmx.y;
    if (closest.z < bmn.z) closest.z = bmn.z; else if (closest.z > bmx.z) closest.z = bmx.z;
    float dx = closest.x - eye.x, dy = closest.y - eye.y, dz = closest.z - eye.z;
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist > GetRange()) return false;

    if (exposedOnly && getExposedFace(pos) < 0) return false;
    return true;
}

int OreMiner::findBestPickaxe(SDK::Actor* lp) const {
    if (!lp) return -1;
    SDK::Container* c = SDK::Inv::container(lp);
    if (!c) return -1;

    // Silent mode can only make the SERVER hold a hotbar slot (MobEquipment
    // carries a 0..8 index) - a pickaxe in the main inventory would be mined
    // with whatever the server thinks is in hand and every break rejected.
    const bool silent = (GetMode() == 1 && GetSilentSlotSwitch());
    int maxSlot = (GetHotbarOnly() || silent) ? 9 : 36;
    int best = -1, bestTier = -1;
    for (int s = 0; s < maxSlot; ++s) {
        SDK::ItemStack* is = c->getItem(s);
        if (!is) continue;
        std::string n = is->getItemName();
        if (n.empty()) continue;
        for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
        if (n.find("pickaxe") == std::string::npos) continue;
        int tier = 1;
        if (n.find("netherite") != std::string::npos) tier = 6;
        else if (n.find("diamond") != std::string::npos) tier = 5;
        else if (n.find("iron") != std::string::npos) tier = 4;
        else if (n.find("stone") != std::string::npos) tier = 3;
        else if (n.find("gold") != std::string::npos) tier = 2;
        if (tier > bestTier) { bestTier = tier; best = s; }
    }
    return best;
}

bool OreMiner::getBestPathToBlock(const SDK::BlockPos& ore, SDK::BlockPos* out) const {
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !out) return false;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return false;
    Vec3 eye{};
    if (!sehGetPos(lp, &eye)) return false;

    int r = (int)GetUncoverRange();
    float best = 1e30f;
    bool found = false;
    SDK::BlockPos bestPos{};

    for (int x = ore.x - r; x <= ore.x + r; ++x)
    for (int y = ore.y - r; y <= ore.y + r; ++y)
    for (int z = ore.z - r; z <= ore.z + r; ++z) {
        SDK::BlockPos p(x, y, z);
        SDK::BlockSource* src = ci->getBlockSource();
        if (!src || src->isAir(p)) continue;
        if (getExposedFace(p) < 0) continue;
        float dx = (float)x + 0.5f - eye.x;
        float dy = (float)y + 0.5f - eye.y;
        float dz = (float)z + 0.5f - eye.z;
        float dPlayer = std::sqrt(dx * dx + dy * dy + dz * dz);
        float dx2 = (float)(x - ore.x), dy2 = (float)(y - ore.y), dz2 = (float)(z - ore.z);
        float dOre = std::sqrt(dx2 * dx2 + dy2 * dy2 + dz2 * dz2);
        float score = dPlayer + dOre;
        if (score < best) { best = score; bestPos = p; found = true; }
    }
    if (!found) return false;
    *out = bestPos;
    return true;
}

void OreMiner::reset() {
    if (mIsMining) {
        SDK::GameContext ctx;
        if (SDK::resolveContext(ctx, false)) {
            void** vt = nullptr;
            void* gm = GM::resolve(ctx.lp, &vt);
            if (gm && vt) {
                IVec3 p{ mCurrentBlock.x, mCurrentBlock.y, mCurrentBlock.z };
                GM::stopDestroy(gm, vt, p);
            }
        }
    }
    // Restore the public slot flip (Default mode). Silent mode never touched
    // the local selection — it only re-informs the server of the player's
    // REAL slot so the tool serverhold ends.
    SDK::GameContext ctx;
    if (SDK::resolveContext(ctx, false))
        restoreSlot(ctx.lp);
    mPrevSlot = -1;
    mLastLocalSlot = -1;

    mCurrentBlock = { INT_MAX, INT_MAX, INT_MAX };
    mTargettingBlock = { INT_MAX, INT_MAX, INT_MAX };
    mCurrentFace = -1;
    mBreakingProgress = 0.f;
    mLastRate = 0.f;
    mToolSlot = -1;
    mStallTicks = 0;
    s_currentBlock = mCurrentBlock;
    s_isMining = false;
    mIsMining = false;
    mIsUncovering = false;
}

void OreMiner::queueBlock(const SDK::BlockPos& pos, int face) {
    SDK::GameContext ctx;
    SDK::resolveContext(ctx, true);
    void** vt = nullptr;
    void* gm = ctx.lp ? GM::resolve(ctx.lp, &vt) : nullptr;

    mCurrentBlock = pos;
    mCurrentFace = face;
    if (mCurrentFace < 0) mCurrentFace = 0;
    mIsMining = true;
    mBreakingProgress = 0.f;
    mLastRate = 0.f;

    mToolSlot = findBestPickaxe(ctx.lp);
    if (mToolSlot >= 0 && ctx.lp) {
        if (mPrevSlot < 0) mPrevSlot = SDK::Inv::selectedSlot(ctx.lp);
        if (GetSilentSlotSwitch()) {
            if (GetMode() == 1) {
                ensureServerHoldsTool(ctx.lp);
            } else {
                SDK::Inv::setSelectedSlot(ctx.lp, mToolSlot);
            }
        }
        // Silent Slot Switch off: leave the public selection untouched — the
        // break is slower (server attributes it to the visibly held item), but
        // nothing flashes in the hotbar and nothing goes out.
    }

    // One startDestroy registers the target in the engine (gm+0x10/0x1C) and emits the vanilla-shaped outbound transaction
    IVec3 ip{ pos.x, pos.y, pos.z };
    GM::prepBlockHit(ctx.level, ip, mCurrentFace);
    if (gm && vt) {
        // Silent: swap the tool into the carried slot so the start-destroy
        // transaction carries the pickaxe, not the visibly held item.
        HeldSlotSave hold;
        if (GetMode() == 1 && GetSilentSlotSwitch() && mToolSlot >= 0 && ctx.lp)
            sehHoldSlot(ctx.lp, mToolSlot, &hold);
        GM::startDestroy(gm, vt, ip, mCurrentFace);
        sehRestoreSlot(ctx.lp, &hold);
    }

    if (GetRotate()) mShouldRotate = true;
    s_currentBlock = mCurrentBlock;
    s_isMining = true;
}

bool OreMiner::GetShowSwing()   const {
    return mSettings.size() > 26
        ? static_cast<BoolSetting*>(mSettings[26])->mValue : false;
}
bool OreMiner::GetSilentSlotSwitch() const {
    return mSettings.size() > 27
        ? static_cast<BoolSetting*>(mSettings[27])->mValue : true;
}
// Silent: make sure the SERVER believes the tool slot is held. The local
// selection may have been changed by the player (scroll / number key) - the
// game then sent its own MobEquipment, so the server holds THAT slot now.
void OreMiner::ensureServerHoldsTool(SDK::Actor* lp) {
    if (!lp || mToolSlot < 0 || mToolSlot > 8) return;
    const int local = SDK::Inv::selectedSlot(lp);
    if (local >= 0 && local != mLastLocalSlot) {
        if (mLastLocalSlot >= 0) mServerSlot = local;  // game re-synced the server
        mLastLocalSlot = local;
    }
    if (mServerSlot != mToolSlot) {
        if (SDK::PacketUtils::sendMobEquipment(lp, mToolSlot))
            mServerSlot = mToolSlot;
    }
}

// Undo the tool hold. Default: flip the visible slot back. Silent: tell the
// server the player's REAL current slot again (read fresh - the player may
// have scrolled while mining).
void OreMiner::restoreSlot(SDK::Actor* lp) {
    if (lp && GetSilentSlotSwitch()) {
        if (GetMode() == 1) {
            const int real = SDK::Inv::selectedSlot(lp);
            if (real >= 0 && mServerSlot >= 0 && mServerSlot != real) {
                if (SDK::PacketUtils::sendMobEquipment(lp, real))
                    mServerSlot = real;
            }
        } else if (mPrevSlot >= 0) {
            SDK::Inv::setSelectedSlot(lp, mPrevSlot);
        }
    }
    mPrevSlot = -1;
    mLastLocalSlot = -1;
}

void OreMiner::OnEnable() {
    reset();
    mShouldRotate = false;
    mServerSlot = -1;
}

void OreMiner::OnDisable() {
    // Slot restore is done in reset(); OnDisable just clears the aim channel
    // so a pending silent rotation cannot poison the next frame.
    reset();
    SilentRotation::Clear(true);
    mPrevSlot = -1;
    mServerSlot = -1;
}

void OreMiner::OnUpdate() {
    if (!IsEnabled()) return;
    bool dbg = GetDebug();

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx, true)) return;
    SDK::ClientInstance* ci = ctx.ci;
    SDK::Actor* lp = ctx.lp;
    SDK::Level* level = ctx.level;

    // One action per game tick.
    long long curTick = lp->getTicksExisted();
    if (curTick >= 0) {
        if (curTick == mLastTick) {
            // still re-arm continuous rotation every frame while mining
            if (mIsMining && GetRotate() && mShouldRotate) {
                Vec3 eye{};
                if (sehGetPos(lp, &eye)) {
                    Vec3 aim((float)mCurrentBlock.x + 0.5f,
                             (float)mCurrentBlock.y + 0.5f,
                             (float)mCurrentBlock.z + 0.5f);
                    float dx = aim.x - eye.x, dy = aim.y - eye.y, dz = aim.z - eye.z;
                    float horiz = std::sqrt(dx * dx + dz * dz);
                    if (horiz > 1e-4f || std::fabs(dy) > 1e-4f) {
                        const float R2D = 57.2957795f;
                        float yaw = std::atan2(dz, dx) * R2D - 90.0f;
                        float pitch = -std::atan2(dy, horiz) * R2D;
                        if (pitch > 89.9f) pitch = 89.9f;
                        if (pitch < -89.9f) pitch = -89.9f;
                        while (yaw > 180.f) yaw -= 360.f;
                        while (yaw < -180.f) yaw += 360.f;
                        SilentRotation::SetContinuous(yaw, pitch);
                        const bool w_ = (GetAsyncKeyState('W') & 0x8000) != 0;
                        const bool a_ = (GetAsyncKeyState('A') & 0x8000) != 0;
                        const bool s_ = (GetAsyncKeyState('S') & 0x8000) != 0;
                        const bool d_ = (GetAsyncKeyState('D') & 0x8000) != 0;
                        SilentRotation::SetMoveInput((w_ ? 1 : 0) - (s_ ? 1 : 0), (d_ ? 1 : 0) - (a_ ? 1 : 0));
                        SilentRotation::PublishMoveFrame(lp->getRot().y);
                    }
                }
            }
            return;
        }
        // Real elapsed time between game ticks, aka engine style
        ULONGLONG nowMs = GetTickCount64();
        if (mLastTickTimeMs > 0) {
            mTickDelta = (float)(nowMs - mLastTickTimeMs) / 1000.f;
        }
        mLastTickTimeMs = nowMs;
        mLastTick = curTick;
    }

    // dont fight Scaffold
    if (Module* sc = ModuleManager::GetInstance().GetModuleByName("Scaffold")) {
        if (sc->IsEnabled()) { reset(); return; }
    }

    void** vt = nullptr;
    void* gm = GM::resolve(lp, &vt);
    if (!gm || !vt) { omDbg(dbg, "GameMode FAIL"); return; }

    SDK::BlockSource* src = ci->getBlockSource();
    if (!src) return;

    SilentRotation::Clear(false);

    // Continue current target if still valid
    bool oreOnlyForCurrent = (GetUncoverMode() == 0) || !mIsUncovering;
    if (mIsMining) {
        Vec3 eyeReach{};
        if (!sehGetPos(lp, &eyeReach) || !sehWithinServerReach(eyeReach, mCurrentBlock)) {
            omDbg(dbg, "target out of server reach, dropping");
            reset();
            mLastTick = curTick;   // consume this tick
            return;
        }
    }
    if (mIsMining && isValidOre(mCurrentBlock, oreOnlyForCurrent, !mIsUncovering)) {
        if (GetRotate()) mShouldRotate = true;

        // Keep the tool held. Default: the visible selection. Silent: the
        // SERVER-side selection only - re-sent whenever the player scrolled
        // (the game then told the server about the new slot) or anything
        // else moved it off the tool.
        if (mToolSlot >= 0 && GetSilentSlotSwitch()) {
            if (GetMode() == 1) ensureServerHoldsTool(lp);
            else SDK::Inv::setSelectedSlot(lp, mToolSlot);
        }

        IVec3 ip{ mCurrentBlock.x, mCurrentBlock.y, mCurrentBlock.z };
        GM::prepBlockHit(level, ip, mCurrentFace);

        // Vanilla-timing break, ref:
        // continueDestroyBlock accumulates gm+0x24 progress only while its
        // whole gate holds and its hardest gate is byte[lp+0xB22]==1, the
        // "attack held on block" flag the games input pipeline derives from the REAL mouse button every tick
        float rate = 0.f;
        if (SDK::Block* target = src->getBlock(mCurrentBlock)) {
            // Silent mode: the tool only exists in the MobEquipment packet, so
            // compute the rate with the selection briefly pointing at it —
            // otherwise an empty hand mines at fist speed (the "hour per block"
            // bug). Default mode: the local selection already holds the tool.
            if (GetMode() == 1 && GetSilentSlotSwitch() && mToolSlot >= 0) {
                HeldSlotSave hold;
                sehHoldSlot(lp, mToolSlot, &hold);
                rate = GM::getDestroyRate(gm, target);
                sehRestoreSlot(lp, &hold);
            } else {
                rate = GM::getDestroyRate(gm, target);
            }
        }

        // swing every tick like real engine does when breaking blocks.
        // Silent mode: AnimatePacket ONLY (the server sees the arm swing,
        // the local first-person animation is never triggered) unless
        // "Show Swing" is on — then the local animation also plays.
        // Default: the local GameMode::swing() animates the arm and carries the
        // server swing via the game's own outbound path.
        if (GetSwing()) {
            if (GetMode() == 1) {
                SDK::PacketUtils::sendSwing(lp);
                // Show Swing: re-enable the local animation on request. Off
                // (default) means the server still gets its AnimatePacket but
                // the HUD never dips — the visible artifact the Silent mode
                // exists to hide.
                if (GetShowSwing()) GM::swing(lp, GM::SwingSource::Mine);
            } else {
                GM::swing(lp, GM::SwingSource::Mine);
            }
        }

        // Per-tick delta
        float ticksElapsed = mTickDelta / 0.050000001f;
        if (ticksElapsed < 0.f) ticksElapsed = 0.f;
        if (ticksElapsed > 8.f) ticksElapsed = 8.f;

        float mult = GetRateMultiplier();
        if (mult < 0.05f) mult = 0.05f;
        if (mult > 1.0f)  mult = 1.0f;   // never faster than the real client

        if (rate > 0.f) {
            mLastRate = rate * mult;
            mBreakingProgress += ticksElapsed * mLastRate;
            mStallTicks = 0;
        } else {
            // getDestroyRate unavailable or the held tool cannot harvest
            mLastRate = 0.f;
            if (++mStallTicks > 20) { reset(); return; }
        }
        if (mBreakingProgress > 1.0f) mBreakingProgress = 1.0f;

        if (GetRotate() && mShouldRotate) {
            Vec3 eye{};
            if (sehGetPos(lp, &eye)) {
                Vec3 aim((float)mCurrentBlock.x + 0.5f,
                         (float)mCurrentBlock.y + 0.5f,
                         (float)mCurrentBlock.z + 0.5f);
                float dx = aim.x - eye.x, dy = aim.y - eye.y, dz = aim.z - eye.z;
                float horiz = std::sqrt(dx * dx + dz * dz);
                if (horiz > 1e-4f || std::fabs(dy) > 1e-4f) {
                    const float R2D = 57.2957795f;
                    float yaw = std::atan2(dz, dx) * R2D - 90.0f;
                    float pitch = -std::atan2(dy, horiz) * R2D;
                    if (pitch > 89.9f) pitch = 89.9f;
                    if (pitch < -89.9f) pitch = -89.9f;
                    while (yaw > 180.f) yaw -= 360.f;
                    while (yaw < -180.f) yaw += 360.f;
                    SilentRotation::SetContinuous(yaw, pitch);
                        const bool w_ = (GetAsyncKeyState('W') & 0x8000) != 0;
                        const bool a_ = (GetAsyncKeyState('A') & 0x8000) != 0;
                        const bool s_ = (GetAsyncKeyState('S') & 0x8000) != 0;
                        const bool d_ = (GetAsyncKeyState('D') & 0x8000) != 0;
                        SilentRotation::SetMoveInput((w_ ? 1 : 0) - (s_ ? 1 : 0), (d_ ? 1 : 0) - (a_ ? 1 : 0));
                        SilentRotation::PublishMoveFrame(lp->getRot().y);
                }
            }
        }

        if (mBreakingProgress >= kBreakDone) {
            // Publish the finished progress into the engine so destroyBlock's
            // gate (progress >= 0.99998999) passes then dispatch same finish a vanilla break would.
            // Silent: park the selection on the tool for the whole finish — the
            // destroy transaction serializes the CARRIED item, and the server
            // rejects a pearl/empty-hand break (the "visual passes but the block
            // never breaks" bug).
            HeldSlotSave hold;
            if (GetMode() == 1 && GetSilentSlotSwitch() && mToolSlot >= 0)
                sehHoldSlot(lp, mToolSlot, &hold);
            sehWriteBreakProgress(gm, 1.0f);
            GM::destroyBlock(gm, vt, ip, mCurrentFace);
            GM::stopDestroy(gm, vt, ip);
            sehRestoreSlot(lp, &hold);
            omDbg(dbg, "broke (%d,%d,%d) uncover=%d ticks=%.1f",
                  mCurrentBlock.x, mCurrentBlock.y, mCurrentBlock.z,
                  (int)mIsUncovering, 1.0f / (mLastRate > 0.f ? mLastRate : 0.000001f));
            // No slot restore here: the next ore is picked this same tick and
            // keeps the tool; "nothing to mine" below restores once. (The old
            // restore sent MobEquipment(prev) without updating mServerSlot, so
            // the NEXT block was mined with the server holding the old item -
            // the "Silent doesn't break unless the pickaxe is selected" bug.)
            mIsMining = false;
            mIsUncovering = false;
            mBreakingProgress = 0.f;
            mLastRate = 0.f;
            s_isMining = false;
            mShouldRotate = false;
            // fall through to pick next ore this same tick
        } else {
            omDbg(dbg, "mining (%d,%d,%d) prog=%.3f rate=%.4f (~%d ticks left)",
                  mCurrentBlock.x, mCurrentBlock.y, mCurrentBlock.z,
                  mBreakingProgress, mLastRate,
                  mLastRate > 0.f ? (int)((1.0f - mBreakingProgress) / mLastRate + 0.5f) : -1);
            return;
        }
    } else if (mIsMining) {
        // Target gone / invalid
        reset();
    }

    // Always the nearest enabled ore. Exposed first, then (if Path Find) a
    // buried one via the closest uncover block.
    Vec3 eye{};
    if (!sehGetPos(lp, &eye)) return;
    int range = (int)std::ceil(GetRange());
    int bx = (int)std::floor(eye.x);
    int by = (int)std::floor(eye.y);
    int bz = (int)std::floor(eye.z);

    SDK::BlockPos bestExposed{ INT_MAX, INT_MAX, INT_MAX };
    SDK::BlockPos bestBuried{ INT_MAX, INT_MAX, INT_MAX };
    float bestExpD = 1e30f, bestBurD = 1e30f;

    for (int x = bx - range; x <= bx + range; ++x)
    for (int y = by - range; y <= by + range; ++y)
    for (int z = bz - range; z <= bz + range; ++z) {
        SDK::BlockPos p(x, y, z);
        if (!isValidOre(p, true, false)) continue;
        float dx = (float)x + 0.5f - eye.x;
        float dy = (float)y + 0.5f - eye.y;
        float dz = (float)z + 0.5f - eye.z;
        float d = dx * dx + dy * dy + dz * dz;
        if (getExposedFace(p) >= 0) {
            if (d < bestExpD) { bestExpD = d; bestExposed = p; }
        } else if (d < bestBurD) {
            bestBurD = d; bestBuried = p;
        }
    }

    if (bestExposed.x != INT_MAX && sehWithinServerReach(eye, bestExposed)) {
        int face = getExposedFace(bestExposed);
        queueBlock(bestExposed, face);
        mTargettingBlock = bestExposed;
        mIsUncovering = false;
        omDbg(dbg, "queue nearest exposed (%d,%d,%d)", bestExposed.x, bestExposed.y, bestExposed.z);
        return;
    }

    if (GetUncoverMode() == 1 && bestBuried.x != INT_MAX) {
        SDK::BlockPos path;
        if (getBestPathToBlock(bestBuried, &path) && sehWithinServerReach(eye, path)) {
            int face = getExposedFace(path);
            queueBlock(path, face);
            mTargettingBlock = bestBuried;
            mIsUncovering = true;
            omDbg(dbg, "queue uncover (%d,%d,%d)->ore(%d,%d,%d)",
                  path.x, path.y, path.z, bestBuried.x, bestBuried.y, bestBuried.z);
            return;
        }
    }

    // Nothing to mine - restore slot (visible in Default, server-side in Silent)
    restoreSlot(lp);
}

void OreMiner::OnRender() {
    // Centered Mining progress bar
    if (GetMiningBar()) renderMiningBar();

    if (!IsEnabled() || !GetRenderBlock() || !mIsMining) return;
    if (SDK::ScreenView::currentScreenName() != "hud_screen") return;
    if (mCurrentBlock.x == INT_MAX) return;

    float target = mBreakingProgress;
    if (target < 0.f) target = 0.f;
    if (target > 1.f) target = 1.f;
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.f || dt > 0.25f) dt = 1.f / 60.f;
    mRenderProgress += (target - mRenderProgress) * std::min(1.f, dt * 18.f);
    float p = mRenderProgress;

    Vec3 newCenter((float)mCurrentBlock.x + 0.5f,
                   (float)mCurrentBlock.y + 0.5f,
                   (float)mCurrentBlock.z + 0.5f);
    if (!mRenderHavePos || mLastQueuedBlock.x != mCurrentBlock.x ||
        mLastQueuedBlock.y != mCurrentBlock.y || mLastQueuedBlock.z != mCurrentBlock.z) {
        mLastQueuedBlock = mCurrentBlock;
        if (!mRenderHavePos) {
            mRenderCenter = newCenter; // first frame: no glide-in from nowhere
            mRenderLerp = 1.f;
        } else {
            mRenderLerp = 0.f;         // glide from the old drawn position
        }
        mRenderHavePos = true;
    }
    mRenderLerp = std::min(1.f, mRenderLerp + dt * 10.f);
    float k = std::min(1.f, dt * 10.f);
    mRenderCenter.x += (newCenter.x - mRenderCenter.x) * k;
    mRenderCenter.y += (newCenter.y - mRenderCenter.y) * k;
    mRenderCenter.z += (newCenter.z - mRenderCenter.z) * k;

    if (p < 0.01f) return;

    float mvp[16];
    Vec3 origin;
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !acquireCameraMVP(ci, mvp, origin)) return;

    ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    float half = p * 0.5f;
    float cx = mRenderCenter.x;
    float cy = mRenderCenter.y;
    float cz = mRenderCenter.z;
    Vec3 mn(cx - half, cy - half, cz - half);
    Vec3 mx(cx + half, cy + half, cz + half);
    ImVec2 pts[8]; bool ok[8];
    if (SDK::BoxOverlay::projectBoxCorners(mn, mx, mvp, origin, screen, pts, ok) < 2) return;

    uint64_t nowMs = GetTickCount64();
    ImColor theme = ColorUtils::getThemedColor(0.0f, nowMs);
    ImColor col = mIsUncovering ? ImColor(255, 220, 40, 200) : theme;
    ImColor fill(col.Value.x, col.Value.y, col.Value.z, 0.35f);

    // Outline style (the ESP "Outline" look): the projected cube's FACE quads
    // carry the translucent fill (the proven old path — the hull fill read as
    // empty at shallow angles) and ONE closed hull contour draws over them —
    // a black-backed themed line instead of the full 12-edge cage. The shape
    // collapses smoothly with the progress shrink, so the outline reads as a
    // tight frame that closes in on the block.
    for (int fi = 0; fi < 6; ++fi) {
        const int* f = SDK::BoxOverlay::kCubeFaces[fi];
        if (!(ok[f[0]] && ok[f[1]] && ok[f[2]] && ok[f[3]])) continue;
        ImVec2 q[4] = { pts[f[0]], pts[f[1]], pts[f[2]], pts[f[3]] };
        dl->AddConvexPolyFilled(q, 4, fill);
    }
    {
        ImVec2 valids[8]; int nv = 0;
        for (int i = 0; i < 8; ++i) if (ok[i]) valids[nv++] = pts[i];
        ImVec2 hull[18];
        const int hullN = SDK::BoxOverlay::convexHull2D(valids, nv, hull);
        if (hullN >= 3) {
            dl->AddPolyline(hull, hullN, IM_COL32(0, 0, 0, 217), ImDrawFlags_Closed, 3.5f);
            dl->AddPolyline(hull, hullN, col, ImDrawFlags_Closed, 2.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Centered Mining progress bar (modern plate).
//
// The ore's texture used to sit in its OWN slot OUTSIDE the plate (a leftover
// from the game-render icon pass, which no longer draws anything) and the
// footprint was "plate + icon slot". It is now drawn INSIDE the plate, left of
// the "Mining" label, with SDK::drawBlockTexture3D on the ImGui draw list so it
// lands ON TOP of the blur/plate — and the footprint (mWidth / mHeight) is the
// plate itself, which is exactly what the HUD editor drags.
//
// Plate and motion follow the counter language (CounterStyle.h / Scaffold):
// backdrop blur, dim, drop shadow, rounding and a thin white hairline; crisp
// text (no glyph shadows); a rounded track with a themed gradient fill; and a
// scale-up appearance (0.85 -> 1 with an ease-out-back overshoot) that leaves
// by easing down to 0.90 while the alpha follows to zero.
// ---------------------------------------------------------------------------

// Theme sweep span across the bar. getThemedColor's index is in the same units
// as its animation clock, so one full palette cycle is ~5000 at the default
// Color Speed — ~900 across the bar gives a clearly readable gradient without
// repeating a hue inside a single bar.
static constexpr float kOreBarSweepSpan = 900.0f;

// Ease-out-back (the ArrayList / ClickGUI spring): overshoots ~10% past 1 before
// settling, which is what makes the bar "pop" into place instead of sliding.
static float omEaseOutBack(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float u = t - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

void OreMiner::renderMiningBar() {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.f || dt > 0.25f) dt = 1.f / 60.f;

    // Appearance driver (0 = hidden, 1 = shown). The gate is unchanged
    // (IsEnabled && mining); the exponential approach is what keeps producing
    // frames while the value sits inside (0,1), i.e. it renders the fade-out.
    const bool shown = IsEnabled() && mIsMining;
    mBarAnim += ((shown ? 1.f : 0.f) - mBarAnim) * std::min(1.f, dt * 10.f);
    if (mBarAnim < 0.002f) {
        mBarAnim = 0.f;
        mBarIconWanted = false;
        // Hidden bar: zero footprint so the HUD editor skips it
        mWidth = 0.f;
        mHeight = 0.f;
        return;
    }
    const float a = mBarAnim;

    const float scale = mScale > 0.01f ? mScale : 1.0f;
    const float fontSize = 16.0f * scale;
    const float barW = 180.0f * scale;
    const float barH = 8.0f * scale;
    const float padX = 12.0f * scale, padY = 8.0f * scale;
    const float iconSize = GetBarIcon() ? 28.0f * scale : 0.0f;
    const float iconGap  = GetBarIcon() ? 8.0f * scale : 0.0f;
    // The header row now carries the icon as well, so it is the taller of the
    // text line and the icon (the plate grows to hold the icon instead of the
    // icon hanging outside the panel).
    const float labelH = std::max(fontSize + 2.0f * scale, iconSize);

    // The plate — and with it the track — must fit the header row: icon, label
    // and the WIDEST possible percentage. "100%" is measured rather than the
    // live value so the plate width (and the bar) never jitter as the value
    // changes, and the row can never collide even with a very wide GUI font.
    const std::string label = "Mining";
    const std::string pctMax = "100%";
    const float labelW = ImRenderUtils::getTextWidth(&label, fontSize);
    const float pctMaxW = ImRenderUtils::getTextWidth(&pctMax, fontSize);
    const float rowMinW = iconSize + iconGap + labelW + 12.0f * scale + pctMaxW;
    const float trackW = std::max(barW, rowMinW);

    const float innerW = trackW + padX * 2.0f;
    const float innerH = labelH + barH + 2.0f * scale + padY * 2.0f;

    // Footprint = the plate. The icon slot outside it is gone, so the HUD
    // editor box and the visible panel are the same rectangle.
    mWidth = innerW;
    mHeight = innerH;

    // First-render default: centered, a little below the middle. After that the
    // HUD editor owns mPosX/mPosY.
    if (!mBarPlaced) {
        mPosX = screen.x * 0.5f - innerW * 0.5f;
        mPosY = screen.y * 0.5f + 60.0f * scale;
        mBarPlaced = true;
    }
    const float x0 = mPosX;
    const float y0 = mPosY;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    // ---- appear / disappear: SCALE-UP + fade (same as the Scaffold counter) --
    // Up: springs from 0.85 with an ease-out-back overshoot. Down: eases to 0.90
    // while the fill/shadow alpha follows mBarAnim to zero.
    const float zoom = shown
        ? 0.85f + 0.15f * omEaseOutBack(a)
        : 0.90f + 0.10f * (a * a * (3.0f - 2.0f * a));   // smoothstep down

    // Center-pivot zoom: plate, icon and glyph positions are all mapped through
    // these, so nothing crawls out of the plate while it animates.
    const float pivotX = x0 + innerW * 0.5f;
    const float pivotY = y0 + innerH * 0.5f;
    auto zx = [&](float x) { return pivotX + (x - pivotX) * zoom; };
    auto zy = [&](float y) { return pivotY + (y - pivotY) * zoom; };

    const ImVec4 panel(zx(x0), zy(y0), zx(x0 + innerW), zy(y0 + innerH));
    float rounding = GetBarRounding() * scale * zoom;
    if (rounding < 0.f) rounding = 0.f;
    const uint64_t nowMs = ColorUtils::GetTimeMs();

    // ---- plate: blur, dim, shadow, rounding, hairline -----------------------
    // Blur is at full strength once the plate is meaningfully visible (a > 1/3)
    // and ramped below that, so a full-strength blur patch can never flash while
    // the plate is still ~invisible. D2D quantizes the strength to 0.5 steps and
    // only re-runs its full-screen Gaussian when the value CHANGES, so the ramp
    // costs 2-3 bakes instead of one per frame of the transition.
    const float blurAmt = GetBarBlur() * std::min(1.f, a * 3.f);
    if (blurAmt > 0.01f)
        D2D::addBlurOptimized(dl, blurAmt, std::optional<ImVec4>(panel), rounding);
    ImRenderUtils::fillShadowRectangle(panel, ImColor(0, 0, 0), GetBarShadow() * a, rounding, 14.0f * scale, 0.6f, dl);
    ImRenderUtils::fillRectangle(panel, ImColor(12, 12, 16, 255), GetBarDim() * a, rounding, dl);
    // Thin white hairline — the old inner black rectangle read as a chunky
    // double border and does not exist on any other counter plate.
    dl->AddRect(ImVec2(panel.x, panel.y), ImVec2(panel.z, panel.w),
                ImColor(255, 255, 255, (int)(25 * a)), rounding, 0, 1.0f);

    // ---- header row: [icon] Mining            87% ---------------------------
    float p = mRenderProgress;
    if (p < 0.f) p = 0.f;
    if (p > 1.f) p = 1.f;
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%d%%", (int)(p * 100.f + 0.5f));
    const std::string pctS = pct;

    // Glyphs zoom with the plate so the text keeps its inset from the plate
    // edges at every point of the animation.
    const float fs = fontSize * zoom;
    const float rowCenterY = zy(y0 + padY + labelH * 0.5f);
    const float textY = rowCenterY - fs * 0.5f;

    float x = zx(x0 + padX);
    if (GetBarIcon()) {
        // Inside the plate now, left of the label, painted by the ImGui pass on
        // top of the blur/plate.
        mBarIconSize = iconSize * zoom;
        mBarIconX = x;
        mBarIconY = rowCenterY - mBarIconSize * 0.5f;
        mBarIconWanted = a > 0.25f && mIsMining;
        if (mBarIconWanted) {
            std::string oreName;
            SDK::ClientInstance* ici = SDK::ClientInstance::get();
            SDK::BlockSource* isrc = ici ? ici->getBlockSource() : nullptr;
            if (isrc) {
                if (SDK::Block* b = isrc->getBlock(mCurrentBlock)) {
                    if (SDK::BlockLegacy* leg = b->getLegacy()) {
                        oreName = leg->getName();
                        SDK::GameIcon::noteBlock(oreName, b);
                    }
                }
            }
            SDK::drawBlockTexture3D(dl, mBarIconX, mBarIconY, mBarIconSize, oreName.c_str(), a);
        }
        x += mBarIconSize + iconGap * zoom;
    } else {
        mBarIconWanted = false;
    }

    // "Mining" plain white, the value in the live theme gradient — the same
    // label/value split the Scaffold "Blocks X" counter uses. Crisp glyphs by
    // default; "Text Shadow" / "Gradient Numbers" are the counters' own knobs.
    const bool doShadow = GetTextShadow();
    const bool gradient = GetGradientPct();
    ImRenderUtils::drawText(ImVec2(x, textY), label, ImColor(255, 255, 255, 255), fs, a, doShadow, dl);
    const float pctW = ImRenderUtils::getTextWidth(&pctS, fs);
    float gx = zx(x0 + innerW - padX) - pctW;   // right-aligned on the plate
    for (int i = 0; i < (int)pctS.size(); ++i) {
        const std::string glyph(1, pctS[i]);
        const ImColor col = gradient ? ColorUtils::getThemedColor(60.0f + (float)i * 72.0f, nowMs)
                                     : ImColor(255, 255, 255, 255);
        ImRenderUtils::drawText(ImVec2(gx, textY), glyph, col, fs, a, doShadow, dl);
        gx += ImRenderUtils::getTextWidth(&glyph, fs);
    }

    // ---- progress bar: rounded track + themed sweep fill --------------------
    const float trackH = (barH + 2.0f * scale) * zoom;
    ImVec2 t0(zx(x0 + padX), zy(y0 + padY + labelH + 2.0f * scale));
    ImVec2 t1(zx(x0 + padX + trackW), t0.y + trackH);
    const float trackR = trackH * 0.5f;

    // Frosted groove instead of a black pill: a barely-there white wash with a
    // hairline — the same translucent treatment the plate itself uses, so the
    // track reads as part of the panel instead of a bar dropped on top of it.
    // (The solid black slab was the last cheap "plate" left in the row.)
    dl->AddRectFilled(t0, t1, ImColor(255, 255, 255, (int)(18 * a)), trackR);
    dl->AddRect(t0, t1, ImColor(255, 255, 255, (int)(28 * a)), trackR, 0, 1.0f);

    // Fill: the smoothing (mRenderProgress) is untouched, the paint is new.
    const float trackPx = t1.x - t0.x;
    const float fillW = trackPx * p;
    if (fillW > 1.0f) {
        const ImVec2 f0(t0.x, t0.y);
        const ImVec2 f1(t0.x + fillW, t1.y);
        const float fr = std::min(trackR, fillW * 0.5f);

        // Soft themed halo under the pill, so the fill glows against the groove
        // instead of sitting flat on it. Mid-sweep colour so it stays neutral
        // against whichever end of the palette the segment landed on.
        const ImColor midTheme = ColorUtils::getThemedColor(kOreBarSweepSpan * 0.5f, nowMs);
        dl->AddRectFilled(ImVec2(f0.x - 1.0f, f0.y - 1.0f), ImVec2(f1.x + 1.0f, f1.y + 1.0f),
                          ImGui::GetColorU32(ImVec4(midTheme.Value.x, midTheme.Value.y,
                                                    midTheme.Value.z, 0.18f * a)), fr + 1.5f);

        // Themed sweep along the bar: the pill is emitted in white and its
        // vertices are tinted in place (the ImRenderUtils::fillGradientRounded
        // trick), which keeps the anti-aliased rounded caps intact — clipping a
        // multi-colour quad to a rounded pill would paint square corners back in.
        // The sweep is anchored to the TRACK, so the colour at a given x depends
        // on x alone and the palette stays put while the bar grows.
        const int vStart = dl->VtxBuffer.Size;
        dl->AddRectFilled(f0, f1, IM_COL32_WHITE, fr);
        const int vEnd = dl->VtxBuffer.Size;
        for (int vi = vStart; vi < vEnd; ++vi) {
            ImDrawVert& v = dl->VtxBuffer[vi];
            float u = trackPx > 1.0f ? (v.pos.x - t0.x) / trackPx : 0.0f;
            if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
            const ImColor c = ColorUtils::getThemedColor(u * kOreBarSweepSpan, nowMs);
            // Vertices carry alpha 0 on the anti-aliased fringe, so SCALING that
            // alpha (instead of replacing it) keeps the silhouette smooth.
            const float fringe = (float)((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
            v.col = ImGui::GetColorU32(ImVec4(c.Value.x, c.Value.y, c.Value.z, a * fringe));
        }

        // Hairline gloss along the top of the fill so the pill does not read as
        // flat paint (drawn only on full-width segments, where it cannot clash
        // with the rounded leading cap).
        if (fillW > trackR * 2.0f)
            dl->AddLine(ImVec2(f0.x + trackR, f0.y + 1.0f), ImVec2(f1.x - trackR, f0.y + 1.0f),
                        ImColor(255, 255, 255, (int)(60 * a)), 1.0f);
    }
}

void OreMiner::DrawGameIcon(void* mcuirc, const std::string& screenName) {
    // Icon is drawn by the ImGui pass with the ore's real texture now.
    (void)mcuirc; (void)screenName;
}
