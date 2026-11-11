#include "GhostHand.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Client/GameMode.h"
#include "../ModuleManager.h"
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
}

void GhostHand::AfterInputTick() {
    // Cheap gate first: this runs every input tick.
    Module* self = ModuleManager::GetInstance().GetModuleByName("GhostHand");
    if (!self || !self->IsEnabled()) return;
    if (!SDK::isGameplayInputActive()) return;
    if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
        if (gui->IsEnabled()) return;

    static bool sLmb = false, sRmb = false;
    const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool lmbClick = lmb && !sLmb;
    const bool rmbClick = rmb && !sRmb;
    sLmb = lmb; sRmb = rmb;
    if (!lmbClick && !rmbClick) return;

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
    // Vanilla already aims at an entity -> it handles the click itself.
    if (hr->mType == SDK::HitType::ENTITY) return;

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
                    // Blank the pick so the same click does not also start
                    // mining the wall in front.
                    ghBlank(hr);
                }
                return;
            }
        }
    }

    // ---- RIGHT CLICK: open the first container the eye-ray passes through ----
    if (rmbClick && wantChests) {
        SDK::BlockSource* src = ci->getBlockSource();
        if (!src) return;
        int lx = INT_MIN, ly = INT_MIN, lz = INT_MIN;
        for (float t = 0.35f; t <= reach; t += 0.12f) {
            const int bx = (int)std::floor(eye.x + dir.x * t);
            const int by = (int)std::floor(eye.y + dir.y * t);
            const int bz = (int)std::floor(eye.z + dir.z * t);
            if (bx == lx && by == ly && bz == lz) continue;
            lx = bx; ly = by; lz = bz;
            SDK::Block* block = src->getBlock(SDK::BlockPos(bx, by, bz));
            SDK::BlockLegacy* legacy = block ? block->getLegacy() : nullptr;
            if (!legacy) continue;
            if (!isContainerName(legacy->getName(), legacy->getBlockId())) continue;
            if (hr->mType == SDK::HitType::BLOCK &&
                hr->mBlockPos[0] == bx && hr->mBlockPos[1] == by && hr->mBlockPos[2] == bz)
                return;
            auto neighbourFree = [&](int dx, int dy, int dz) {
                SDK::Block* nb = src->getBlock(SDK::BlockPos(bx + dx, by + dy, bz + dz));
                SDK::BlockLegacy* lg = nb ? nb->getLegacy() : nullptr;
                return !lg || lg->isAir();
            };
            for (const FaceProbe& probe : kFaces) {
                if (!neighbourFree(probe.dx, probe.dy, probe.dz)) continue;
                const Vec3 hit(
                    (float)bx + 0.5f + probe.dx * 0.5f,
                    (float)by + 0.5f + probe.dy * 0.5f,
                    (float)bz + 0.5f + probe.dz * 0.5f);
                void** vt = nullptr;
                void* gm = GM::resolve(lp, &vt);
                if (gm && vt) {
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
                            if (SDK::PacketUtils::sendHotbarSlot(lp, emptySlot))
                                parkedSlot = cur;
                        }
                    }
                    GM::prepBlockHit(level, SDK::IVec3(bx, by, bz),
                                     probe.face, hit.x, hit.y, hit.z);
                    const bool opened = GM::buildBlock(gm, vt, SDK::IVec3(bx, by, bz),
                                                       (unsigned char)probe.face);
                    if (opened) {
                        GM::swing(lp, GM::SwingSource::Interact);
                        // Blank the pick so the same click does not also
                        // place a block against the wall in front.
                        ghBlank(hr);
                    }
                    if (parkedSlot >= 0)
                        SDK::PacketUtils::sendHotbarSlot(lp, parkedSlot);
                }
                return;
            }
            return;   // fully enclosed container — nothing to aim at
        }
    }
}
