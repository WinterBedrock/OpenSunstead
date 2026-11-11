#pragma once

#include "../Module.h"
#include "../../SDK/World/World.h"
#include <imgui.h>
#include <mutex>
#include <unordered_map>
#include <vector>

class BlockESP : public Module {
public:
    BlockESP() : Module("BlockESP", "Draws boxes around selected blocks", Category::Visual, 0) {
        AddSetting(new EnumSetting("ESP Type", "Rendering style", 2, { "2D", "3D", "Outline" }));                // [0]
        AddSetting(new NumberSetting("Radius", "Search radius", 24.0f, 4.0f, 96.0f, 1.0f));                    // [1]
        AddSetting(new NumberSetting("Blocks/Tick", "Blocks scanned each tick", 4096.0f, 256.0f, 32768.0f, 256.0f)); // [2]
        AddSetting(new BoolSetting("Diamond", "Show diamond ore", true));                                      // [3]
        AddSetting(new BoolSetting("Emerald", "Show emerald ore", true));                                      // [4]
        AddSetting(new BoolSetting("Gold", "Show gold ore", true));                                            // [5]
        AddSetting(new BoolSetting("Iron", "Show iron ore", true));                                            // [6]
        AddSetting(new BoolSetting("Coal", "Show coal ore", true));                                            // [7]
        AddSetting(new BoolSetting("Redstone", "Show redstone ore", true));                                    // [8]
        AddSetting(new BoolSetting("Lapis", "Show lapis ore", true));                                          // [9]
        AddSetting(new BoolSetting("Portal", "Show portal blocks", true));                                     // [10]
        AddSetting(new BoolSetting("Chests", "Show chests/barrels", false));                                   // [11]
        AddSetting(new BoolSetting("Spawners", "Show monster spawners", false));                               // [12]
        AddSetting(new BoolSetting("Only Exposed", "Only show blocks touching air", false));                    // [13]
        AddSetting(new BoolSetting("Always show", "Render outside the HUD screen", false));                     // [14]
        AddSetting(new BoolSetting("Debug", "Print scan/render diagnostics to the console", false));            // [15]
        AddSetting(new BoolSetting("Fill", "Fill the box interior", true));                                      // [16]
        AddSetting(new NumberSetting("Fill Opacity", "Interior fill transparency", 0.20f, 0.0f, 1.0f, 0.01f));    // [17]
        AddSetting(new BoolSetting("Ancient Debris", "Show ancient debris", true));                             // [18]
        AddSetting(new BoolSetting("Merge", "Unite touching blocks of the same kind into one shape", true)); // [19]
        AddSetting(new BoolSetting("Shulker Boxes", "Show shulker boxes (all 17 color variants)", false));            // [20]
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
    std::unordered_map<SDK::BlockPos, FoundBlock, SDK::BlockPosHash> mFoundBlocks;
    std::unordered_map<SDK::BlockPos, FoundBlock, SDK::BlockPosHash> mScanBuffer;
    std::unordered_map<void*, int> mLegacyKindCache;

    void resetScan(const SDK::BlockPos& center, int radius);
};
