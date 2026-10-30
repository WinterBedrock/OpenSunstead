#pragma once
#include "../Types.h"
#include "../../Memory/OffsetManager.h"
#include <memory>
#include <string>
#include <functional>
#include <vector>
#include <cstring>

extern uintptr_t g_RenderMaterialGroupAddr;

// Render-pipeline function addresses resolved by SigManager.
extern uintptr_t g_Tessellator_begin;
extern uintptr_t g_Tessellator_vertex;
extern uintptr_t g_Tessellator_color;
extern uintptr_t g_MeshHelpers_renderMeshImmediately;

namespace SDK {

class ScreenController{public:virtual ~ScreenController()=default;};

// UIControl layout (Latite 1.26.30 / verified against getScreenName path):
//   +0x10 Vec2 position, +0x18 u64 flags (bit0 = position dirty),
//   +0x20 string name, +0x40 Vec2 parentRelativePos, +0x48 Vec2 size,
//   +0x98 vector<shared_ptr<UIControl>> children.
// VisualTree: rootControl @ +0x08.
// ScreenView: controller @ +0x38, visualTree @ +0x48.
class UIControl;
class VisualTree {
public:
    UIControl* getRootControl() {
        return *reinterpret_cast<UIControl**>(reinterpret_cast<uintptr_t>(this) + Offsets().VT_RootControl);
    }
};

class UIControl {
public:
    // Field offsets live in OffsetManager (UI_*). The layout below is the
    // Latite 1.26.30 baseline; 1.26.50 moved several of these.

    std::string* getName() {
        return reinterpret_cast<std::string*>(reinterpret_cast<uintptr_t>(this) + Offsets().UI_Name);
    }

    Vec2* getPosition() {
        return reinterpret_cast<Vec2*>(reinterpret_cast<uintptr_t>(this) + Offsets().UI_Position);
    }
    Vec2* getParentRelativePosition() {
        return reinterpret_cast<Vec2*>(reinterpret_cast<uintptr_t>(this) + Offsets().UI_RelPos);
    }
    Vec2* getSize() {
        return reinterpret_cast<Vec2*>(reinterpret_cast<uintptr_t>(this) + Offsets().UI_Size);
    }

    // children: std::vector<std::shared_ptr<UIControl>> — three pointers.
    // shared_ptr is 16 bytes (object*, control_block*).
    void forEachChildRaw(void (*fn)(UIControl*, void*), void* ctx);
    void forEachDescendantRaw(void (*fn)(UIControl*, void*), void* ctx);
    // Depth-first name search (SEH-safe).
    UIControl* findDescendant(const std::string& want);

    // Latite UIControl::updatePos (1.26.30 offsets): recompute the ABSOLUTE
    // position from the parent's absolute position + our parent-relative
    // position, fixing the parent chain first when its dirty bit is set, then
    // CLEAR our dirty bit so the game's own layout pass does not re-run (and
    // undo) the value. Returns false on any guarded read failure.
    bool updatePos();
    // updatePos for the whole descendant tree (Latite MovableChat calls
    // control->updatePos() on every descendant after shifting the panel).
    void updateDescendantsPos();
    // Shift the panel off-screen (hide=true) or back (hide=false), idempotently
    // like Latite: +20000 only while below, -20000 only while above. Returns
    // true when the shift was applied this call.
    bool shiftOffscreen(bool hide);

    // Offset parent-relative Y by dy and keep absolute position in sync.
    // Marks the dirty bit so layout will recompute when the game asks.
    void offsetY(float dy);
};

inline UIControl* findDescendantByName(UIControl* root, const std::string& want) {
    return root ? root->findDescendant(want) : nullptr;
}

class ScreenView {
public:
    ScreenController* getScreenController() {
        return *reinterpret_cast<ScreenController**>(reinterpret_cast<uintptr_t>(this) + Offsets().SV_ScreenController);
    }
    VisualTree* getVisualTree() {
        return *reinterpret_cast<VisualTree**>(reinterpret_cast<uintptr_t>(this) + Offsets().SV_VisualTree);
    }
    std::string getScreenName();

    static std::string& currentScreenName() { static std::string s; return s; }
    static ScreenController*& currentScreenController() {
        static ScreenController* c = nullptr; return c;
    }
};

// Set every frame by Chat::OnRender (Chat.cpp): true while the CUSTOM chat
// input is open. It is a hand-drawn overlay, so currentScreenName() stays
// "hud_screen" the whole time — the screen name alone can never tell that
// the user is typing.
extern bool g_chatInputOpen;

// The one gate every gameplay-key reader must use: the vanilla HUD screen is
// active AND the custom chat input is not capturing the keyboard. Reading raw
// keys without this leaks key state into chat typing.
inline bool isGameplayInputActive() {
    return !g_chatInputOpen && ScreenView::currentScreenName() == "hud_screen";
}

// Bedrock HashedString: { uint64 hash; std::string str; void* lastCompare; }
// — layout confirmed by literal-building call sites (e.g. the nametag material
// set-up at 0x1425FA7C0), which write the hash at +0x00, the SSO buffer at
// +0x08, the length at +0x18, the capacity at +0x20 and zero at +0x28.
// The hash is FNV-1, NOT FNV-1a (multiply first, then xor). RenderMaterialGroup
// lookups are BY HASH, so a wrong hash simply misses. Verified against eight
// literals the binary embeds next to their strings:
//   entity_alphatest             0x0A4F54B100E83415
//   entity_alphatest_glint_item  0xE0458F6E31EB2B8E
//   selection_overlay            0xB1136C9563344E6C
//   name_tag                     0x9D4D4085A61F9885
//   name_tag_with_backface       0x1F78EBD14DD47B59
//   name_tag_text                0x371EAB69E17AD46D
//   name_tag_text_with_backface  0x1DF15582CD941011
//   name_tag_depth_tested        0xC2FAD6537C4A4E6D
// FNV-1a reproduces none of them; FNV-1 reproduces all eight.
class HashedString {
public:
    uint64_t hash;
    std::string str;
    void* lastCompare;
    HashedString(const std::string& s) : str(s) {
        uint64_t h = 0xCBF29CE484222325uLL;
        for (char c : s) { h *= 0x100000001B3uLL; h ^= (unsigned char)c; }
        hash = h;
        lastCompare = nullptr;
    }
};

// Tessellator primitive modes, read off the vertex-count divisor table inside
// Tessellator::begin (0x141F413C0): mode 1 reserves in groups of 4, mode 2 in
// groups of 3, mode 4 in groups of 2. Anything else falls through the switch
// entirely — an unhandled mode byte produces no geometry.
enum class Primitive : int { Quad = 1, Triangle = 2, Line = 4 };

class Tessellator {
public:
    // Argument order confirmed against the game's own call at 0x14186FE15
    // (4-vertex quad): r8b = 1 (mode Quad), r9d = 4 (count), [rsp+0x20] byte
    // = 0 (reserve flag off — begin() skips the vertex pre-reserve and the
    // buffer grows as vertices arrive). rdx is left unset at that call site,
    // so nullptr is what the game effectively passes.
    void begin(Primitive fmt, int numVertices) {
        if (!g_Tessellator_begin) return;
        ((void(__fastcall*)(void*, void*, char, int, unsigned char))g_Tessellator_begin)(
            this, nullptr, (char)fmt, numVertices, (unsigned char)0);
    }
    void vertex(float x, float y, float z = 0.f) {
        if (!g_Tessellator_vertex) return;
        ((void(__fastcall*)(void*, float, float, float))g_Tessellator_vertex)(this, x, y, z);
    }
    void color(Color const* col) {
        if (!g_Tessellator_color || !col) return;
        color(col->r, col->g, col->b, col->a);
    }
    void color(float r = 1.f, float g = 1.f, float b = 1.f, float a = 1.f) {
        if (!g_Tessellator_color) return;
        ((void(__fastcall*)(void*, float, float, float, float))g_Tessellator_color)(this, r, g, b, a);
    }
};

class MaterialPtr {
public:
    std::shared_ptr<void> self;
    static MaterialPtr* getSelectionBoxMaterial();
    static MaterialPtr* getSelectionOverlayMaterial();
    // Materials that do NOT depth-test: the binary pairs `name_tag` with a
    // separate `name_tag_depth_tested` (name_text likewise) — a "_depth_tested"
    // variant only exists if the base material is not depth-tested. That is why
    // vanilla nameplates show through walls unless the depth-tested variant is
    // selected (`nameplateDepthTested` option). `_with_backface` additionally
    // skips backface culling, so a closed box drawn with it is solid from any
    // winding.
    static MaterialPtr* getNameTagMaterial();
    static MaterialPtr* getNameTagBackfaceMaterial();
    static MaterialPtr* getSelectionOverlayDoubleSidedMaterial();
    static MaterialPtr* getEntityAlphatestMaterial();
    static MaterialPtr* getUIColor();
    static MaterialPtr* getUITextureAndColor();
    static MaterialPtr* getUITextured();
    static MaterialPtr* getImGui();
    static MaterialPtr* createMaterial(const HashedString& name);
};

class ScreenContext {
public:
    Color* getShaderColor() {
        return *reinterpret_cast<Color**>(reinterpret_cast<uintptr_t>(this) + Offsets().VT_Color);
    }
    Tessellator* getTessellator() {
        return *reinterpret_cast<Tessellator**>(reinterpret_cast<uintptr_t>(this) + Offsets().VT_Tessellator);
    }
    inline static ScreenContext* instance3d = nullptr;
};

class MeshHelpers {
public:
    // The 4th argument is a caller-owned scratch/parameter block, and it must
    // cover every offset the callee touches. Call chain: RenderMeshImmediately
    // (0x145AC0C90) -> 0x14429C110, which reads a COUNT at block+0x140, walks
    // `40 * count` entries (switching on the byte at entry+0x20, dereferencing
    // entry+0x00/+0x10) and tests a discriminator byte at +0x40. Too small a
    // block puts the +0x140 count read past the buffer, so the loop iterates
    // over stack garbage. 0x200 zeroed bytes is correct at both reads: count 0
    // skips the walk, discriminator 0 takes the branch the game's own
    // "nothing attached" call sites take.
    static constexpr size_t kParamBlockSize = 0x200;

    static void renderMeshImmediately(ScreenContext* scn, Tessellator* tess, MaterialPtr* material) {
        if (!g_MeshHelpers_renderMeshImmediately || !scn || !tess || !material) return;
        alignas(16) unsigned char params[kParamBlockSize] = { 0 };
        ((void(__fastcall*)(ScreenContext*, Tessellator*, MaterialPtr*, void*))
            g_MeshHelpers_renderMeshImmediately)(scn, tess, material, params);
    }
};

// The caller owns the getter's shared_ptr return storage, not a borrowed pointer.
inline MaterialPtr* MaterialPtr::createMaterial(const HashedString& name) {
    if (!g_RenderMaterialGroupAddr) return nullptr;
    void* groupThis = reinterpret_cast<void*>(g_RenderMaterialGroupAddr);
    if (!sdk_safe_ptr(groupThis, sizeof(void*))) return nullptr;
    void** vtable = *reinterpret_cast<void***>(groupThis);
    if (!vtable || !sdk_safe_ptr(vtable, sizeof(void*) * 2)) return nullptr;
    using getMaterial_t = void(__fastcall*)(void*, MaterialPtr*, const HashedString*);
    auto fn = reinterpret_cast<getMaterial_t>(vtable[1]);
    if (!fn) return nullptr;

    auto* out = new MaterialPtr;
    bool ok = false;
    CrashHandler::EnterProbe();
    __try {
        fn(groupThis, out, &name);
        ok = out->self.get() != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    CrashHandler::LeaveProbe();
    if (!ok) { delete out; return nullptr; }
    return out;
}

// The material renderMeshImmediately is about to draw (its third argument),
// so a caller hooking that function can read which named material is drawn.
inline bool materialNameOf(void* material, char* buf, size_t cap) {
    (void)material;
    if (buf && cap) buf[0] = 0;
    // The material argument is a shared_ptr wrapper; raw name layout is unverified.
    return false;
}

// Resolve-once-ON-SUCCESS helper behind every named material accessor below.
// A plain `static MaterialPtr* m = createMaterial(...)` caches unconditionally:
// one call made before SigManager resolved the material group latches nullptr
// for the rest of the process.
inline MaterialPtr* sdk_cachedMaterial(MaterialPtr*& slot, const char* name) {
    if (!slot) slot = MaterialPtr::createMaterial(HashedString(name));
    return slot;
}

inline MaterialPtr* MaterialPtr::getSelectionBoxMaterial() {
    static MaterialPtr* m = nullptr; return sdk_cachedMaterial(m, "selection_box");
}
inline MaterialPtr* MaterialPtr::getSelectionOverlayMaterial() {
    static MaterialPtr* m = nullptr; return sdk_cachedMaterial(m, "selection_overlay");
}
inline MaterialPtr* MaterialPtr::getNameTagMaterial() {
    static MaterialPtr* m = nullptr; return sdk_cachedMaterial(m, "name_tag");
}
inline MaterialPtr* MaterialPtr::getNameTagBackfaceMaterial() {
    static MaterialPtr* m = nullptr; return sdk_cachedMaterial(m, "name_tag_with_backface");
}
inline MaterialPtr* MaterialPtr::getSelectionOverlayDoubleSidedMaterial() {
    static MaterialPtr* m = nullptr; return sdk_cachedMaterial(m, "selection_overlay_double_sided");
}
inline MaterialPtr* MaterialPtr::getEntityAlphatestMaterial() {
    static MaterialPtr* m = nullptr; return sdk_cachedMaterial(m, "entity_alphatest");
}
inline MaterialPtr* MaterialPtr::getUIColor() {
    static MaterialPtr* m = nullptr; return sdk_cachedMaterial(m, "ui_fill_color");
}
inline MaterialPtr* MaterialPtr::getUITextureAndColor() {
    static MaterialPtr* m = nullptr; return sdk_cachedMaterial(m, "ui_texture_and_color");
}
inline MaterialPtr* MaterialPtr::getUITextured() {
    static MaterialPtr* m = nullptr; return sdk_cachedMaterial(m, "ui_textured");
}
inline MaterialPtr* MaterialPtr::getImGui() {
    static MaterialPtr* m = nullptr; return sdk_cachedMaterial(m, "ImGui");
}

using ScreenChangeCallback = std::function<void(const std::string&)>;
class ScreenEvents {
public:
    static ScreenEvents& get() { static ScreenEvents i; return i; }
    void onChange(ScreenChangeCallback cb) { mCallbacks.push_back(std::move(cb)); }
    void fire(const std::string& n) { for (auto& cb : mCallbacks) if (cb) cb(n); }
private:
    std::vector<ScreenChangeCallback> mCallbacks;
};

} // SDK
