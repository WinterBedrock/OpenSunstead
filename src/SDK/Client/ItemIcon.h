#pragma once
#include <cstdint>

// Item / block icon rendering through Minecraft's own ItemRenderer — the same
// path as inventory slots, so a block comes out as the proper 3D-looking icon
// instead of a flat atlas crop. Reversed from the game's call sequence on
// 1.26.42.01:
//   v27 = ClientInstance::vtable[79](clientInstance)          ; MinecraftGame*
//   BaseActorRenderContext_ctor(ctx, screenContext, clientInstance, v27)
//   *(BYTE*)(stack + 0x24) = 0                                ; showPickUp = false
//   renderGuiItemNew(itemRenderer, ctx, stack, 0, x, y, ench, 1.0f, 1.0f, scale, 0)
//   BaseActorRenderContext_dtor(ctx)                          ; releases refcounts
// Arg types from the callee's reads: movss for args 5,6,8,9,10; movzx byte for
// arg 7; mov dword for arg 11. Rendered size = scale * 16 px (arg 10 * 16.0f).
// MUST be called on the game's UI render path (the ScreenView::setupAndRender
// hook's MinecraftUIRenderContext) — there is no valid ScreenContext or render
// state in the ImGui/Present hook.

namespace SDK {

// True when every function this needs was resolved.
bool itemIconAvailable();

// Draws `block`'s icon with top-left at (x, y) in game UI pixels, `pixels`
// wide/tall. `mcuirc` is the ScreenView::setupAndRender hook's
// MinecraftUIRenderContext. Returns false if anything was unavailable or failed
// validation; never throws and never faults.
bool drawBlockIcon(void* mcuirc, void* block, float x, float y, float pixels);

// Our own overlay icon — three-face isometric cube, hashed from the block
// name so every ore/block has a stable colour without the game's ItemRenderer.
// Drawn on the ImGui list (Present hook), so it never depends on ScreenContext.
void drawBlockIconImGui(void* drawList, float x, float y, float pixels,
                        const char* blockName, float alpha = 1.0f);

} // namespace SDK
