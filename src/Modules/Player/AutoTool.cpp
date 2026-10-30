#include "AutoTool.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../Setting.h"
#include "../../Core/Logger.h"
#include <string>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include <Windows.h>

SUNSTEAD_DEFINE_DBG(atDbg, "AutoTool", 300)

// ---- tool families ----------------------------------------------------------
enum ToolType { TT_NONE = 0, TT_PICKAXE, TT_AXE, TT_SHOVEL, TT_SWORD, TT_SHEARS, TT_HOE };

static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// Classify a held item name into {tool family, material tier}. tier: 0=non-tool,
// 1 wood, 2 gold, 3 stone, 4 iron, 5 diamond, 6 netherite.
static ToolType classifyTool(const std::string& n, int& tierOut) {
    tierOut = 0;
    ToolType t = TT_NONE;
    if (has(n, "pickaxe"))      t = TT_PICKAXE; // check before "axe"
    else if (has(n, "axe"))     t = TT_AXE;
    else if (has(n, "shovel"))  t = TT_SHOVEL;
    else if (has(n, "sword"))   t = TT_SWORD;
    else if (has(n, "shears"))  t = TT_SHEARS;
    else if (has(n, "hoe"))     t = TT_HOE;
    if (t == TT_NONE) return TT_NONE;

    if (has(n, "netherite"))                 tierOut = 6;
    else if (has(n, "diamond"))              tierOut = 5;
    else if (has(n, "iron"))                 tierOut = 4;
    else if (has(n, "stone"))                tierOut = 3;
    else if (has(n, "gold"))                 tierOut = 2; // "golden_*"
    else if (has(n, "wood"))                 tierOut = 1; // "wooden_*"
    else                                     tierOut = 1; // unknown tier -> lowest usable
    return t;
}

// Map a block identifier to the ideal tool family. Order matters: more specific
// families are tested first so overlapping substrings resolve correctly
// (e.g. "*_brick" stays a pickaxe target, "concrete_powder" a shovel target).
static ToolType blockTool(const std::string& n) {
    if (has(n, "web"))                                        return TT_SWORD;
    if (has(n, "leaves") || has(n, "wool") || has(n, "vine")) return TT_SHEARS;

    // Axe family (wood + wood-like decoration/utility blocks).
    if (has(n, "log") || has(n, "wood") || has(n, "plank") || has(n, "fence") ||
        has(n, "door") || has(n, "chest") || has(n, "crafting_table") || has(n, "pumpkin") ||
        has(n, "melon") || has(n, "bookshelf") || has(n, "barrel") || has(n, "ladder") ||
        has(n, "sign") || has(n, "stripped") || has(n, "stem") || has(n, "hyphae") ||
        has(n, "bamboo") || has(n, "mangrove_roots") || has(n, "beehive") || has(n, "bee_nest") ||
        has(n, "jukebox") || has(n, "note_block") || has(n, "lectern") || has(n, "composter") ||
        has(n, "cartography") || has(n, "fletching") || has(n, "smithing_table") || has(n, "loom"))
        return TT_AXE;

    // Shovel family (soft granular / soil blocks). Exclude stone-bricks/sandstone
    // which merely share the "sand"/"brick" substrings.
    if ((has(n, "dirt") || has(n, "grass") || has(n, "gravel") || has(n, "clay") ||
         has(n, "soul_sand") || has(n, "soul_soil") || has(n, "snow") || has(n, "podzol") ||
         has(n, "mycelium") || has(n, "farmland") || has(n, "path") || has(n, "concrete_powder") ||
         has(n, "mud") || has(n, "sand")) &&
        !has(n, "sandstone") && !has(n, "brick"))
        return TT_SHOVEL;

    // Pickaxe family (hard blocks). Broad fallback for anything stone/metal/ore.
    if (has(n, "stone") || has(n, "cobble") || has(n, "ore") || has(n, "deepslate") ||
        has(n, "granite") || has(n, "diorite") || has(n, "andesite") || has(n, "obsidian") ||
        has(n, "netherrack") || has(n, "brick") || has(n, "concrete") || has(n, "terracotta") ||
        has(n, "furnace") || has(n, "anvil") || has(n, "blackstone") || has(n, "basalt") ||
        has(n, "end_stone") || has(n, "purpur") || has(n, "prismarine") || has(n, "sandstone") ||
        has(n, "quartz") || has(n, "amethyst") || has(n, "calcite") || has(n, "tuff") ||
        has(n, "dripstone") || has(n, "magma") || has(n, "copper") || has(n, "lapis") ||
        has(n, "redstone") || has(n, "sea_lantern") || has(n, "glazed") || has(n, "polished") ||
        has(n, "smooth") || has(n, "rail") || has(n, "cauldron") || has(n, "hopper") ||
        has(n, "iron") || has(n, "gold") || has(n, "diamond") || has(n, "emerald") ||
        has(n, "netherite") || has(n, "coal") || has(n, "_block"))
        return TT_PICKAXE;

    return TT_NONE;
}

// Convert a material tier to a ranking score. Default ranks by tier (netherite
// best). Prefer-Speed ranks by raw dig-speed multiplier (gold fastest).
static int rankScore(int tier, bool preferSpeed) {
    if (!preferSpeed) return tier; // 6=netherite .. 1=wood
    switch (tier) {                // gold's dig speed beats netherite
        case 2: return 6; // gold
        case 6: return 5; // netherite
        case 5: return 4; // diamond
        case 4: return 3; // iron
        case 3: return 2; // stone
        default: return 1; // wood / unknown
    }
}

// ---- context / inventory helpers -------------------------------------------
// Context resolution is SDK::resolveContext (GameContext).

static SDK::Actor* sehLocalPlayer(SDK::ClientInstance* ci) {
    SDK::Actor* lp = ci->getLocalPlayer();
    return (lp && sdk_safe_ptr(lp, Layout::Probe::Actor)) ? lp : nullptr;
}

// Returns true and fills pos with the crosshair's BLOCK hit, or false otherwise.
static bool sehGetHitBlock(SDK::Level* level, SDK::BlockPos& pos) {
    SDK::HitResult* hr = level->getHitResult(); // internally SEH-guarded
    if (!hr || !sdk_safe_ptr(hr, 0x30)) return false;
    __try {
        if (hr->mType != SDK::HitType::BLOCK) return false;
        pos = SDK::BlockPos(hr->mBlockPos[0], hr->mBlockPos[1], hr->mBlockPos[2]);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static std::string sehGetBlockName(SDK::ClientInstance* ci, const SDK::BlockPos& pos) {
    SDK::BlockSource* bs = ci->getBlockSource();
    if (!bs) return "";
    SDK::Block* b = bs->getBlock(pos);
    if (!b) return "";
    SDK::BlockLegacy* l = b->getLegacy();
    if (!l) return "";
    std::string n = l->getName(); // internally SEH-guarded
    for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
    return n;
}

// Container / slot access is SDK::Inv (see SDK/Inventory/InventoryAccess.h).

// Best hotbar slot (0..8) holding a tool of `want` (with a shears->sword
// fallback for foliage). Returns -1 if no suitable tool is in the hotbar.
// getItemName builds a std::string and is internally SEH-guarded, so no __try
// wraps this scan (a __try around std::string is C2712).
static int findBestTool(SDK::Actor* lp, ToolType want, bool preferSpeed) {
    SDK::Container* c = SDK::Inv::container(lp);
    if (!c) return -1;
    int bestSlot = -1, bestScore = -1, bestWant = -1; // bestWant: 1 primary, 0 fallback
    for (int slot = 0; slot < 9; ++slot) {
        SDK::ItemStack* is = c->getItem(slot);
        if (!is) continue;
        std::string n = is->getItemName();
        if (n.empty()) continue;
        for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
        int tier = 0;
        ToolType t = classifyTool(n, tier);
        if (t == TT_NONE) continue;

        int wantRank; // 1 = exact family, 0 = acceptable fallback
        if (t == want) wantRank = 1;
        else if (want == TT_SHEARS && t == TT_SWORD) wantRank = 0; // foliage fallback
        else continue;

        int score = rankScore(tier, preferSpeed);
        if (wantRank > bestWant || (wantRank == bestWant && score > bestScore)) {
            bestWant = wantRank; bestScore = score; bestSlot = slot;
        }
    }
    return bestSlot;
}

void AutoTool::OnUpdate() {
    if (!IsEnabled()) return;
    const bool dbg = GetDebug();

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx, true)) return;
    SDK::ClientInstance* ci = ctx.ci;
    SDK::Level* level = ctx.level;
    SDK::Actor* lp = ctx.lp;
    if (!lp) return;

    // "mining" gate: crosshair on a block, and (optionally) the destroy button held.
    SDK::BlockPos hit;
    bool onBlock = sehGetHitBlock(level, hit);
    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool active = onBlock && (!GetOnlyWhileMining() || lmb);

    if (!active) {
        // Not mining: restore the pre-switch slot if we changed it.
        if (mSwitched && GetSwitchBack() && mPrevSlot >= 0 && mPrevSlot <= 8) {
            SDK::Inv::setSelectedSlot(lp, mPrevSlot);
            atDbg(dbg, "idle -> restored slot %d", mPrevSlot);
        }
        mSwitched = false;
        mPrevSlot = -1;
        return;
    }

    std::string name = sehGetBlockName(ci, hit);
    if (name.empty()) return;

    ToolType want = blockTool(name);
    if (want == TT_NONE) return; // no ideal tool -> leave the current slot alone

    int best = findBestTool(lp, want, GetPreferSpeed());
    if (best < 0) { atDbg(dbg, "block '%s' want=%d, no tool in hotbar", name.c_str(), (int)want); return; }

    int cur = SDK::Inv::selectedSlot(lp);
    if (cur == best) return; // already holding the best tool

    if (!mSwitched) mPrevSlot = cur; // remember the original slot once per session
    SDK::Inv::setSelectedSlot(lp, best);
    mSwitched = true;
    atDbg(dbg, "block '%s' want=%d -> slot %d (from %d)", name.c_str(), (int)want, best, cur);
}

void AutoTool::OnDisable() {
    if (mSwitched && GetSwitchBack() && mPrevSlot >= 0 && mPrevSlot <= 8) {
        SDK::GameContext ctx;
        if (SDK::resolveContext(ctx, false) && ctx.lp)
            SDK::Inv::setSelectedSlot(ctx.lp, mPrevSlot);
    }
    mSwitched = false;
    mPrevSlot = -1;
}
