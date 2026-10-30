#include "BlockTracers.h"
#include "../../Render/FrameView.h"
#include "../../Utils/MCSDK.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../Setting.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <mutex>
#include <string>
#include <Windows.h>

// BlockTracers = BlockESP's block scan (radius / blocks-per-tick trigger + the
// same active-block selection) feeding Tracers-style lines. The scan half is a
// direct port of BlockESP::OnTick so the two modules track identical block sets;
// the render half draws one anchored line per found block instead of a box.

namespace {

enum BlockKind {
    BK_None = 0,
    BK_Diamond,
    BK_Emerald,
    BK_Gold,
    BK_Iron,
    BK_Coal,
    BK_Redstone,
    BK_Lapis,
    BK_Portal,
    BK_Chest,
    BK_Spawner,
    BK_Debris,
};

static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

// Identical classification to BlockESP (name-first, numeric-id fallback).
static int classifyLegacy(SDK::BlockLegacy* legacy) {
    if (!legacy) return BK_None;
    std::string n = lowerCopy(legacy->getName());
    if (!n.empty()) {
        if (contains(n, "air")) return BK_None;
        if (contains(n, "diamond_ore")) return BK_Diamond;
        if (contains(n, "emerald_ore")) return BK_Emerald;
        if (contains(n, "gold_ore")) return BK_Gold;
        if (contains(n, "iron_ore")) return BK_Iron;
        if (contains(n, "coal_ore")) return BK_Coal;
        if (contains(n, "redstone_ore")) return BK_Redstone;
        if (contains(n, "lapis_ore")) return BK_Lapis;
        // Ancient debris ("ancient_debris"). Match the bare word too so neither
        // namespace prefixes nor future renames break the check.
        if (contains(n, "ancient_debris") || contains(n, "debris")) return BK_Debris;
        if (contains(n, "portal")) return BK_Portal;
        if (contains(n, "chest") || contains(n, "barrel")) return BK_Chest;
        if (contains(n, "spawner")) return BK_Spawner;
    }

    uint16_t id = legacy->getBlockId();
    switch (id) {
        case 56: case 660: return BK_Diamond;
        case 129: case 662: return BK_Emerald;
        case 14: case 657: return BK_Gold;
        case 15: case 656: return BK_Iron;
        case 16: case 661: return BK_Coal;
        case 73: case 74: case 658: case 659: return BK_Redstone;
        case 21: case 655: return BK_Lapis;
        case 90: return BK_Portal;
        case 54: case 130: case 146: case 458: return BK_Chest;
        case 52: return BK_Spawner;
        default: break;
    }
    return BK_None;
}

// Maps a BlockKind to its selection toggle. Setting indices here match the
// BlockTracers header ([6..15]) -- the ONLY difference from BlockESP's helper.
static bool kindEnabled(const BlockTracers* self, int kind) {
    switch (kind) {
        case BK_Diamond:  return static_cast<BoolSetting*>(self->mSettings[6])->mValue;
        case BK_Emerald:  return static_cast<BoolSetting*>(self->mSettings[7])->mValue;
        case BK_Gold:     return static_cast<BoolSetting*>(self->mSettings[8])->mValue;
        case BK_Iron:     return static_cast<BoolSetting*>(self->mSettings[9])->mValue;
        case BK_Coal:     return static_cast<BoolSetting*>(self->mSettings[10])->mValue;
        case BK_Redstone: return static_cast<BoolSetting*>(self->mSettings[11])->mValue;
        case BK_Lapis:    return static_cast<BoolSetting*>(self->mSettings[12])->mValue;
        case BK_Portal:   return static_cast<BoolSetting*>(self->mSettings[13])->mValue;
        case BK_Chest:    return static_cast<BoolSetting*>(self->mSettings[14])->mValue;
        case BK_Spawner:  return static_cast<BoolSetting*>(self->mSettings[15])->mValue;
        case BK_Debris:   return static_cast<BoolSetting*>(self->mSettings[18])->mValue;
        default: return false;
    }
}

static ImColor colorForKind(int kind) {
    switch (kind) {
        case BK_Diamond:  return ImColor(0.0f, 1.0f, 1.0f, 1.0f);
        case BK_Emerald:  return ImColor(0.0f, 1.0f, 0.25f, 1.0f);
        case BK_Gold:     return ImColor(1.0f, 0.9f, 0.0f, 1.0f);
        case BK_Iron:     return ImColor(1.0f, 0.55f, 0.15f, 1.0f);
        case BK_Coal:     return ImColor(0.12f, 0.12f, 0.14f, 1.0f);
        case BK_Redstone: return ImColor(1.0f, 0.05f, 0.05f, 1.0f);
        case BK_Lapis:    return ImColor(0.1f, 0.25f, 1.0f, 1.0f);
        case BK_Portal:   return ImColor(0.55f, 0.1f, 0.85f, 1.0f);
        case BK_Chest:    return ImColor(1.0f, 0.55f, 0.0f, 1.0f);
        case BK_Spawner:  return ImColor(0.25f, 0.25f, 0.8f, 1.0f);
        case BK_Debris:   return ImColor(0.55f, 0.35f, 0.2f, 1.0f);
        default: return ImColor(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

static bool exposedToAir(SDK::BlockSource* src, const SDK::BlockPos& p) {
    static const SDK::BlockPos offsets[6] = {
        {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
    };
    for (const auto& o : offsets) {
        if (src->isAir(SDK::BlockPos(p.x + o.x, p.y + o.y, p.z + o.z))) return true;
    }
    return false;
}

// mat4Mul / worldToScreen are consolidated in SDK/Math.h (mat4Multiply,
// worldToScreen; both global, resolved via ADL).

static bool readActorPos(SDK::Actor* actor, Vec3* out) {
    if (!actor || !out) return false;
    __try {
        *out = actor->getPos();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void BlockTracers::resetScan(const SDK::BlockPos& center, int radius) {
    mCenter = center;
    mRadius = radius;
    mCursor = 0;
    // Keep mFoundBlocks intact until the new pass completes (no flicker on
    // recenter), just like BlockESP.
    mScanBuffer.clear();
    mLegacyKindCache.clear();
}

void BlockTracers::OnEnable() {
    std::lock_guard<std::mutex> lock(mBlocksMutex);
    mFoundBlocks.clear();
    mScanBuffer.clear();
    mLegacyKindCache.clear();
    mCursor = 0;
}

void BlockTracers::OnDisable() {
    std::lock_guard<std::mutex> lock(mBlocksMutex);
    mFoundBlocks.clear();
    mScanBuffer.clear();
    mLegacyKindCache.clear();
    mCursor = 0;
}

void BlockTracers::OnTick() {
    if (!IsEnabled()) return;
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp || !sdk_safe_ptr(lp, 0x230)) return;
    SDK::BlockSource* src = ci->getBlockSource();
    if (!src) return;

    Vec3 pos;
    if (!readActorPos(lp, &pos)) return;

    int radius = (int)static_cast<NumberSetting*>(mSettings[4])->mValue;
    if (radius < 4) radius = 4;
    SDK::BlockPos center((int)std::floor(pos.x), (int)std::floor(pos.y), (int)std::floor(pos.z));
    if (radius != mRadius ||
        std::abs(center.x - mCenter.x) > 8 || std::abs(center.y - mCenter.y) > 8 || std::abs(center.z - mCenter.z) > 8) {
        resetScan(center, radius);
    }

    int side = radius * 2 + 1;
    int total = side * side * side;
    int budget = (int)static_cast<NumberSetting*>(mSettings[5])->mValue;
    if (budget < 256) budget = 256;
    bool onlyExposed = static_cast<BoolSetting*>(mSettings[16])->mValue;
    int r2 = radius * radius;

    // Progressive, double-buffered scan (identical to BlockESP): accumulate the
    // current pass into mScanBuffer (no lock), then swap into mFoundBlocks under
    // the lock only when a full pass completes -> render always sees a complete
    // set, never a half-refilled one.
    for (int i = 0; i < budget && total > 0; ++i) {
        if (mCursor == 0) mScanBuffer.clear();   // begin a fresh pass
        int cur = mCursor++;
        bool passDone = (mCursor >= total);

        int lx = cur % side;
        int ly = (cur / side) % side;
        int lz = cur / (side * side);
        int dx = lx - radius, dy = ly - radius, dz = lz - radius;
        if (dx * dx + dy * dy + dz * dz <= r2) {
            SDK::BlockPos bp(center.x + dx, center.y + dy, center.z + dz);
            SDK::Block* block = src->getBlock(bp);
            SDK::BlockLegacy* legacy = block ? block->getLegacy() : nullptr;
            if (legacy) {
                int kind = BK_None;
                auto it = mLegacyKindCache.find(legacy);
                if (it != mLegacyKindCache.end()) {
                    kind = it->second;
                } else {
                    kind = classifyLegacy(legacy);
                    mLegacyKindCache[legacy] = kind;
                }

                bool isEnabledKind = kindEnabled(this, kind);
                bool exposedOk = !onlyExposed || exposedToAir(src, bp);
                if (isEnabledKind && exposedOk) {
                    mScanBuffer[bp] = { bp, colorForKind(kind), kind };
                }
            }
        }

        if (passDone) {
            std::lock_guard<std::mutex> lock(mBlocksMutex);
            mFoundBlocks.swap(mScanBuffer);
            mCursor = 0;
            break;
        }
    }
}

void BlockTracers::OnRender() {
    if (!IsEnabled()) return;
    // "Always show" off (default) -> only render on the in-world HUD screen.
    if (!static_cast<BoolSetting*>(mSettings[17])->mValue &&
        SDK::ScreenView::currentScreenName() != "hud_screen") return;

    std::vector<FoundBlock> blocks;
    {
        std::lock_guard<std::mutex> lock(mBlocksMutex);
        if (mFoundBlocks.empty()) return;
        blocks.reserve(mFoundBlocks.size());
        for (const auto& kv : mFoundBlocks) blocks.push_back(kv.second);
    }

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) return;

    // Camera MVP via the shared delayed-transform acquisition (FrameView.h).
    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) return;

    // Draw once per frame (OnRender can be invoked more than once per frame).
    int frame = ImGui::GetFrameCount();
    static int s_drawnFrame = -1;
    if (s_drawnFrame == frame) return;
    s_drawnFrame = frame;

    int   anchor    = static_cast<EnumSetting*>(mSettings[0])->mValue;   // 0 top,1 center,2 bottom
    int   colMode   = static_cast<EnumSetting*>(mSettings[1])->mValue;   // 0 block,1 theme,2 custom
    float* ccol     = static_cast<ColorSetting*>(mSettings[2])->mValue;
    float thickness = static_cast<NumberSetting*>(mSettings[3])->mValue;
    uint64_t nowMs  = GetTickCount64();

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImVec2 from(screen.x * 0.5f,
                anchor == 0 ? 0.0f : (anchor == 1 ? screen.y * 0.5f : screen.y));

    for (const FoundBlock& fb : blocks) {
        // Line endpoint = the block's centre in world space.
        Vec3 center((float)fb.pos.x + 0.5f, (float)fb.pos.y + 0.5f, (float)fb.pos.z + 0.5f);
        ImVec2 to;
        if (!worldToScreen(center, mvp, origin, screen, &to)) continue;  // skip behind camera

        ImU32 color;
        if (colMode == 0) {
            color = fb.color;                                            // per-block ore colour
        } else if (colMode == 1) {
            color = (ImU32)ColorUtils::getThemedColor(0.0f, nowMs);      // themed
        } else {
            color = ImGui::ColorConvertFloat4ToU32(ImVec4(ccol[0], ccol[1], ccol[2], ccol[3])); // custom
        }

        // Dark underlay + coloured line for legibility (matches BlockESP style).
        dl->AddLine(from, to, ImColor(0, 0, 0, 160), thickness + 1.5f);
        dl->AddLine(from, to, color, thickness);
    }
}
