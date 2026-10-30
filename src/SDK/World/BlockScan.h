#pragma once
// Sunstead SDK — shared block classification + projection for block overlays
// (BlockESP/Xray). Needs the World types and worldToScreen — include after
// World.h / Math.h.
#include "World.h"
#include "../Math.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <string>

namespace SDK::BlockScan {

// Valuable-block kinds. Values are stable (persisted configs index settings by
// position, kinds feed those toggles).
enum Kind {
    Kind_None = 0,
    Kind_Diamond,
    Kind_Emerald,
    Kind_Gold,
    Kind_Iron,
    Kind_Copper,
    Kind_Coal,
    Kind_Redstone,
    Kind_Lapis,
    Kind_Portal,
    Kind_Chest,
    Kind_Spawner,
    Kind_Debris,
    Kind_Shulker,
};

inline std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

inline bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

// Name-first classification with the block-id table as the fallback: names
// survive data-driven renumbering, ids catch renamed legacy blocks.
inline int classify(BlockLegacy* legacy) {
    if (!legacy) return Kind_None;
    std::string n = lowerCopy(legacy->getName());
    if (!n.empty()) {
        if (contains(n, "air")) return Kind_None;
        if (contains(n, "diamond_ore")) return Kind_Diamond;
        if (contains(n, "emerald_ore")) return Kind_Emerald;
        if (contains(n, "gold_ore")) return Kind_Gold;
        if (contains(n, "iron_ore")) return Kind_Iron;
        if (contains(n, "copper_ore")) return Kind_Copper;
        if (contains(n, "coal_ore")) return Kind_Coal;
        if (contains(n, "redstone_ore") || contains(n, "lit_redstone_ore")) return Kind_Redstone;
        if (contains(n, "lapis_ore")) return Kind_Lapis;
        // Ancient debris — match the bare word too so neither namespace
        // prefixes nor future renames break the check.
        if (contains(n, "ancient_debris") || contains(n, "debris")) return Kind_Debris;
        if (contains(n, "portal")) return Kind_Portal;
        if (contains(n, "shulker_box")) return Kind_Shulker;   // base + 16 dyes
        if (contains(n, "chest") || contains(n, "barrel")) return Kind_Chest;
        if (contains(n, "spawner")) return Kind_Spawner;
    }

    uint16_t id = legacy->getBlockId();
    switch (id) {
        case 56: case 660: return Kind_Diamond;
        case 129: case 662: return Kind_Emerald;
        case 14: case 657: return Kind_Gold;
        case 15: case 656: return Kind_Iron;
        case 16: case 661: return Kind_Coal;
        case 73: case 74: case 658: case 659: return Kind_Redstone;
        case 21: case 655: return Kind_Lapis;
        case 90: return Kind_Portal;
        case 54: case 130: case 146: case 458: return Kind_Chest;
        case 52: return Kind_Spawner;
        default: break;
    }
    return Kind_None;
}

inline ImColor colorFor(int kind) {
    switch (kind) {
        case Kind_Diamond:  return ImColor(0.0f, 1.0f, 1.0f, 1.0f);
        case Kind_Emerald:  return ImColor(0.0f, 1.0f, 0.25f, 1.0f);
        case Kind_Gold:     return ImColor(1.0f, 0.9f, 0.0f, 1.0f);
        case Kind_Iron:     return ImColor(1.0f, 0.55f, 0.15f, 1.0f);
        case Kind_Copper:   return ImColor(1.0f, 0.65f, 0.5f, 1.0f);
        case Kind_Coal:     return ImColor(0.12f, 0.12f, 0.14f, 1.0f);
        case Kind_Redstone: return ImColor(1.0f, 0.05f, 0.05f, 1.0f);
        case Kind_Lapis:    return ImColor(0.1f, 0.25f, 1.0f, 1.0f);
        case Kind_Portal:   return ImColor(0.55f, 0.1f, 0.85f, 1.0f);
        case Kind_Chest:    return ImColor(1.0f, 0.55f, 0.0f, 1.0f);
        case Kind_Spawner:  return ImColor(0.25f, 0.25f, 0.8f, 1.0f);
        case Kind_Debris:   return ImColor(0.55f, 0.35f, 0.2f, 1.0f);
        case Kind_Shulker:  return ImColor(0.60f, 0.25f, 0.85f, 1.0f);   // shulker purple
        default: return ImColor(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

// Project one block's 8 corners with the shared MVP. Returns the number of
// corners in front of the camera; fills out/valid.
inline int projectBlockBox(const BlockPos& p, const float* mvp, const Vec3& origin,
                           const ImVec2& screen, ImVec2 out[8], bool valid[8]) {
    Vec3 mn((float)p.x, (float)p.y, (float)p.z);
    Vec3 mx((float)p.x + 1.0f, (float)p.y + 1.0f, (float)p.z + 1.0f);
    const Vec3 c[8] = {
        { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mx.x, mn.y, mx.z }, { mn.x, mn.y, mx.z },
        { mn.x, mx.y, mn.z }, { mx.x, mx.y, mn.z }, { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z },
    };
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        valid[i] = worldToScreen(c[i], mvp, origin, screen, &out[i]);
        if (valid[i]) ++n;
    }
    return n;
}

// True when any of the six neighbours is air (the "visible from a cave"
// filter used by Xray/BlockESP's Only Exposed option).
inline bool exposedToAir(BlockSource* src, const BlockPos& p) {
    static const BlockPos offsets[6] = {
        {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
    };
    for (const auto& o : offsets) {
        if (src->isAir(BlockPos(p.x + o.x, p.y + o.y, p.z + o.z))) return true;
    }
    return false;
}

} // namespace SDK::BlockScan
