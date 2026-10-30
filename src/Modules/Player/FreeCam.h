#pragma once
#include "../Module.h"
#include "../../SDK/Types.h"

namespace SDK { class Actor; }

// FreeCam — the reference client/classic ghost flight.
//
// The BODY flies (velocity writes from the game thread) while the server is
// never told about any of it: outgoing PlayerAuthInput AND MovePlayer packets
// are cancelled entirely, so the server's copy of the player just stands at the
// enable point with zero input (this is exactly how the reference client/the reference client implement it
// — spoofing positions INTO the PAI payload instead made the client-side
// movement predictor rewind the flying body every ack, which was the chunk
// flicker / "everything teleports" mess).
//
// Noclip is done through the engine's own HasCollision/HasGravity actor flags
// (restored on disable) rather than shrinking the AABB — a shrunken collision
// box wedged into the floor made the physics depenetration shove the body
// upward and the client's chunk streaming churn.
//
// GHOST BODY ("Show Model"): the server copy of you never moved, so instead of
// only drawing a wireframe box at the enable point, the module can leave your
// actual player MODEL standing there while the camera flies. The engine never
// dispatches the local player's model in first person (it is not collected
// into the frame), so the render path is forced directly:
//
//   Chams' ActorRenderDispatcher::render detour (the single MinHook slot on
//   that target) sees every entity dispatch of the frame. On the FIRST
//   non-local-player dispatch it calls the vanilla dispatcher once more for
//   the LOCAL PLAYER with `pos` bound to FreeCam's frozen enable-point
//   position — the dispatcher copies that argument straight into the render
//   entry (verified in IDA: *a5/*a5+8 are written into the entry's transform),
//   so the model is drawn standing exactly where FreeCam was enabled, with
//   normal depth against the world. Interleaved with the real entity draws,
//   it is submitted in the same pass and depth-tests like any other entity.
//
//   In third person the local player IS dispatched normally; that call is
//   instead re-issued with the frozen position (GhostPosFor), so the model
//   still stands at the enable point instead of at the flying body.
//
// One injection per world-render frame is guaranteed by
// GameHooks::g_freeCamGhostDrawn. Everything is inert when the module is off,
// and the wireframe box remains the fallback visual.
//
// On disable the snapshot is written back, so the body lands exactly where the
// server always believed it stood (skipped after a server correction).
class FreeCam : public Module {
public:
    FreeCam();

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;     // 20 Hz — velocity writes (Jetpack scheme)
    void OnUpdate() override;   // legacy frame hook, unused
    void OnRender() override;
    void OnPacketSend(class SDK::Packet* packet, int packetId, bool& cancel) override;
    void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) override;

    // ---- Ghost body API (called from the render hooks, see file header) -----
    // True while FreeCam wants the ghost body drawn this frame: the module is
    // flying, the "Show Model" setting is on, and no server correction storm
    // invalidated the frozen snapshot. Actor-independent on purpose — the
    // first-person injection path needs the answer before it knows which
    // actor is being dispatched.
    static bool GhostWanted();
    // The frozen enable-point position (only meaningful when GhostWanted()).
    static bool GhostOrigin(Vec3& out);
    // Third-person path: when `actor` is the local player while FreeCam is
    // flying, fills `out` with the frozen enable-point position and returns
    // true — the dispatcher detour re-issues that call with `pos` bound here.
    static bool GhostPosFor(SDK::Actor* actor, Vec3& out);

    // ---- Diagnostics (injection heartbeat in Chams) --------------------------
    bool IsActiveForDiag() const { return mActive; }
    bool IsLagbackedForDiag() const { return mLagbacked; }
    bool GetShowModelForDiag() const {
        return mSettings.size() > 5 && static_cast<BoolSetting*>(mSettings[5])->mValue;
    }

private:
    float GetSpeed() const;
    bool  GetThroughWalls() const;
    bool  GetShowBox() const;
    bool  GetDisableOnLagback() const;
    bool  GetShowPos() const;
    bool  GetShowModel() const;

    bool  mActive = false;
    bool  mLagbacked = false;
    bool  mImmobileApplied = false;
    bool  mThroughWallsApplied = false;

    // Correction-storm detector (see OnPacketReceive): corrections inside one
    // 3 s window. A handful is normal server noise; a flood means the server
    // is actively fighting the frozen position.
    unsigned long long mStormStartMs = 0;
    int                mStormCount = 0;

    // Server-side (frozen) state captured at enable time and restored on
    // disable unless the server corrected us.
    Vec3  mFrozenPos{};
    Vec3  mFrozenPosOld{};
    Vec3  mFrozenVel{};
    Vec3  mFrozenAabbMin{};
    Vec3  mFrozenAabbMax{};
    float mFrozenWidth = 0.6f;
    float mFrozenHeight = 1.8f;
};
