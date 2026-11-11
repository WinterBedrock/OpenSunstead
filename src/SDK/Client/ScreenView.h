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

    bool updatePos();
    // updatePos for the whole descendant tree (Latite MovableChat calls
    // control->updatePos() on every descendant after shifting the panel).
    void updateDescendantsPos();
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

extern bool g_chatInputOpen;

inline bool isGameplayInputActive() {
    return !g_chatInputOpen && ScreenView::currentScreenName() == "hud_screen";
}

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

enum class Primitive : int { Quad = 1, Triangle = 2, Line = 4 };

class Tessellator {
public:
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
