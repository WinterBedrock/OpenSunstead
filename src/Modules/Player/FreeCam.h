#pragma once
#include "../Module.h"
#include "../../SDK/Types.h"

namespace SDK { class Actor; }

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

    static bool GhostWanted();
    // The frozen enable-point position (only meaningful when GhostWanted()).
    static bool GhostOrigin(Vec3& out);
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
