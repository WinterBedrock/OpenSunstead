#include "FreeCam.h"
#include "../Setting.h"
#include "../ModuleManager.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Network/Packet.h"
#include "../../Memory/OffsetManager.h"
#include "../../Render/FrameView.h"
#include "../../Render/BoxRender.h"
#include "../../Core/Logger.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <Windows.h>

namespace {

// SEH-guarded snapshot / restore of everything FreeCam disturbs. POD-only
// bodies (MSVC forbids __try in a frame that needs C++ unwinding).
bool sehReadState(SDK::Actor* lp,
                  Vec3& pos, Vec3& posOld, Vec3& vel,
                  Vec3& aabbMin, Vec3& aabbMax,
                  float& w, float& h) {
    if (!lp) return false;
    __try {
        auto* sv = lp->getStateVector();
        auto* ab = lp->getAabbs();
        if (!sv || !ab) return false;
        pos = sv->pos;
        posOld = sv->posOld;
        vel = sv->velocity;
        aabbMin = ab->mMin;
        aabbMax = ab->mMax;
        w = ab->width;
        h = ab->height;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehRestoreState(SDK::Actor* lp,
                     const Vec3& pos, const Vec3& posOld, const Vec3& vel,
                     const Vec3& aabbMin, const Vec3& aabbMax,
                     float w, float h) {
    if (!lp) return false;
    __try {
        auto* sv = lp->getStateVector();
        auto* ab = lp->getAabbs();
        if (!sv || !ab) return false;
        sv->pos = pos;
        sv->posOld = posOld;
        sv->velocity = vel;
        ab->mMin = aabbMin;
        ab->mMax = aabbMax;
        ab->width = w;
        ab->height = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// NO INPUT FREEZE. An earlier revision froze the local input pipeline with the
// engine's Immobile flag — but that flag's follow-up system ZEROES the state
// vector's velocity every tick (it is the "frozen player" state servers
// apply), which deleted the flight velocity we had just written: the player
// could not move at all. The held-WASD double-drive concern is moot anyway:
// outgoing movement packets are cancelled below, so the vanilla walk only
// moves the local body in the same direction the flight keys already push it.

// POD-only helpers so the Module methods themselves never mix __try with

bool sehZeroVelocity(SDK::Actor* lp) {
    if (!lp) return false;
    __try {
        auto* sv = lp->getStateVector();
        if (!sv) return false;
        sv->velocity.x = 0.f;
        sv->velocity.y = 0.f;
        sv->velocity.z = 0.f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehSetVelocity(SDK::Actor* lp, float vx, float vy, float vz) {
    if (!lp) return false;
    __try {
        auto* sv = lp->getStateVector();
        if (!sv) return false;
        sv->velocity.x = vx;
        sv->velocity.y = vy;
        sv->velocity.z = vz;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehGetRuntimeId(SDK::Actor* lp, uint64_t& out) {
    if (!lp) return false;
    __try {
        out = lp->getRuntimeID();
        return out != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehReadInt64(const void* base, uintptr_t offset, int64_t& out) {
    if (!base) return false;
    __try {
        out = *reinterpret_cast<int64_t*>(reinterpret_cast<uintptr_t>(base) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Through Walls: collapse the flying body's AABB to a POINT (height -> 0,
// width -> 0.001) — a zero-height box has no vertical extent for the
// horizontal-collision pass to resolve against (the exact mechanism NoClip
// uses, which is proven to work on this build; the old width-only shrink
// still collided). Client game type additionally set to Spectator where the
// engine consults it (also disables gravity, which fits flight). Both
// restored on disable.
//
// NOTE on the `on` parameter: on=true APPLIES (save + collapse + Spectator),
// on=false RESTORES. The call sites used to be inverted - OnTick restored
// every tick (so flying through blocks never actually worked) and OnDisable
// APPLIED (leaving the player stuck in Spectator mode after turning FreeCam
// off). Fixed.
int   sehTW_savedType = -1;
float sehTW_savedW = 0.f;
float sehTW_savedH = 0.f;
bool sehThroughWalls(SDK::Actor* lp, bool on) {
    if (!lp) return false;
    if (on) {
        const int cur = lp->getGameType();
        if (cur >= 0 && cur != 6) sehTW_savedType = cur;
        lp->setGameType(6 /*GameType::Spectator*/);
        __try {
            auto* ab = lp->getAabbs();
            if (ab && sdk_safe_ptr(ab, sizeof(::AABBShapeComponent))) {
                if (ab->width > 0.05f && ab->width < 5.0f) {
                    sehTW_savedW = ab->width;
                    sehTW_savedH = ab->height;
                }
                const float cx = (ab->mMin.x + ab->mMax.x) * 0.5f;
                const float cz = (ab->mMin.z + ab->mMax.z) * 0.5f;
                const float hw = 0.0005f;
                ab->mMin.x = cx - hw; ab->mMin.z = cz - hw;
                ab->mMax.x = cx + hw; ab->mMax.z = cz + hw;
                ab->mMax.y = ab->mMin.y;          // zero height: NoClip's proven trick
                ab->width = 0.001f;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return true;
    }
    // off: restore both (position-relative rebuild - the body has flown since)
    __try {
        auto* ab = lp->getAabbs();
        if (ab && sehTW_savedW > 0.05f) {
            const float cx = (ab->mMin.x + ab->mMax.x) * 0.5f;
            const float cz = (ab->mMin.z + ab->mMax.z) * 0.5f;
            const float feetY = ab->mMin.y;
            const float hw = sehTW_savedW * 0.5f;
            ab->mMin.x = cx - hw; ab->mMin.z = cz - hw;
            ab->mMax.x = cx + hw; ab->mMax.z = cz + hw;
            ab->mMax.y = feetY + sehTW_savedH;
            ab->width = sehTW_savedW; ab->height = sehTW_savedH;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (sehTW_savedType >= 0) lp->setGameType(sehTW_savedType);
    sehTW_savedType = -1; sehTW_savedW = 0.f; sehTW_savedH = 0.f;
    return true;
}

// Is the MINECRAFT window the foreground window? GetAsyncKeyState is
// process-global: without this gate the fly keys answered while the game was
// in the background. The client also runs a GUI thread while a screen is
// open — the hud_screen check in OnTick covers that half.
bool sdkIsGameFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    // Accept any foreground window owned by this process.
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return fgPid == GetCurrentProcessId();
}

} // namespace

void FreeCam::OnTick() {
    if (!IsEnabled() || !mActive || mLagbacked) return;

    // INPUT GATES: only drive the camera when the game window is focused AND
    // the HUD is the active screen. Without these the WASD keys flew the
    // camera while typing in chat / any GUI, and even while Minecraft ran in
    // the background (GetAsyncKeyState is process-global). With them, keys
    // reach FreeCam exactly when vanilla movement would receive them.
    if (!sdkIsGameFocused()) return;
    if (!SDK::isGameplayInputActive()) return;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;

    const bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
    const bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
    const bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
    const bool space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    // Horizontal direction relative to facing — the same key-offset table Speed
    // and Scaffold use, so all three agree on the yaw convention.
    float vx = 0.f, vz = 0.f;
    float off = 0.f;
    bool hasHoriz = true;
    if      (w && a) off = -45.f;
    else if (w && d) off =  45.f;
    else if (s && a) off = -135.f;
    else if (s && d) off =  135.f;
    else if (w)      off =   0.f;
    else if (a)      off = -90.f;
    else if (s)      off = -180.f;
    else if (d)      off =  90.f;
    else             hasHoriz = false;

    // Diagonals normalize to the same speed as cardinals.
    const float speed = GetSpeed();
    if (hasHoriz) {
        const float yaw = lp->getRot().y;
        const float ang = (yaw + off + 90.f) * SDKMath::D2R;
        vx = std::cos(ang) * speed;
        vz = std::sin(ang) * speed;
    }

    const float vy = space ? speed : (shift ? -speed : 0.f);

    // Through Walls (default on): collapse the AABB to a point and set the
    // client game type to Spectator. Re-applied every tick — the game rewrites
    // its synched data. Restored on disable.
    if (GetThroughWalls()) {
        sehThroughWalls(lp, true);
        mThroughWallsApplied = true;
        // Spectator has no gravity, so a zero vertical velocity is a stable
        // hover. (The old +0.08 "anti-gravity" feed was written for a
        // gravity-active body; with Spectator it is a pure upward drift - the
        // "glide pulls me into the air" effect.)
        const float hoverVy = (space || shift) ? vy : 0.f;
        sehSetVelocity(lp, vx, hoverVy, vz);
        return;
    }
    mThroughWallsApplied = false;

    sehSetVelocity(lp, vx, vy, vz);
}

FreeCam::FreeCam()
    : Module("FreeCam", "Ghost flight; the server sees you stand still", Category::Player, 0) {
    AddSetting(new NumberSetting("Speed", "Fly speed in blocks per tick (1.0 = 20 blocks/s)",
                                 1.0f, 0.1f, 5.0f, 0.1f));
    AddSetting(new BoolSetting("Through Walls", "Fly through blocks while in FreeCam", true));
    AddSetting(new BoolSetting("Show Box", "Draw frozen server position", true));
    AddSetting(new BoolSetting("Disable On Lagback", "Turn off if the server teleports you", true));
    AddSetting(new BoolSetting("Show Position", "Draw the live freecam position as text in the corner", false));
    // Ghost body: your own player MODEL left standing at the enable point,
    // instead of / in addition to the wireframe box. Appended after the four
    // original settings so every index-based getter above keeps its slot.
    AddSetting(new BoolSetting("Show Model", "Leave your player model standing at the enable point", true));
    canBind = true;
}

float FreeCam::GetSpeed() const {
    return static_cast<const NumberSetting*>(mSettings[0])->mValue;
}

bool FreeCam::GetThroughWalls() const {
    return static_cast<const BoolSetting*>(mSettings[1])->mValue;
}

bool FreeCam::GetShowBox() const {
    return static_cast<const BoolSetting*>(mSettings[2])->mValue;
}

bool FreeCam::GetDisableOnLagback() const {
    return static_cast<const BoolSetting*>(mSettings[3])->mValue;
}

bool FreeCam::GetShowPos() const {
    return mSettings.size() > 4 && static_cast<BoolSetting*>(mSettings[4])->mValue;
}

bool FreeCam::GetShowModel() const {
    return mSettings.size() > 5 && static_cast<BoolSetting*>(mSettings[5])->mValue;
}

bool FreeCam::GhostWanted() {
    FreeCam* self = static_cast<FreeCam*>(ModuleManager::GetInstance().GetModuleByName("FreeCam"));
    return self && self->IsEnabled() && self->mActive && !self->mLagbacked && self->GetShowModel();
}

bool FreeCam::GhostOrigin(Vec3& out) {
    if (!GhostWanted()) return false;
    FreeCam* self = static_cast<FreeCam*>(ModuleManager::GetInstance().GetModuleByName("FreeCam"));
    if (!self) return false;
    // The frozen snapshot is written once in OnEnable and only read here; a
    // torn read would at worst misplace the model for a single frame.
    out = self->mFrozenPos;
    return true;
}

bool FreeCam::GhostPosFor(SDK::Actor* actor, Vec3& out) {
    if (!actor || !GhostWanted()) return false;
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return false;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp || lp != actor) return false;
    FreeCam* self = static_cast<FreeCam*>(ModuleManager::GetInstance().GetModuleByName("FreeCam"));
    if (!self) return false;
    out = self->mFrozenPos;
    return true;
}

void FreeCam::OnEnable() {
    mLagbacked = false;
    mActive = false;
    mThroughWallsApplied = false;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;

    if (!sehReadState(lp, mFrozenPos, mFrozenPosOld, mFrozenVel,
                      mFrozenAabbMin, mFrozenAabbMax,
                      mFrozenWidth, mFrozenHeight)) {
        LOG_WARN("FreeCam: failed to snapshot player state", "FreeCam");
        return;
    }
    mActive = true;
    // No input freeze: the Immobile flag's follow-up system zeroes the state
    // vector's velocity every tick, which deleted the flight velocity and
    // left the player unable to move. The velocity write in OnTick IS the
    // only movement driver now; held WASD cannot double-drive anything that
    // matters because outgoing movement packets are cancelled below.
    mImmobileApplied = false;
}

void FreeCam::OnDisable() {
    if (!mActive) return;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (ci) {
        SDK::Actor* lp = ci->getLocalPlayer();
        // No Immobile flag was applied, so nothing to restore there.
        mImmobileApplied = false;
        if (lp && mThroughWallsApplied) {
            // Restore the engine movement flags before anything else.
            sehThroughWalls(lp, false);
            mThroughWallsApplied = false;
        }
        // Kill any flight velocity so the body does not keep drifting after
        // the restore.
        if (lp) sehZeroVelocity(lp);
        // Skip the restore after a server correction storm: the server has
        // authoritatively moved us, so writing the pre-FreeCam position back
        // would fight it. The full-size AABB box is still restored in every
        // case so nothing shrunken can outlive the module.
        if (lp && !mLagbacked) {
            sehRestoreState(lp, mFrozenPos, mFrozenPosOld, mFrozenVel,
                            mFrozenAabbMin, mFrozenAabbMax,
                            mFrozenWidth, mFrozenHeight);
        }
    }
    mActive = false;
    mLagbacked = false;
}

// Frame-time scaled movement was removed together with the teleport scheme;
// OnTick (velocity-driven) replaces it entirely.
void FreeCam::OnUpdate() {}

// Outgoing movement packets are dropped: the server's copy of the player just
// stands at the enable point with zero input (same scheme as the reference
// clients). Incoming corrections are dropped in OnPacketReceive — this hook
// only guarantees the server never learns the body moved.
void FreeCam::OnPacketSend(SDK::Packet* packet, int packetId, bool& cancel) {
    if (!IsEnabled() || !mActive || mLagbacked || !packet) return;

    if (packetId == static_cast<int>(SDK::PacketID::PlayerAuthInput) ||
        packetId == static_cast<int>(SDK::PacketID::MovePlayer)) {
        cancel = true;
    }
}

// Incoming movement packets are dropped, not spoofed (reverse-engineered on
// 1.26.42.01): the client applies a server correction by REWINDING the local
// body — MovePlayer is the legacy teleport and CorrectPlayerMovePrediction is
// the server-auth "your predicted position is wrong, go here" — so leaving
// either uncancelled yanked the flying body back to the enable point over and
// over ("it teleports backwards"). Cancelling both keeps the body in free
// flight; the server-side copy just stays at the frozen position, which the
// cancelled outgoing packets above already guarantee.
//
// A single dropped correction is NOISE (servers send them on idle resyncs once
// the outgoing packets stop) — it must NOT toggle the module: that turned the
// dropped rewind into a disable + restore, i.e. the same teleport through the
// back door. Only a genuine STORM of corrections (server hard-fighting us)
// trips the user's "Disable On Lagback" policy.
void FreeCam::OnPacketReceive(SDK::Packet* packet, int packetId, bool& cancel) {
    if (!IsEnabled() || !mActive || mLagbacked || !packet) return;

    const bool isMovePlayer = packetId == static_cast<int>(SDK::PacketID::MovePlayer);
    const bool isCorrection =
        packetId == static_cast<int>(SDK::PacketID::CorrectPlayerMovePrediction);
    if (!isMovePlayer && !isCorrection) return;

    if (isMovePlayer) {
        // MovePlayerPacket::mPlayerID @ +0x30 — only OUR teleport counts, every
        // other player's movement comes through the same packet id.
        if (!sdk_safe_ptr(packet, Layout::MovePlayer::PlayerID + sizeof(int64_t))) return;

        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        if (!ci) return;
        SDK::Actor* lp = ci->getLocalPlayer();
        if (!lp) return;

        uint64_t myId = 0;
        if (!sehGetRuntimeId(lp, myId)) return;

        int64_t pktId = 0;
        if (!sehReadInt64(packet, Layout::MovePlayer::PlayerID, pktId)) return;
        if (static_cast<uint64_t>(pktId) != myId) return;
    }

    // Drop the packet BEFORE the game's handler rewinds the body.
    cancel = true;

    // Storm detector: > 8 corrections inside 3 s means the server is
    // aggressively fighting the frozen position (respawn / anticheat kick
    // sequence) — honour the lagback policy in that case only.
    const unsigned long long now = GetTickCount64();
    if (now - mStormStartMs > 3000ull) {
        mStormStartMs = now;
        mStormCount = 0;
    }
    if (++mStormCount <= 8) return;

    mLagbacked = true;
    if (GetDisableOnLagback()) {
        LOG_INFO("FreeCam: server correction storm, disabling", "FreeCam");
        SetEnabled(false);
    }
}

void FreeCam::OnRender() {
    if (!IsEnabled() || !mActive) return;
    if (!SDK::isHudScreen()) return;

    // Optional live-position readout: the flying body's current coordinates as
    // text in the bottom-left corner (themed colour, one decimal).
    if (GetShowPos()) {
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
        Vec3 p{};
        if (lp) {
            __try { p = lp->getPos(); } __except (EXCEPTION_EXECUTE_HANDLER) { p = Vec3{}; }
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "XYZ %.1f / %.1f / %.1f", p.x, p.y, p.z);
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        const float fs = 16.0f;
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const ImVec2 pos(10.0f, screen.y - ts.y - 12.0f);
        dl->AddRectFilled(ImVec2(pos.x - 6.0f, pos.y - 4.0f),
                          ImVec2(pos.x + ts.x + 6.0f, pos.y + ts.y + 4.0f),
                          IM_COL32(0, 0, 0, 110), 5.0f);
        dl->AddText(pos, IM_COL32(255, 255, 255, 220), buf);
        (void)fs;
    }

    if (!GetShowBox()) return;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, Layout::Probe::ClientInstance)) return;

    float mvp[16];
    Vec3 origin{};
    if (!acquireCameraMVP(ci, mvp, origin)) return;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 screen(io.DisplaySize.x, io.DisplaySize.y);
    if (screen.x < 1.f || screen.y < 1.f) return;

    // Expand the frozen AABB slightly for visibility (like the reference client).
    Vec3 lo = mFrozenAabbMin;
    Vec3 hi = mFrozenAabbMax;
    lo.x -= 0.05f; lo.y -= 0.05f; lo.z -= 0.05f;
    hi.x += 0.05f; hi.y += 0.05f; hi.z += 0.05f;

    SDK::drawBoxWireframe(ImGui::GetBackgroundDrawList(), lo, hi, mvp, origin, screen,
                          IM_COL32(255, 165, 0, 180), 1.5f, IM_COL32(255, 165, 0, 40));
}
