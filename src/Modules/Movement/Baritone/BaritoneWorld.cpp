#include "BaritoneWorld.h"
#include "../../../SDK/World/World.h"

#include <cstring>
#include <string>

namespace baritone {
namespace {

// Block classification tables. Substrings because Bedrock names are versioned
// and prefixed inconsistently (tall_grass / short_grass / legacy grass).
// kSolidExceptions is checked FIRST: grass_block contains "grass" and must
// stay solid or every cell in the world stops being standable.
const char* const kSolidExceptions[] = {
    "grass_block", "grass_path", "dirt_with_roots", "mushroom_block",
    "mushroom_stem", "dried_kelp_block", "end_portal_frame", "flowering_azalea",
    "azalea_leaves", "candle_cake", "glass",
};

const char* const kClearNames[] = {
    "air",
    "grass", "fern", "seagrass", "kelp", "vine", "lily",
    "sapling", "flower", "tulip", "orchid", "allium", "daisy", "poppy",
    "dandelion", "cornflower", "bluet", "rose", "lilac", "peony", "sunflower",
    "mushroom", "deadbush", "dead_bush", "sugar_cane", "reeds", "bamboo_sapling",
    "wheat", "carrots", "potatoes", "beetroot", "melon_stem", "pumpkin_stem",
    "nether_wart", "sweet_berry", "cave_vines", "glow_lichen", "moss_carpet",
    "hanging_roots", "spore_blossom", "pink_petals", "leaf_litter", "bush",
    "carpet", "torch", "redstone_wire", "tripwire",
    "rail", "lever", "button", "pressure_plate", "banner", "sign_post", "sign",
    "web", "string", "snow_layer", "top_snow", "short_grass", "tall_grass",
    "fire", "portal", "end_gateway", "structure_void",
    "item_frame", "painting", "flower_pot", "skull", "candle", "amethyst_bud",
    "sculk_vein", "coral", "sea_pickle", "turtle_egg", "frogspawn",
};

const char* const kWaterNames[] = { "water", "bubble_column" };
const char* const kLavaNames[] = { "lava" };
const char* const kHazardNames[] = {
    "cactus", "sweet_berry_bush", "wither_rose", "campfire", "soul_campfire",
    "magma", "pointed_dripstone", "fire", "soul_fire", "powder_snow",
};

bool nameHasAny(const char* n, const char* const* table, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (std::strstr(n, table[i]) != nullptr) return true;
    return false;
}

enum class Kind : unsigned char {
    Unknown, Clear, Solid, Water, Lava, Hazard,
};

Kind classifyName(const char* n) {
    if (!n || !n[0]) return Kind::Unknown;
    if (nameHasAny(n, kSolidExceptions, sizeof(kSolidExceptions) / sizeof(*kSolidExceptions)))
        return Kind::Solid;
    if (nameHasAny(n, kLavaNames, sizeof(kLavaNames) / sizeof(*kLavaNames)))
        return Kind::Lava;
    if (nameHasAny(n, kHazardNames, sizeof(kHazardNames) / sizeof(*kHazardNames)))
        return Kind::Hazard;
    if (nameHasAny(n, kWaterNames, sizeof(kWaterNames) / sizeof(*kWaterNames)))
        return Kind::Water;
    if (nameHasAny(n, kClearNames, sizeof(kClearNames) / sizeof(*kClearNames)))
        return Kind::Clear;
    return Kind::Solid;
}

// POD + SEH: reads the block name into a fixed buffer without constructing
// std::string inside the guarded region (C2712).
bool sehBlockName(SDK::BlockSource* bs, const BlockPos& pos, char* buf, size_t cap) {
    if (!bs || !buf || cap == 0) return false;
    __try {
        SDK::Block* b = bs->getBlock(pos);
        if (!b) return false;
        SDK::BlockLegacy* l = b->getLegacy();
        if (!l) return false;
        return SDK::sehCopyBlockName(l, buf, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

BlockState classifyAt(SDK::BlockSource* bs, const BlockPos& pos) {
    char buf[96] = { 0 };
    BlockState out;
    if (!sehBlockName(bs, pos, buf, sizeof(buf)))
        return out;                                     // unloaded / unreadable

    switch (classifyName(buf)) {
    case Kind::Clear:  out.loaded = true; break;
    case Kind::Solid:  out.loaded = true; out.solid = true; break;
    case Kind::Water:  out.loaded = true; out.liquid = true; break;
    case Kind::Lava:   out.loaded = true; out.liquid = true; out.hazard = true; break;
    case Kind::Hazard: out.loaded = true; out.hazard = true; break;
    default: break;
    }
    return out;
}

} // namespace

BlockState BlockSourceWorld::getBlock(const BlockPos& pos) const {
    if (const auto found = cache.find(pos); found != cache.end())
        return found->second;
    const auto state = classifyAt(source, pos);
    if (state.loaded)
        cache.emplace(pos, state);
    return state;
}

BlockState BlockSourceWorld::getBlockNow(const BlockPos& pos) const {
    return classifyAt(source, pos);
}

} // namespace baritone
