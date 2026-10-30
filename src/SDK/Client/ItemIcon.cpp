#include "ItemIcon.h"
#include "ClientInstance.h"
#include "../Types.h"
#include <Windows.h>
#include <cstring>
#include <string>
#include <algorithm>
#include <cctype>
#include <imgui.h>

extern uintptr_t g_ItemStackBase_ctorFromBlock;
extern uintptr_t g_ItemStackBase_dtor;
extern uintptr_t g_ItemStack_vtable;
extern uintptr_t g_BaseActorRenderContext_ctor;
extern uintptr_t g_BaseActorRenderContext_dtor;
extern uintptr_t g_ItemRenderer_renderGuiItemNew;
extern uintptr_t g_ItemRenderer_flushGuiItem;

namespace SDK {

// MinecraftUIRenderContext layout (vptr first, Latite 1.26.4x):
// +0x00 vtable, +0x08 ClientInstance*, +0x10 ScreenContext*.
static constexpr uintptr_t kUircClientInstance = 0x08;
static constexpr uintptr_t kUircScreenContext  = 0x10;

// BaseActorRenderContext: itemRenderer lives at +0x58. Exact object size is
// unpublished, so over-allocate generously — the ctor writes only within
// its own object.
static constexpr uintptr_t kBarcItemRenderer = 0x58;
static constexpr size_t    kBarcSize         = 0x600;

// ItemStack is 0x98 bytes. showPickUp is the byte at +0x24 (the game clears it
// right before rendering so the icon does not animate as a pickup).
static constexpr size_t    kItemStackSize = 0x98;
static constexpr uintptr_t kStackShowPickUp = 0x24;

// renderGuiItemNew's arg 10 is multiplied by 16.0f to get the on-screen size.
static constexpr float kIconBaseSize = 16.0f;

bool itemIconAvailable() {
    return g_ItemStack_vtable && g_ItemStackBase_ctorFromBlock && g_ItemStackBase_dtor &&
           g_BaseActorRenderContext_ctor && g_BaseActorRenderContext_dtor &&
           g_ItemRenderer_renderGuiItemNew && g_ItemRenderer_flushGuiItem;
}

// POD-only worker so __try is legal (C2712) and no C++ object can be left
// half-destroyed if the game throws mid-render.
static bool sehDrawBlockIcon(void* mcuirc, void* screenContext, void* clientInstance,
                             void* mcGame, void* block, float x, float y, float scale) {
    __try {
        // Both game objects are raw byte buffers we own; the game's ctors fill them.
        alignas(16) unsigned char ctxBuf[kBarcSize];
        alignas(16) unsigned char stackBuf[kItemStackSize];
        memset(ctxBuf, 0, sizeof(ctxBuf));
        memset(stackBuf, 0, sizeof(stackBuf));

        using barcCtor_t = void*(__fastcall*)(void*, void*, void*, void*);
        ((barcCtor_t)g_BaseActorRenderContext_ctor)(ctxBuf, screenContext, clientInstance, mcGame);

        void* itemRenderer = *(void**)(ctxBuf + kBarcItemRenderer);
        if (!itemRenderer || !sdk_safe_ptr(itemRenderer, sizeof(void*))) {
            using barcDtor_t = void*(__fastcall*)(void*);
            ((barcDtor_t)g_BaseActorRenderContext_dtor)(ctxBuf);
            return false;
        }

        // ItemStackBase::ItemStackBase(Block const&, int count, CompoundTag const*)
        using stackCtor_t = void*(__fastcall*)(void*, void*, int, void*);
        ((stackCtor_t)g_ItemStackBase_ctorFromBlock)(stackBuf, block, 1, nullptr);
        *(uintptr_t*)stackBuf = g_ItemStack_vtable;
        *reinterpret_cast<int32_t*>(stackBuf + 0x80) = 0;
        stackBuf[0x90] = 0;
        stackBuf[kStackShowPickUp] = 0;
        void* itemRef = *(void**)(stackBuf + 0x08);
        if (stackBuf[0x22] == 0 || stackBuf[0x23] == 0 ||
            !itemRef || !sdk_safe_ptr(itemRef, sizeof(void*)) || !*(void**)itemRef) {
            ((void(__fastcall*)(void*))g_ItemStackBase_dtor)(stackBuf);
            ((void(__fastcall*)(void*))g_BaseActorRenderContext_dtor)(ctxBuf);
            return false;
        }

        using render_t = void(__fastcall*)(void*, void*, void*, int, float, float,
                                           bool, float, float, float, int);
        ((render_t)g_ItemRenderer_renderGuiItemNew)(
            itemRenderer, ctxBuf, stackBuf,
            /*iconIndex*/ 0, x, y,
            /*enchantGlint*/ false,
            /*arg8*/ 1.0f, /*arg9*/ 1.0f,
            /*scale*/ scale,
            /*arg11*/ 0);

        // MANDATORY: renderGuiItemNew only queues geometry into the tessellator;
        // the game flushes after every call. Without this the batch is never
        // submitted and grows every frame until the game dies. Flag byte 0
        // matches a plain icon draw.
        unsigned char renderFlags = 0;
        using flush_t = void*(__fastcall*)(void*, void*, unsigned char*);
        ((flush_t)g_ItemRenderer_flushGuiItem)(mcuirc, ctxBuf, &renderFlags);

        using stackDtor_t = void(__fastcall*)(void*);
        ((stackDtor_t)g_ItemStackBase_dtor)(stackBuf);
        using barcDtor_t = void*(__fastcall*)(void*);
        ((barcDtor_t)g_BaseActorRenderContext_dtor)(ctxBuf);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool drawBlockIcon(void* mcuirc, void* block, float x, float y, float pixels) {
    if (!itemIconAvailable()) return false;
    if (!block || !sdk_safe_ptr(block, sizeof(void*))) return false;
    if (!mcuirc || !sdk_safe_ptr(mcuirc, kUircScreenContext + sizeof(void*))) return false;
    if (!(pixels > 0.5f) || pixels > 512.f) return false;

    void* clientInstance = *(void**)((uintptr_t)mcuirc + kUircClientInstance);
    void* screenContext  = *(void**)((uintptr_t)mcuirc + kUircScreenContext);
    if (!clientInstance || !sdk_safe_ptr(clientInstance, sizeof(void*))) return false;
    if (!screenContext  || !sdk_safe_ptr(screenContext,  sizeof(void*))) return false;

    // Layout self-check: if +0x08 really is the ClientInstance then it must be
    // the one we resolve independently. A mismatch means the render-context
    // layout drifted, and we bail instead of feeding garbage to the game.
    ClientInstance* known = ClientInstance::get();
    if (!known || (void*)known != clientInstance) return false;
    Actor* player = known->getLocalPlayer();
    Minecraft* mc = known->getMinecraft();
    if (!player || !sdk_safe_ptr(player, Layout::Probe::Actor) ||
        !mc || !sdk_safe_ptr(mc, Layout::Probe::Minecraft) || !mc->getLevel()) return false;

    MinecraftGame* mcGame = known->getMinecraftGame();
    if (!mcGame || !sdk_safe_ptr(mcGame, sizeof(void*))) return false;

    // COORDINATE SPACE: renderGuiItemNew works in the game's UI units (the
    // game's callers pass pos * guiScaleFrac), our HUD positions are ImGui
    // pixels — convert here so callers can keep thinking in pixels.
    GuiData* gd = known->getGuiData();
    float frac = gd ? gd->getGuiScaleFrac() : 0.f;
    if (frac <= 0.f) {
        // Fall back to 1/guiScale, then to 1:1, rather than drawing in the wrong
        // place: a wrong-but-visible icon is worse than an unscaled one.
        const float s = gd ? gd->getGuiScale() : 0.f;
        frac = (s > 0.f) ? (1.f / s) : 1.f;
    }

    return sehDrawBlockIcon(mcuirc, screenContext, clientInstance, mcGame, block,
                            x * frac, y * frac, (pixels * frac) / kIconBaseSize);
}

static ImU32 packFace(float r, float g, float b, float a) {
    return IM_COL32((int)(r * 255.f + 0.5f), (int)(g * 255.f + 0.5f),
                    (int)(b * 255.f + 0.5f), (int)(a * 255.f + 0.5f));
}

static void tintOf(const char* name, float& r, float& g, float& b) {
    r = 0.55f; g = 0.58f; b = 0.62f;
    if (!name || !*name) return;
    std::string n(name);
    for (char& c : n) c = (char)tolower((unsigned char)c);
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    if (has("emerald"))      { r = 0.18f; g = 0.78f; b = 0.38f; return; }
    if (has("diamond"))      { r = 0.30f; g = 0.82f; b = 0.86f; return; }
    if (has("gold"))         { r = 0.95f; g = 0.80f; b = 0.22f; return; }
    if (has("iron"))         { r = 0.82f; g = 0.78f; b = 0.72f; return; }
    if (has("coal"))         { r = 0.22f; g = 0.22f; b = 0.24f; return; }
    if (has("redstone"))     { r = 0.86f; g = 0.16f; b = 0.14f; return; }
    if (has("lapis"))        { r = 0.18f; g = 0.32f; b = 0.78f; return; }
    if (has("debris") || has("netherite")) { r = 0.38f; g = 0.26f; b = 0.22f; return; }
    if (has("obsidian"))     { r = 0.16f; g = 0.08f; b = 0.28f; return; }
    if (has("glowstone"))    { r = 0.95f; g = 0.78f; b = 0.32f; return; }
    if (has("netherrack"))   { r = 0.62f; g = 0.22f; b = 0.20f; return; }
    if (has("end_stone"))    { r = 0.86f; g = 0.84f; b = 0.58f; return; }
    if (has("sand"))         { r = 0.86f; g = 0.80f; b = 0.52f; return; }
    if (has("gravel"))       { r = 0.55f; g = 0.53f; b = 0.50f; return; }
    if (has("dirt") || has("grass")) { r = 0.48f; g = 0.34f; b = 0.20f; return; }
    if (has("stone") || has("deepslate") || has("cobble")) { r = 0.48f; g = 0.48f; b = 0.50f; return; }
    if (has("plank") || has("log") || has("wood") || has("oak") || has("spruce")
        || has("birch") || has("jungle") || has("acacia") || has("mangrove")) {
        r = 0.70f; g = 0.52f; b = 0.28f; return;
    }
    if (has("wool") || has("concrete") || has("terracotta")) { r = 0.78f; g = 0.78f; b = 0.80f; return; }
    if (has("glass"))        { r = 0.70f; g = 0.86f; b = 0.92f; return; }
    if (has("obsidian"))     { r = 0.16f; g = 0.08f; b = 0.28f; return; }
    // Stable fallback from the name so unknown blocks still differ.
    unsigned h = 2166136261u;
    for (unsigned char c : n) { h ^= c; h *= 16777619u; }
    r = 0.28f + ((h        & 255) / 255.f) * 0.55f;
    g = 0.28f + (((h >> 8) & 255) / 255.f) * 0.55f;
    b = 0.28f + (((h >> 16)& 255) / 255.f) * 0.55f;
}

void drawBlockIconImGui(void* drawList, float x, float y, float pixels, const char* blockName, float alpha) {
    if (!drawList || !(pixels > 2.f) || alpha <= 0.01f) return;
    ImDrawList* dl = (ImDrawList*)drawList;
    float r, g, b;
    tintOf(blockName, r, g, b);
    alpha = std::clamp(alpha, 0.f, 1.f);

    const float s = pixels;
    const float cx = x + s * 0.50f;
    const float cy = y + s * 0.54f;
    const float hx = s * 0.42f;
    const float hy = s * 0.24f;
    const float hh = s * 0.42f;

    const ImVec2 topN(cx,          cy - hh);
    const ImVec2 topE(cx + hx,     cy - hh + hy);
    const ImVec2 topS(cx,          cy - hh + hy * 2.f);
    const ImVec2 topW(cx - hx,     cy - hh + hy);
    const ImVec2 botE(cx + hx,     cy + hy);
    const ImVec2 botS(cx,          cy + hy * 2.f);
    const ImVec2 botW(cx - hx,     cy + hy);

    dl->AddQuadFilled(topN, topE, topS, topW, packFace(r * 1.18f, g * 1.18f, b * 1.18f, alpha));
    dl->AddQuadFilled(topW, topS, botS, botW, packFace(r * 0.72f, g * 0.72f, b * 0.72f, alpha));
    dl->AddQuadFilled(topS, topE, botE, botS, packFace(r * 0.88f, g * 0.88f, b * 0.88f, alpha));
    const ImU32 edge = packFace(r * 0.35f, g * 0.35f, b * 0.35f, alpha * 0.85f);
    dl->AddLine(topN, topE, edge, 1.1f);
    dl->AddLine(topE, topS, edge, 1.1f);
    dl->AddLine(topS, topW, edge, 1.1f);
    dl->AddLine(topW, topN, edge, 1.1f);
    dl->AddLine(topW, botW, edge, 1.1f);
    dl->AddLine(topS, botS, edge, 1.1f);
    dl->AddLine(topE, botE, edge, 1.1f);
    dl->AddLine(botW, botS, edge, 1.1f);
    dl->AddLine(botS, botE, edge, 1.1f);
}

} // namespace SDK
