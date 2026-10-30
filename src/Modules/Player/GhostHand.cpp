#include "GhostHand.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Client/GameMode.h"
#include "../ModuleManager.h"
#include "../../Core/Logger.h"
#include "../Combat/Sprint.h"
#include "../Misc/AntiBot.h"
#include <vector>
#include <string>
#include <cctype>
#include <cmath>
#include <Windows.h>

// ===========================================================================
// GhostHand (see header). The Level's crosshair HitResult is rewritten inside
// the shared ClientInputUpdateSystem::tick hook, right after the original
// tick rebuilt this frame's input — so the vanilla click handling later this
// frame consumes OUR pick: the entity behind the wall, or the container the
// eye-ray passes through.
// ===========================================================================
namespace GM = SDK::GameMode;

namespace {

// Face probes + hit-point helper (ChestAura's recipe, so the server's own
// HitResult check agrees with the face id we send).
struct FaceProbe { int face; int dx, dy, dz; };
constexpr FaceProbe kFaces[] = {
    { 1,  0, 1,  0 }, // up    (+Y)
    { 2,  0, 0, -1 }, // north (-Z)
    { 3,  0, 0,  1 }, // south (+Z)
    { 4, -1, 0,  0 }, // west  (-X)
    { 5,  1, 0,  0 }, // east  (+X)
    { 0,  0, -1, 0 }, // down  (-Y)
};

// Container classifier (ChestAura's, collapsed to a bool).
bool isContainerName(const std::string& raw, uint16_t legacyId) {
    std::string n = raw;
    if (!n.empty()) {
        std::transform(n.begin(), n.end(), n.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        const size_t colon = n.find(':');
        if (colon != std::string::npos) n = n.substr(colon + 1);
        if (n.find("ender_chest") != std::string::npos)   return true;
        if (n.find("trapped_chest") != std::string::npos) return true;
        if (n.find("shulker_box") != std::string::npos)   return true;
        if (n.find("barrel") != std::string::npos)        return true;
        if (n.find("chest") != std::string::npos)         return true;
        return false;
    }
    switch (legacyId) {
    case 54: case 130: case 146: case 218: case 458: return true;
    default: return false;
    }
}

// Slab-method ray vs AABB. Writes the entry distance to *tOut.
bool rayAabb(const Vec3& o, const Vec3& d, const Vec3& mn, const Vec3& mx, float* tOut) {
    const float O[3] = { o.x, o.y, o.z };
    const float D[3] = { d.x, d.y, d.z };
    const float MN[3] = { mn.x, mn.y, mn.z };
    const float MX[3] = { mx.x, mx.y, mx.z };
    float tmin = 0.0f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(D[i]) < 1e-8f) {
            if (O[i] < MN[i] || O[i] > MX[i]) return false;
            continue;
        }
        float t1 = (MN[i] - O[i]) / D[i];
        float t2 = (MX[i] - O[i]) / D[i];
        if (t1 > t2) { const float tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }
    *tOut = tmin;
    return tmax >= tmin;
}

// Unit look direction from Bedrock (pitch, yaw): the inverse of
// SDK::calcLookAngles — forward = (-sin(yaw)cos(pitch), -sin(pitch),
// cos(yaw)cos(pitch)).
Vec3 lookDirFromRot(const Vec2& rot) {
    const float p = rot.x * 0.01745329252f;
    const float y = rot.y * 0.01745329252f;
    const float cp = std::cos(p);
    return Vec3(-std::sin(y) * cp, -std::sin(p), std::cos(y) * cp);
}

// Eye position + rotation read (object-free so __try stays legal, MSVC C2712).
bool ghReadEyeRot(SDK::Actor* lp, Vec3* eye, Vec2* rot) {
    __try { *eye = lp->getPos(); *rot = lp->getRot(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Blank the pick for this frame (after we acted ourselves) so the vanilla
// click does not ALSO mine / place against the wall in front.
bool ghBlank(SDK::HitResult* hr) {
    __try {
        hr->mType = SDK::HitType::NOTHING;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

void GhostHand::OnEnable() {
    // The shared input hook drives AfterInputTick; make sure it is installed
    // even if Sprint has never been enabled this session.
    Sprint::EnsureInputHook();
    LOG_INFO("GhostHand enabled; input hook = "
             + std::string(Sprint::HookInstalled() ? "installed" : "MISSING"), "GhostHand");
}

void GhostHand::AfterInputTick() {
    // Cheap gate first: this runs every input tick.
    Module* self = ModuleManager::GetInstance().GetModuleByName("GhostHand");
    if (!self || !self->IsEnabled()) return;
    if (!SDK::isGameplayInputActive()) return;
    if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
        if (gui->IsEnabled()) return;

    // Click edges. The vanilla click is processed against the game's OWN
    // pick (recomputed later in the frame, which is why rewriting the pick
    // here did nothing), so GhostHand acts itself through the proven
    // pipelines (Aura's entity attack, ChestAura's block use) and then
    // blanks the pick so the same click does not ALSO mine/place on the wall.
    static bool sLmb = false, sRmb = false;
    const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool lmbClick = lmb && !sLmb;
    const bool rmbClick = rmb && !sRmb;
    sLmb = lmb; sRmb = rmb;
    if (!lmbClick && !rmbClick) return;
    const bool dbg = self->mSettings.size() > 3 &&
        static_cast<BoolSetting*>(self->mSettings[3])->mValue;
    if (dbg) LOG_INFO("click: lmb=" + std::to_string((int)lmbClick) +
                      " rmb=" + std::to_string((int)rmbClick), "GhostHand");

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp || !sdk_safe_ptr(lp, 0x230)) return;
    SDK::Level* level = nullptr;
    {
        SDK::Minecraft* mc = ci->getMinecraft();
        if (!mc || !sdk_safe_ptr(mc, 0xC0)) return;
        level = mc->getLevel();
        if (!level || !sdk_safe_ptr(level, 0x10)) return;
    }

    SDK::HitResult* hr = level->getHitResult();
    if (!hr || !sdk_safe_ptr(hr, 0x60)) return;
    if (dbg) LOG_INFO("crosshair hit, type=" + std::to_string((int)hr->mType), "GhostHand");
    // Vanilla already aims at an entity -> it handles the click itself.
    if (hr->mType == SDK::HitType::ENTITY) { if (dbg) LOG_INFO("vanilla already on entity - skip", "GhostHand"); return; }

    Vec3 eye{};
    Vec2 rot{};
    if (!ghReadEyeRot(lp, &eye, &rot)) return;
    const Vec3 dir = lookDirFromRot(rot);
    const float reach = static_cast<NumberSetting*>(self->mSettings[0])->mValue;
    const bool wantEntities = static_cast<BoolSetting*>(self->mSettings[1])->mValue;
    const bool wantChests   = static_cast<BoolSetting*>(self->mSettings[2])->mValue;

    // ---- LEFT CLICK: hit the player the eye-ray touches behind the wall ----
    if (lmbClick && wantEntities) {
        std::vector<SDK::Actor*> actors;
        if (SDK::sehGetActorList(level, &actors)) {
            AntiBot* antiBot = static_cast<AntiBot*>(
                ModuleManager::GetInstance().GetModuleByName("AntiBot"));
            SDK::Actor* best = nullptr;
            float bestT = reach;
            Vec3 bmn{}, bmx{};
            for (SDK::Actor* a : actors) {
                if (!a || a == lp || !sdk_safe_ptr(a, 0x230)) continue;
                if (!SDK::sehIsPlayer(a)) continue;
                if (antiBot && antiBot->IsBot(a)) continue;
                Vec3 mn, mx;
                if (!SDK::sehGetActorBox(a, &mn, &mx)) continue;
                float t = 0.0f;
                if (!rayAabb(eye, dir, mn, mx, &t)) continue;
                if (t < bestT) { bestT = t; best = a; bmn = mn; bmx = mx; }
            }
            if (dbg) LOG_INFO("ray sweep: " + std::to_string(actors.size()) +
                              " actors, best=" + (best ? "found" : "none"), "GhostHand");
            if (best) {
                void** vt = nullptr;
                void* gm = GM::resolve(lp, &vt);
                if (gm && vt) {
                    // Click point: the closest point of the target's AABB to
                    // the eye — the same reference Aura hands the transaction.
                    const Vec3 click(std::clamp(eye.x, bmn.x, bmx.x),
                                     std::clamp(eye.y, bmn.y, bmx.y),
                                     std::clamp(eye.z, bmn.z, bmx.z));
                    GM::prepEntityHit(level);
                    GM::swing(lp);
                    GM::attack(gm, vt, best, &click);
                    if (dbg) LOG_INFO("GHOST ATTACK issued through the wall", "GhostHand");
                    // Blank the pick so the same click does not also start
                    // mining the wall in front.
                    ghBlank(hr);
                } else if (dbg) {
                    LOG_INFO("GameMode resolve failed", "GhostHand");
                }
                return;
            }
        }
    }

    // ---- RIGHT CLICK: open the first container the eye-ray passes through ----
    if (rmbClick && wantChests) {
        SDK::BlockSource* src = ci->getBlockSource();
        if (!src) {
            if (dbg) LOG_INFO("chest path: blockSource NULL", "GhostHand");
            return;
        }
        if (dbg) LOG_INFO("chest path: marching ray from eye "
            + std::to_string(eye.x) + "," + std::to_string(eye.y) + "," + std::to_string(eye.z)
            + " dir " + std::to_string(dir.x) + "," + std::to_string(dir.y) + "," + std::to_string(dir.z), "GhostHand");
        int lx = INT_MIN, ly = INT_MIN, lz = INT_MIN;
        for (float t = 0.35f; t <= reach; t += 0.12f) {
            const int bx = (int)std::floor(eye.x + dir.x * t);
            const int by = (int)std::floor(eye.y + dir.y * t);
            const int bz = (int)std::floor(eye.z + dir.z * t);
            if (bx == lx && by == ly && bz == lz) continue;
            lx = bx; ly = by; lz = bz;
            SDK::Block* block = src->getBlock(SDK::BlockPos(bx, by, bz));
            SDK::BlockLegacy* legacy = block ? block->getLegacy() : nullptr;
            if (dbg) {
                const std::string nm = legacy ? legacy->getName() : std::string("<no block>");
                LOG_INFO("march cell " + std::to_string(bx) + "," + std::to_string(by) + ","
                         + std::to_string(bz) + " -> " + nm, "GhostHand");
            }
            if (!legacy) continue;
            if (!isContainerName(legacy->getName(), legacy->getBlockId())) continue;
            if (dbg) LOG_INFO("CONTAINER found at " + std::to_string(bx) + ","
                              + std::to_string(by) + "," + std::to_string(bz), "GhostHand");
            // Vanilla already picked THIS CONTAINER -> its own right-click
            // opens it; a synthetic open on top would be a duplicate. (The
            // old check compared the pick against EVERY marched cell, so it
            // bailed on the WALL the crosshair was aiming at — the march
            // could never walk past the picked block, and the chest only
            // ever opened when the 0.12-step march happened to skip the
            // pick cell entirely: aiming dead at a block corner, or while
            // sneaking, where the lowered camera ray diverges from our
            // getPos-based ray. Hence "opens only at the corner / on
            // shift".)
            if (hr->mType == SDK::HitType::BLOCK &&
                hr->mBlockPos[0] == bx && hr->mBlockPos[1] == by && hr->mBlockPos[2] == bz)
                return;
            // Pick a face that is not buried, so the server's own
            // reach/visibility check on the click point agrees. NOTE:
            // BlockSource::isSolid() returns TRUE for air on this build (the
            // debug logs proved it — a face over a marched-through AIR cell
            // read as buried), so "free face" is tested directly on the
            // neighbour block: free = no block / air legacy.
            auto neighbourFree = [&](int dx, int dy, int dz) {
                SDK::Block* nb = src->getBlock(SDK::BlockPos(bx + dx, by + dy, bz + dz));
                SDK::BlockLegacy* lg = nb ? nb->getLegacy() : nullptr;
                return !lg || lg->isAir();
            };
            for (const FaceProbe& probe : kFaces) {
                if (!neighbourFree(probe.dx, probe.dy, probe.dz)) {
                    if (dbg) LOG_INFO("face " + std::to_string(probe.face) + " buried, next", "GhostHand");
                    continue;
                }
                if (dbg) LOG_INFO("free face " + std::to_string(probe.face) + " — opening", "GhostHand");
                const Vec3 hit(
                    (float)bx + 0.5f + probe.dx * 0.5f,
                    (float)by + 0.5f + probe.dy * 0.5f,
                    (float)bz + 0.5f + probe.dz * 0.5f);
                void** vt = nullptr;
                void* gm = GM::resolve(lp, &vt);
                if (gm && vt) {
                    // A held BLOCK turns the interaction into a PLACEMENT
                    // against the container's face — the "opens only on some
                    // edge / weird angles" symptom. Park on an empty hotbar
                    // slot (server-side only) for the open, exactly like
                    // ChestAura's Empty Hand, then restore.
                    int parkedSlot = -1;
                    {
                        SDK::Container* cont = SDK::Inv::container(lp);
                        int emptySlot = -1;
                        if (cont) {
                            for (int s = 0; s < 9; ++s) {
                                SDK::ItemStack* st = cont->getItem(s);
                                if (!st || !st->isValidStack()) { emptySlot = s; break; }
                            }
                        }
                        const int cur = SDK::Inv::selectedSlot(lp);
                        if (emptySlot >= 0 && cur >= 0 && cur <= 8 && emptySlot != cur) {
                            if (SDK::PacketUtils::sendHotbarSlot(lp, emptySlot)) {
                                parkedSlot = cur;
                                if (dbg) LOG_INFO("parked hotbar slot " + std::to_string(cur)
                                                  + " -> " + std::to_string(emptySlot), "GhostHand");
                            }
                        }
                    }
                    GM::prepBlockHit(level, SDK::IVec3(bx, by, bz),
                                     probe.face, hit.x, hit.y, hit.z);
                    const bool opened = GM::buildBlock(gm, vt, SDK::IVec3(bx, by, bz),
                                                       (unsigned char)probe.face);
                    if (dbg) LOG_INFO(std::string("buildBlock ") +
                                      (opened ? "OK — open issued" : "FAILED"), "GhostHand");
                    if (opened) {
                        GM::swing(lp, GM::SwingSource::Interact);
                        // Blank the pick so the same click does not also
                        // place a block against the wall in front.
                        ghBlank(hr);
                    }
                    if (parkedSlot >= 0)
                        SDK::PacketUtils::sendHotbarSlot(lp, parkedSlot);
                } else if (dbg) {
                    LOG_INFO("GameMode resolve failed", "GhostHand");
                }
                return;
            }
            if (dbg) LOG_INFO("container fully enclosed — nothing to aim at", "GhostHand");
            return;   // fully enclosed container — nothing to aim at
        }
    }
}
