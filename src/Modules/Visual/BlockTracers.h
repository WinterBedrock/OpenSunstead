#pragma once

#include "../Module.h"
#include "../../SDK/World/World.h"
#include <imgui.h>
#include <mutex>
#include <unordered_map>
#include <vector>

// BlockTracers -- draws a line from a screen anchor toward every selected block
// in range. It is to blocks what the Tracers module is to players: same anchor /
// color / thickness options as Tracers, but the endpoints are world blocks found
// by the SAME progressive, double-buffered scan the BlockESP module uses (same
// radius / blocks-per-tick trigger and the same active-block selection toggles).
//
// The scan runs on the tick thread and publishes a completed set atomically; the
// render thread reads that set and draws a line per block. This mirrors BlockESP
// exactly so the two modules behave identically w.r.t. which blocks are tracked.
class BlockTracers : public Module {
public:
    BlockTracers() : Module("BlockTracers", "Draws lines toward selected blocks", Category::Visual, 0) {
        // ---- Tracer line appearance (mirrors the Tracers module) ----
        AddSetting(new EnumSetting("Anchor", "Where lines originate", 2, { "Top", "Center", "Bottom" })); // [0]
        AddSetting(new EnumSetting("Color", "Line color source", 0, { "Block", "Theme", "Custom" }));      // [1]
        AddSetting(new ColorSetting("Custom Color", "Custom line color", 0.2f, 1.0f, 1.0f, 1.0f));          // [2]
        AddSetting(new NumberSetting("Thickness", "Line thickness", 1.5f, 0.5f, 5.0f, 0.1f));               // [3]

        // ---- Scan / trigger (mirrors BlockESP) ----
        AddSetting(new NumberSetting("Radius", "Search radius", 24.0f, 4.0f, 96.0f, 1.0f));                      // [4]
        AddSetting(new NumberSetting("Blocks/Tick", "Blocks scanned each tick", 4096.0f, 256.0f, 32768.0f, 256.0f)); // [5]

        // ---- Active-block selection (identical set to BlockESP) ----
        AddSetting(new BoolSetting("Diamond", "Show diamond ore", true));           // [6]
        AddSetting(new BoolSetting("Emerald", "Show emerald ore", true));           // [7]
        AddSetting(new BoolSetting("Gold", "Show gold ore", true));                 // [8]
        AddSetting(new BoolSetting("Iron", "Show iron ore", true));                 // [9]
        AddSetting(new BoolSetting("Coal", "Show coal ore", true));                 // [10]
        AddSetting(new BoolSetting("Redstone", "Show redstone ore", true));         // [11]
        AddSetting(new BoolSetting("Lapis", "Show lapis ore", true));               // [12]
        AddSetting(new BoolSetting("Portal", "Show portal blocks", true));          // [13]
        AddSetting(new BoolSetting("Chests", "Show chests/barrels", false));        // [14]
        AddSetting(new BoolSetting("Spawners", "Show monster spawners", false));    // [15]

        AddSetting(new BoolSetting("Only Exposed", "Only track blocks touching air", false)); // [16]
        AddSetting(new BoolSetting("Always show", "Render on every screen, not just the in-world HUD", false)); // [17]
        AddSetting(new BoolSetting("Ancient Debris", "Track ancient debris", true));          // [18]
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
    // Completed set the render thread reads (swapped in atomically per pass).
    std::unordered_map<SDK::BlockPos, FoundBlock, SDK::BlockPosHash> mFoundBlocks;
    // In-progress accumulation buffer (tick-thread only).
    std::unordered_map<SDK::BlockPos, FoundBlock, SDK::BlockPosHash> mScanBuffer;
    std::unordered_map<void*, int> mLegacyKindCache;

    void resetScan(const SDK::BlockPos& center, int radius);
};
