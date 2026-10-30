#pragma once
#include "../Types.h"
#include "../Actor/Actor.h"
#include <cstdint>

namespace SDK {

// 1.26.50.04 layout, verified against ClientInputUpdateSystem::tick
// (sub_146697260) and the entt pool stride (100 bytes, getOrEmplace
// sub_14267A330). Bit positions match the game's own rebuild loop.
struct MoveInputState {
    bool sneakDown : 1;                // 0
    bool sneakToggleDown : 1;          // 1
    bool wantDownSlow : 1;             // 2
    bool wantUpSlow : 1;               // 3
    bool blockSelectDown : 1;          // 4
    bool ascendBlock : 1;              // 5
    bool descendBlock : 1;             // 6
    bool jumpDown : 1;                  // 7
    bool sprintDown : 1;               // 8
    bool upLeft : 1;                    // 9
    bool upRight : 1;                   // 10
    bool downLeft : 1;                  // 11
    bool downRight : 1;                 // 12
    bool up : 1;                        // 13
    bool down : 1;                      // 14
    bool left : 1;                      // 15
    bool right : 1;                     // 16
    bool ascend : 1;                    // 17
    bool descend : 1;                   // 18
    bool changeHeight : 1;              // 19
    bool lookCenter : 1;                // 20
    bool sneakInputCurrentlyDown : 1;  // 21
    bool sneakInputWasReleased : 1;    // 22
    bool sneakInputWasPressed : 1;     // 23
    bool jumpInputWasReleased : 1;     // 24
    bool jumpInputWasPressed : 1;      // 25
    bool jumpInputCurrentlyDown : 1;   // 26

    Vec2 analogMoveVector;             // +0x04
    uint8_t lookSlightDirField = 0;    // +0x0C
    uint8_t lookNormalDirField = 0;    // +0x0D
    uint8_t lookSmoothDirField = 0;     // +0x0E
    uint8_t _pad = 0;                   // +0x0F
};
static_assert(sizeof(MoveInputState) == 0x10);

// Tail bitfield at +0x60. The tick derives these from the inputState bits
// (sprinting from sprintDown, jumping gated by holdAutoJumpInWaterTicks).
enum MoveInputTailBits : uint32_t {
    TailSneaking         = 1u << 0,
    TailSprinting        = 1u << 1,
    TailWantUp           = 1u << 2,
    TailWantDown         = 1u << 3,
    TailJumping          = 1u << 4,
    TailAutoJumpInWater  = 1u << 5,
    TailMoveLocked       = 1u << 6,
    TailPersistSneak     = 1u << 7,
    TailAutoJumpEnabled  = 1u << 8,
};

struct MoveInputComponent {
    MoveInputState inputState;          // +0x00
    MoveInputState rawInputState;       // +0x10
    uint8_t holdAutoJumpInWaterTicks = 0; // +0x20
    Vec2 move;                           // +0x24
    Vec2 lookDelta;                      // +0x2C
    Vec2 interactDirection;              // +0x34
    Vec3 displacement;                   // +0x3C
    Vec3 displacementDelta;              // +0x48
    Vec3 cameraOrientation;              // +0x54
    uint32_t tail = 0;                   // +0x60
};
static_assert(sizeof(MoveInputComponent) == 0x64);

// getOrEmplace(EntityContext*) — resolved by signature into
// g_MoveInputComponent_get. SEH-guarded; null when unreachable.
inline MoveInputComponent* getMoveInput(Actor* a) {
    if (!a || !g_MoveInputComponent_get) return nullptr;
    __try {
        auto* ctx = (EntityContext*)((uintptr_t)a + 0x8);
        if (!ctx || !sdk_safe_ptr(ctx, sizeof(EntityContext))) return nullptr;
        using fn_t = MoveInputComponent * (__fastcall*)(void*);
        void* p = ((fn_t)g_MoveInputComponent_get)(ctx);
        if (!p || !sdk_safe_ptr(p, sizeof(MoveInputComponent))) return nullptr;
        return (MoveInputComponent*)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

} // namespace SDK
