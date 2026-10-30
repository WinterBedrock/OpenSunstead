#pragma once
#include "../Module.h"

namespace SDK { class Packet; }

// AntiEffect — cancels selected (negative) effects FOR THE CLIENT.
//
// Two independent mechanisms:
//   Purge (per-effect toggles) — every game tick the local player's
//     MobEffectsComponent (entt vector of MobEffectInstance, stride
//     Layout::EffectInstance_size, id @ +0) is walked and every BLOCKED
//     instance is neutralized in place: duration = 1 tick, noCounter = 0,
//     amplifier = 0. The game's own effect ticker expires the instance the
//     next tick and runs its NATIVE cleanup (attribute recalc, HUD, particles)
//     — no component surgery, no dangling entries, survives re-application
//     because the purge runs every tick.
//   Block All (packet) — incoming MobEffectPackets (wire id 28 on 1.26.42.01,
//     re-verified in IDA: createPacket case 28 -> vtable getId 0x1C, name
//     "MobEffectPacket"; the old 29 is UpdateAttributesPacket) targeting the
//     local player are dropped before the game ever sees them, so the effect
//     is never applied at all (no one-tick flash the Purge leaves).
//
// NOTE on movement-affecting effects (levitation, slowness): on strict
// server-authoritative movement servers the server keeps simulating the effect
// for us — no client-side cancel can change that; expect corrections there.
// Visual-only effects (darkness, nausea, blindness) cancel perfectly.
//
// MobEffectPacket layout (IDA-verified 1.26.42.01): Packet base 0x30,
// runtimeId u64 @ 0x30 ("Actor Runtime ID" per the packet's own field
// descriptor), instance vector @ 0x38, input tick @ 0x50, event u32 @ 0x58
// (ctor default 1 = Add).
class AntiEffect : public Module {
public:
    AntiEffect() : Module("AntiEffect", "Cancels selected effects client-side", Category::Player, 0) {
        AddSetting(new EnumSetting("Mode", "How effects are cancelled", 1,
            { "Purge", "Block All", "Both" }));                                                       // [0]
        AddSetting(new BoolSetting("Levitation", "", true));                                          // [1]
        AddSetting(new BoolSetting("Darkness", "", true));                                            // [2]
        AddSetting(new BoolSetting("Blindness", "", true));                                           // [3]
        AddSetting(new BoolSetting("Nausea", "", true));                                              // [4]
        AddSetting(new BoolSetting("Poison (incl. Fatal)", "", true));                                // [5]
        AddSetting(new BoolSetting("Wither", "", true));                                              // [6]
        AddSetting(new BoolSetting("Hunger", "", false));                                             // [7]
        AddSetting(new BoolSetting("Slowness", "", false));                                           // [8]
        AddSetting(new BoolSetting("Weakness", "", false));                                           // [9]
        AddSetting(new BoolSetting("Mining Fatigue", "", false));                                     // [10]
    }

    void OnUpdate() override;
    void OnPacketReceive(SDK::Packet* packet, int packetId, bool& cancel) override;

    int  GetMode() const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }
    bool GetLevitation()    const { return static_cast<BoolSetting*>(mSettings[1])->mValue; }
    bool GetDarkness()      const { return static_cast<BoolSetting*>(mSettings[2])->mValue; }
    bool GetBlindness()     const { return static_cast<BoolSetting*>(mSettings[3])->mValue; }
    bool GetNausea()        const { return static_cast<BoolSetting*>(mSettings[4])->mValue; }
    bool GetPoison()        const { return static_cast<BoolSetting*>(mSettings[5])->mValue; }
    bool GetWither()        const { return static_cast<BoolSetting*>(mSettings[6])->mValue; }
    bool GetHunger()        const { return static_cast<BoolSetting*>(mSettings[7])->mValue; }
    bool GetSlowness()      const { return static_cast<BoolSetting*>(mSettings[8])->mValue; }
    bool GetWeakness()      const { return static_cast<BoolSetting*>(mSettings[9])->mValue; }
    bool GetMiningFatigue() const { return static_cast<BoolSetting*>(mSettings[10])->mValue; }

private:
    bool isBlocked(int id) const;
};
