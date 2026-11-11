#pragma once
#include "../Module.h"
#include "../../SDK/World/World.h"
#include <imgui.h>
#include <mutex>
#include <unordered_map>

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
    std::unordered_map<SDK::BlockPos, FoundBlock, SDK::BlockPosHash> mFoundBlocks;
    // Scratch set the tick thread accumulates the in-progress pass into.
    std::unordered_map<SDK::BlockPos, FoundBlock, SDK::BlockPosHash> mScanBuffer;
    std::unordered_map<void*, int> mLegacyKindCache;

    void resetScan(const SDK::BlockPos& center, int radius);
};

void Xray_StartRetextureSweep(int radiusBlocks);
void Xray_StepRetextureSweep();
