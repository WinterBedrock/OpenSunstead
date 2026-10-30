#pragma once

#include <cstdint>
#include <string>
#include <cstdio>

namespace SDK {

enum class EffectType : uint32_t {
    Empty = 0, Speed = 1, Slowness = 2, Haste = 3, MiningFatigue = 4,
    Strength = 5, InstantHealth = 6, InstantDamage = 7, JumpBoost = 8,
    Nausea = 9, Regeneration = 10, Resistance = 11, FireResistance = 12,
    WaterBreathing = 13, Invisibility = 14, Blindness = 15, NightVision = 16,
    Hunger = 17, Weakness = 18, Poison = 19, Wither = 20, HealthBoost = 21,
    Absorption = 22, Saturation = 23, Levitation = 24, FatalPoison = 25,
    ConduitPower = 26, SlowFalling = 27, BadOmen = 28, VillageHero = 29,
    Darkness = 30, TrialOmen = 31, WindCharged = 32, Weaving = 33,
    Oozing = 34, Infested = 35, RaidOmen = 36,
};

inline const char* effectTypeName(uint32_t id) {
    switch (static_cast<EffectType>(id)) {
    case EffectType::Empty: return "Empty";
    case EffectType::Speed: return "Speed";
    case EffectType::Slowness: return "Slowness";
    case EffectType::Haste: return "Haste";
    case EffectType::MiningFatigue: return "MiningFatigue";
    case EffectType::Strength: return "Strength";
    case EffectType::InstantHealth: return "InstantHealth";
    case EffectType::InstantDamage: return "InstantDamage";
    case EffectType::JumpBoost: return "JumpBoost";
    case EffectType::Nausea: return "Nausea";
    case EffectType::Regeneration: return "Regeneration";
    case EffectType::Resistance: return "Resistance";
    case EffectType::FireResistance: return "FireResistance";
    case EffectType::WaterBreathing: return "WaterBreathing";
    case EffectType::Invisibility: return "Invisibility";
    case EffectType::Blindness: return "Blindness";
    case EffectType::NightVision: return "NightVision";
    case EffectType::Hunger: return "Hunger";
    case EffectType::Weakness: return "Weakness";
    case EffectType::Poison: return "Poison";
    case EffectType::Wither: return "Wither";
    case EffectType::HealthBoost: return "HealthBoost";
    case EffectType::Absorption: return "Absorption";
    case EffectType::Saturation: return "Saturation";
    case EffectType::Levitation: return "Levitation";
    case EffectType::FatalPoison: return "FatalPoison";
    case EffectType::ConduitPower: return "ConduitPower";
    case EffectType::SlowFalling: return "SlowFalling";
    case EffectType::BadOmen: return "BadOmen";
    case EffectType::VillageHero: return "VillageHero";
    case EffectType::Darkness: return "Darkness";
    case EffectType::TrialOmen: return "TrialOmen";
    case EffectType::WindCharged: return "WindCharged";
    case EffectType::Weaving: return "Weaving";
    case EffectType::Oozing: return "Oozing";
    case EffectType::Infested: return "Infested";
    case EffectType::RaidOmen: return "RaidOmen";
    default: return "Unknown";
    }
}

inline std::string effectDurationString(int durationTicks, bool infinite) {
    if (infinite || durationTicks <= 0) return "inf";
    const int totalSec = durationTicks / 20;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", totalSec / 60, totalSec % 60);
    return buf;
}

inline constexpr uint32_t kMobEffectsComponentHash = 0xE6A1B550u;
// Prefer Offsets().EffectInstance_size at runtime; this is the known fallback only.
inline constexpr size_t   kMobEffectInstanceSize   = 0xA8;

// One active effect on an actor (from MobEffectsComponent vector).
struct MobEffectData {
    uint32_t id = 0;
    int amplifier = 0;
    int durationTicks = 0;
    bool infinite = false;
};

} // namespace SDK
