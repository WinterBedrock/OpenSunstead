#pragma once

#include "../Types.h"
#include "../Actor/Actor.h"
#include "../Client/ClientInstance.h"
#include <cstdint>
#include <cmath>
#include <string>

namespace SDK {

struct BlockPos {
    int x = 0;
    int y = 0;
    int z = 0;

    BlockPos() = default;
    BlockPos(int x_, int y_, int z_) : x(x_), y(y_), z(z_) {}
    explicit BlockPos(const Vec3& v)
        : x((int)std::floor(v.x)), y((int)std::floor(v.y)), z((int)std::floor(v.z)) {}

    bool operator==(const BlockPos& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const BlockPos& o) const { return !(*this == o); }
    BlockPos offset(int dx, int dy, int dz) const { return BlockPos(x + dx, y + dy, z + dz); }
};

struct BlockPosHash {
    size_t operator()(const BlockPos& p) const noexcept {
        uint64_t x = (uint32_t)p.x, y = (uint32_t)p.y, z = (uint32_t)p.z;
        return (size_t)(x * 0x9E3779B185EBCA87ull ^ y * 0xC2B2AE3D27D4EB4Full ^ z * 0x165667B19E3779F9ull);
    }
};

class BlockLegacy {
public:
    std::string getName() const {
        std::string out;
        // 1.26.60.05 (IDA-derived): the FULL registered id ("minecraft:diamond_ore")
        // lives in the HashedString mName @+0xE0 — FNV hash@+0xE0, string
        // buf@+0xE8, size@+0xF8, cap@+0x100. Natives test air by comparing
        // *(u64*)(legacy+0xE0) with the baked FNV("minecraft:air") constant
        // 0xBD584BAF00003448, so this is the authoritative name field. The
        // 1.26.30-era +0x90/+0x98 short-name pair is stale and reads garbage
        // on this build.
        if (sdk_safe_string((void*)((uintptr_t)this + 0xE8), out) && !out.empty()) return out;
        return {};
    }

    uint16_t getBlockId() const {
        __try {
            // 1.26.60.05 (IDA-derived): UNCHANGED from 1.26.30 — the base ctor
            // sub_1435A7570 still stores the int16 id arg at *(WORD*)(this+0x17E).
            // Name matching remains primary.
            if (!sdk_safe_ptr((void*)((uintptr_t)this + 0x17E), sizeof(uint16_t))) return 0xFFFF;
            uint16_t id = *(uint16_t*)((uintptr_t)this + 0x17E);
            if (id < 10000) return id;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0xFFFF;
    }

    bool isAir() const {
        std::string n = getName();
        if (!n.empty()) return n == "air" || n == "minecraft:air" || n.find(":air") != std::string::npos;
        return getBlockId() == 0;
    }

    bool isSolid() const {
        __try {
            // 1.26.60.05 (IDA-derived): the old bool at +0x19C is DEAD (the
            // ctor zeroes it). The natives' solid gate is exactly
            // BlockSource_A vtable slot 53 (sub_1432D1C00):
            //   (legacy[0x29] & 5) == 4   AND   (qword@+0x130 & 0x40000)
            // — air fails the bit test.
            if (sdk_safe_ptr((void*)((uintptr_t)this + 0x130), sizeof(uint64_t)) &&
                sdk_safe_ptr((void*)((uintptr_t)this + 0x29), 1)) {
                if ((*(uint8_t*)((uintptr_t)this + 0x29) & 5) == 4 &&
                    (*(uint64_t*)((uintptr_t)this + 0x130) & 0x40000) != 0)
                    return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return !isAir();
    }
};

// Copy a BlockLegacy's registered name into a caller-owned char buffer — POD
// in/out so SEH-guarded callers avoid constructing a std::string in scope
// (MSVC C2712).
inline bool sehCopyBlockName(BlockLegacy* legacy, char* buf, size_t cap) {
    if (!legacy || !buf || cap == 0) return false;
    size_t len = 0;
    // Same field as getName(): the mName HashedString's std::string at +0xE8.
    if (!sdk_copy_cstr((void*)((uintptr_t)legacy + 0xE8), buf, cap, &len)) return false;
    return len > 0;
}

class Block {
public:
    BlockLegacy* getLegacy() const {
        // 1.26.60.05 (IDA re-verified): Block::mLegacy at +0x68 (confirmed by
        // the mesher 0x791B91D and sub_14D1ED640). Unchanged from 1.26.30.
        return readLegacyAt(0x68);
    }

    uint32_t getRuntimeId() const {
        __try {
            if (sdk_safe_ptr((void*)((uintptr_t)this + 0xC0), sizeof(uint32_t)))
                return *(uint32_t*)((uintptr_t)this + 0xC0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

private:
    BlockLegacy* readLegacyAt(uintptr_t off) const {
        BlockLegacy* legacy = readLegacyPtrAt(off);
        // 0x120 covers BlockLegacy's mName HashedString (+0xE0) and its
        // std::string (+0xE8..+0x108) on 1.26.60.05 (sizeof(BlockLegacy)=0x378).
        if (!legacy || !sdk_safe_ptr(legacy, 0x120)) return nullptr;
        return legacy->getName().empty() ? nullptr : legacy;
    }

    BlockLegacy* readLegacyPtrAt(uintptr_t off) const {
        __try {
            if (!sdk_safe_ptr((void*)((uintptr_t)this + off), sizeof(void*))) return nullptr;
            return *(BlockLegacy**)((uintptr_t)this + off);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }
};

class BlockSource {
public:
    bool isValid() const {
        if (!sdk_safe_ptr((void*)this, sizeof(void*))) return false;
        void** vt = *(void***)this;
        return vt && sdk_safe_ptr(vt, sizeof(void*) * 4) && vt[1] && vt[2] && vt[3] &&
               sdk_safe_ptr(vt[1], 16) && sdk_safe_ptr(vt[2], 16) && sdk_safe_ptr(vt[3], 16);
    }

    Block* getBlock(const BlockPos& pos) {
        if (!isValid()) return nullptr;
        void** vt = *(void***)this;
        // 1.26.60.05: vt[2] IS getBlock(this, BlockPos const&). Slot 3 is a
        // position-keyed metadata map probe (not a Block) and 2-arg vt[1] is
        // a layer-mangled forwarder.
        if (Block* b = callByRef(vt[2], pos)) return b;
        return nullptr;
    }

    Block* getBlock(int x, int y, int z) { return getBlock(BlockPos(x, y, z)); }

    bool isAir(const BlockPos& pos) {
        Block* b = getBlock(pos);
        BlockLegacy* l = b ? b->getLegacy() : nullptr;
        return !l || l->isAir();
    }

    bool isSolid(const BlockPos& pos) {
        Block* b = getBlock(pos);
        BlockLegacy* l = b ? b->getLegacy() : nullptr;
        return l && l->isSolid();
    }

private:
    static bool usableBlock(Block* b) {
        if (!b || !sdk_safe_ptr(b, sizeof(void*))) return false;
        return b->getLegacy() != nullptr;
    }

    Block* callByRef(void* fn, const BlockPos& pos) const {
        if (!fn || !sdk_safe_ptr(fn, 16)) return nullptr;
        __try {
            using fn_t = Block*(__fastcall*)(const BlockSource*, const BlockPos&);
            Block* b = ((fn_t)fn)(this, pos);
            return usableBlock(b) ? b : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    Block* callByRefWithLayer(void* fn, const BlockPos& pos, uint32_t layer) const {
        if (!fn || !sdk_safe_ptr(fn, 16)) return nullptr;
        __try {
            using fn_t = Block*(__fastcall*)(const BlockSource*, const BlockPos&, uint32_t);
            Block* b = ((fn_t)fn)(this, pos, layer);
            return usableBlock(b) ? b : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    Block* callXYZ(void* fn, const BlockPos& pos) const {
        if (!fn || !sdk_safe_ptr(fn, 16)) return nullptr;
        __try {
            using fn_t = Block*(__fastcall*)(const BlockSource*, int, int, int);
            Block* b = ((fn_t)fn)(this, pos.x, pos.y, pos.z);
            return usableBlock(b) ? b : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

};

// 1.26.60.05: the CI vtable slot 30 BlockSource getter is gone (slot 30 is
// now getLocalPlayer). The long-lived client BlockSource lives on the
// Dimension (dim+0xF0 shared holder, BS_A object at +0x20). See the ADT in
// compatibility/blocksource-rakpeer2-1.26.60.json.
} // namespace SDK
// Defined in GameHooks.cpp (Dimension-rooted, see blocksource-rakpeer2 json).
SDK::BlockSource* GameHooks_GetClientBlockSource();
namespace SDK {
inline BlockSource* ClientInstance::getBlockSource() {
    return ::GameHooks_GetClientBlockSource();
}

} // namespace SDK
