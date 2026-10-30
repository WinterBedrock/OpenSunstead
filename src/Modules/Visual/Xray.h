#pragma once
#include "../Module.h"
#include "../../SDK/World/World.h"
#include <imgui.h>
#include <mutex>
#include <unordered_map>

// Xray — every block texture in the world becomes empty, ores keep theirs.
//
// HOW IT WORKS (v3 — the texture-atlas redirect)
//
// The world draws every block face with a UV rect into the stitched terrain
// atlas, resolved through ONE map walker inside the game
// (TextureAtlas::lookupEntry — the FNV-1 unordered_map<string, AtlasEntry>
// lookup at sub_145B0D2E0 on 1.26.50.04; the chunk mesher, the block-break
// overlay and the persona builder all go through it).
//
// Xray hooks that walker and answers with the entry of the fully transparent
// "structure_void" tile for every texture name that is not an ore (matched by
// basename, so "diamond_ore" and "textures/blocks/deepslate/
// deepslate_diamond_ore" style keys both classify). Result: the world is
// literally textured with emptiness — no boxes, no scanning, no fake colour —
// and the ore tiles keep their real pixels. The redirect target is verified
// by the entry's own name before use, so a bad lookup falls back to vanilla
// rendering instead of the magenta "missing" tile.
//
// The ore toggles feed a keep-mask the hook reads; changing one applies to
// the very next tessellation. Toggling the module is instant at the lookup
// level (the next lookup answers with the real entries again).
//
// The old BlockESP-style overlay survives as an OPT-IN extra ("Overlay
// Boxes", off by default): the budget-limited progressive scan + boxes/fill
// through the frame-locked W2S camera. "Darkness" (a full-screen dim plate)
// works with or without the boxes. Chests/spawners are block entities with
// their own renderer and are not part of the block atlas — they are covered
// by the box overlay, not the texture redirect.
class Xray : public Module {
public:
    Xray() : Module("Xray", "See ores and valuables through the ground", Category::Visual, 0) {
        AddSetting(new EnumSetting("Style", "Box rendering style", 0, { "3D", "2D", "Outline" }));       // [0]
        AddSetting(new NumberSetting("Radius", "Search radius (blocks)", 48.0f, 16.0f, 128.0f, 1.0f));   // [1]
        AddSetting(new NumberSetting("Blocks/Tick", "Blocks scanned each tick", 16384.0f, 256.0f, 32768.0f, 256.0f)); // [2]
        AddSetting(new BoolSetting("Fill", "Fill box interiors", true));                                  // [3]
        AddSetting(new NumberSetting("Fill Opacity", "Interior fill transparency", 0.30f, 0.0f, 1.0f, 0.01f)); // [4]
        AddSetting(new BoolSetting("Only Exposed", "Only highlight ores with open air next to them", false)); // [5]
        AddSetting(new BoolSetting("Darkness", "Dim the world so the ores stand out", false));            // [6]
        AddSetting(new NumberSetting("Darkness Opacity", "How dark the world becomes", 0.88f, 0.0f, 1.0f, 0.01f)); // [7]
        AddSetting(new BoolSetting("Diamond", "Show diamond ore", true));                                 // [8]
        AddSetting(new BoolSetting("Iron", "Show iron ore", true));                                       // [9]
        AddSetting(new BoolSetting("Gold", "Show gold ore", true));                                       // [10]
        AddSetting(new BoolSetting("Coal", "Show coal ore", false));                                      // [11]
        AddSetting(new BoolSetting("Redstone", "Show redstone ore", false));                              // [12]
        AddSetting(new BoolSetting("Lapis", "Show lapis ore", true));                                     // [13]
        AddSetting(new BoolSetting("Emerald", "Show emerald ore", true));                                 // [14]
        AddSetting(new BoolSetting("Copper", "Show copper ore", true));                                   // [15]
        AddSetting(new BoolSetting("Ancient Debris", "Show ancient debris", true));                       // [16]
        AddSetting(new BoolSetting("Chests", "Show chests/barrels", false));                              // [17]
        AddSetting(new BoolSetting("Spawners", "Show monster spawners", false));                          // [18]
        AddSetting(new BoolSetting("Debug", "Print scan/render diagnostics to the console", false));      // [19]
        AddSetting(new BoolSetting("Overlay Boxes", "Also draw ore highlight boxes (the old BlockESP-style overlay) on top of the texture xray", false)); // [20]
        canBind = true;
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;
    void OnRender() override;

private:
    struct FoundBlock {
        SDK::BlockPos pos;
        ImColor color;
        int kind = 0;
    };

    SDK::BlockPos mCenter{};
    int mRadius = 0;
    int mCursor = 0;
    mutable std::mutex mBlocksMutex;
    // Published set the render thread reads. Only ever replaced atomically (swap)
    // when a full scan pass completes, so boxes stay stable instead of blinking
    // as the progressive scan refills them.
    std::unordered_map<SDK::BlockPos, FoundBlock, SDK::BlockPosHash> mFoundBlocks;
    // Scratch set the tick thread accumulates the in-progress pass into.
    std::unordered_map<SDK::BlockPos, FoundBlock, SDK::BlockPosHash> mScanBuffer;
    std::unordered_map<void*, int> mLegacyKindCache;

    void resetScan(const SDK::BlockPos& center, int radius);
};

// Retexture sweep control (see Xray.cpp "RETEXTURE SWEEP"). Started on module
// enable/disable and on ore-toggle changes; drained every game tick from
// ModuleManager::OnTick (so the post-disable sweep finishes too).
void Xray_StartRetextureSweep(int radiusBlocks);
void Xray_StepRetextureSweep();
