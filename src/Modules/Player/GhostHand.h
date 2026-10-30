#pragma once
#include "../Module.h"

// GhostHand — interact through blocks, done as a real hook: the shared
// ClientInputUpdateSystem::tick hook (the one Sprint installs) runs
// GhostHand::AfterInputTick() right after the original, and there the
// module REWRITES the Level's crosshair HitResult:
//   * the pick landed on a BLOCK (a wall), but the eye-ray touches a PLAYER
//     behind it -> the HitResult becomes an ENTITY hit on that player, so
//     the vanilla left-click attacks them straight through the wall;
//   * the eye-ray passes a CONTAINER (chest / trapped / barrel / shulker)
//     before the picked block -> the HitResult becomes a BLOCK hit on the
//     container's nearest free face, so the vanilla right-click opens it
//     through the wall.
// The vanilla input flow does everything else (swing, cooldowns, the
// transactions) — we only redirect what the pick selected, exactly the
// "patch what CameraClip patches" idea.
class GhostHand : public Module {
public:
    GhostHand() : Module("GhostHand", "Hit players and open containers through blocks", Category::Player, 0) {
        AddSetting(new NumberSetting("Reach", "How far through blocks the ghost ray reaches", 4.5f, 1.0f, 6.0f, 0.1f));
        AddSetting(new BoolSetting("Entities", "Left click hits players through blocks", true));
        AddSetting(new BoolSetting("Chests", "Right click opens containers through blocks", true));
        AddSetting(new BoolSetting("Debug", "Log every step of the ghost pipeline to logs.txt", false));
        canBind = false;
    }
    void OnEnable() override;
    // Called from the shared ClientInput tick hook, right after the original
    // (installed by Sprint; OnEnable makes sure it exists).
    static void AfterInputTick();
};
