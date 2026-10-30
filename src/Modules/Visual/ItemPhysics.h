#pragma once
#include "../Module.h"

// ---------------------------------------------------------------------------
// ItemPhysics — makes dropped items lie on the ground like Java Edition instead
// of standing upright in the air, spinning.
//
// Three independent pieces, each found in a different place. Where the spin came
// from is worth recording, because the first theory was wrong:
//
//   query.life_time — REJECTED. The initial assumption was that the spin was a
//   Molang expression in the vanilla render controller, animated against that
//   query, so the query getter was hooked to freeze its clock. The debug log
//   disproved it outright: the getter fired ~300 times a second with the freeze
//   applied and the items kept turning.
//
// It is all plain C++ in ItemActorRender::renderItem (0x141E7B620):
//
//   1. SPIN. The angle accumulates from the actor's age and is handed to a
//      matrix rotate about Y:
//
//        cvtsi2ss xmm7, [rsi+448h]   ; age in ticks
//        addss    xmm7, xmm6         ; + partial tick
//        mulss    xmm7, 0.05         ; -> seconds
//        ...                         ; rad->deg->rad round trip
//        movaps   xmm2, xmm7         ; the angle argument
//        call     Matrix_rotateAxisAngle
//
//      Patched at the `movaps` -> `xorps xmm2, xmm2` (identical three bytes), so
//      the angle is zero and the rotation is identity.
//
//   2. BOB. ItemActorRender::getBobHeight (0x141E7A2A0) is a real function:
//
//        base   = 0.2f unless the item is inside a block, then 0.0f
//        result = base + (sinTable[(age + partial) * 10430.378] * 0.1f + 0.1f)
//
//      The sine term is the bounce, swinging ±0.1 around that trailing +0.1, so
//      the resting midpoint is base + 0.1f. Hooked (a function boundary exists,
//      so no reason to poke bytes) and made to return that midpoint, minus the
//      Height offset when Lay Flat is on.
//
//   3. LAY FLAT. The upright orientation IS that same Y rotation — remove the
//      spin and the item just stands still. Tipping it over needs an extra
//      rotation about X, which is done by hooking Matrix_rotateAxisAngle and
//      substituting axis+angle for the ONE call the item spin makes. That call is
//      identified by return address, so every other user of this very hot,
//      very general helper is passed straight through.
//
// All three are render-only: no entity field is written, nothing reaches the
// server, and disabling restores vanilla exactly (the patched bytes included).
// ---------------------------------------------------------------------------
class ItemPhysics : public Module {
public:
    ItemPhysics()
        : Module("ItemPhysics", "Makes dropped items lie flat on the ground",
                 Category::Visual, 0) {
        AddSetting(new BoolSetting("No Spin",
            "Freeze the idle rotation of dropped items", true));
        AddSetting(new BoolSetting("No Bob",
            "Stop dropped items floating up and down", true));
        AddSetting(new BoolSetting("Lay Flat",
            "Tip items onto their side so they lie on the ground", true));
        AddSetting(new NumberSetting("Height",
            "How far to lower the item toward the ground", 0.22f, 0.0f, 0.4f, 0.01f));
        canBind = true;
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;
    void UpdateSettingsVisibility() override;

    // Reconcile the byte patch with the current module/setting state. Idempotent.
    static void Apply();

    // Read by the hooks in GameHooks. Static so neither needs a ModuleManager
    // lookup on the render path, which runs per item per frame.
    static bool  NoBobEnabled();
    static bool  LayFlatEnabled();
    static float HeightOffset();
    // Return address of the item-spin call into Matrix_rotateAxisAngle. The
    // Lay Flat hook compares against this so it only alters that one call.
    static uintptr_t SpinCallReturnAddr();
};
