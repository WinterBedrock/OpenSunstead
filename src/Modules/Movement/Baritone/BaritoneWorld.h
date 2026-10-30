#pragma once
#include "BaritoneCore.h"

namespace SDK { class BlockSource; }

namespace baritone {

// IWorld over SDK::BlockSource. Classification is name-based (Bedrock names are
// the stable signal across versions); a per-search cache lives in the
// implementation because the search asks about the same cells repeatedly.
class BlockSourceWorld final : public IWorld {
    SDK::BlockSource* source;
    mutable std::unordered_map<BlockPos, BlockState, SDK::BlockPosHash> cache;

public:
    explicit BlockSourceWorld(SDK::BlockSource* src) : source(src) {}

    // Uncached probe used by the executor, which must see the world as it is
    // right now rather than as it was when the path was planned.
    [[nodiscard]] BlockState getBlock(const BlockPos& pos) const override;
    [[nodiscard]] BlockState getBlockNow(const BlockPos& pos) const;
};

} // namespace baritone
