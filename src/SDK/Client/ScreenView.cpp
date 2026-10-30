#include "ScreenView.h"
#include <cstring>
#include "../../Core/Logger.h"

namespace SDK {
namespace {

// SEH-safe walk of vector<shared_ptr<UIControl>> at +0x98.
// shared_ptr = 16 bytes; object pointer is the first qword.
void walkChildren(UIControl* parent, void (*fn)(UIControl*, void*), void* ctx, bool recursive) {
    if (!parent || !fn) return;
    const int childOff = Offsets().UI_Children;
    if (!childOff || !sdk_safe_ptr(parent, childOff + 0x18)) return;

    __try {
        uintptr_t base = reinterpret_cast<uintptr_t>(parent) + childOff;
        uintptr_t begin = *reinterpret_cast<uintptr_t*>(base);
        uintptr_t end   = *reinterpret_cast<uintptr_t*>(base + 8);
        if (!begin || end < begin) return;
        const uintptr_t stride = 16; // shared_ptr
        if (((end - begin) % stride) != 0) return;
        size_t n = (end - begin) / stride;
        if (n > 512) n = 512; // sanity

        for (size_t i = 0; i < n; ++i) {
            UIControl* child = *reinterpret_cast<UIControl**>(begin + i * stride);
            if (!child || !sdk_safe_ptr(child, 0xA0)) continue;
            fn(child, ctx);
            if (recursive) walkChildren(child, fn, ctx, true);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

void UIControl::forEachChildRaw(void (*fn)(UIControl*, void*), void* ctx) {
    walkChildren(this, fn, ctx, false);
}

void UIControl::forEachDescendantRaw(void (*fn)(UIControl*, void*), void* ctx) {
    walkChildren(this, fn, ctx, true);
}

// Depth-first search for a descendant whose control name equals `want` (the
// Latite MovableChat approach targets controls by name, e.g. "chat_panel").
// The comparison lives in a POD C-context so the walker stays SEH-clean
// (std::string locals cannot live beside __try).
namespace {
struct FindNameCtx { const char* want; size_t wantLen; UIControl* out; };
// POD-only string compare (MSVC std::string layout: SSO buf at +0, size at
// +0x10, capacity at +0x18; >= 16 means heap pointer at +0). Never constructs
// a std::string, so it is C2712-clean inside __try.
bool sehNameEquals(UIControl* c, const char* want, size_t wantLen) {
    __try {
        const uintptr_t base = reinterpret_cast<uintptr_t>(c) + Offsets().UI_Name;
        if (!sdk_safe_ptr(reinterpret_cast<void*>(base), 0x20)) return false;
        const size_t len = *reinterpret_cast<const size_t*>(base + 0x10);
        if (len != wantLen || len >= 4096) return false;
        const size_t cap = *reinterpret_cast<const size_t*>(base + 0x18);
        const char* data = (cap >= 16)
            ? *reinterpret_cast<const char* const*>(base)
            : reinterpret_cast<const char*>(base);
        if (!sdk_safe_ptr(const_cast<char*>(data), len)) return false;
        return std::memcmp(data, want, len) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void findNameVisit(UIControl* c, void* ud) {
    FindNameCtx* ctx = static_cast<FindNameCtx*>(ud);
    if (ctx->out) return;   // already found
    if (sehNameEquals(c, ctx->want, ctx->wantLen)) ctx->out = c;
}
} // namespace

UIControl* UIControl::findDescendant(const std::string& want) {
    if (want.empty() || !sdk_safe_ptr(this, Offsets().UI_Children + 0x18)) return nullptr;
    FindNameCtx ctx{ want.c_str(), want.size(), nullptr };
    forEachDescendantRaw(&findNameVisit, &ctx);
    return ctx.out;
}

void UIControl::offsetY(float dy) {
    if (!sdk_safe_ptr(this, 0x50)) return;
    const int flagsOff = Offsets().UI_Flags;
    __try {
        Vec2* rel = getParentRelativePosition();
        Vec2* abs = getPosition();
        if (rel) rel->y += dy;
        if (abs) abs->y += dy;
        // Mark cached position dirty (bit 0) so the game re-resolves layout.
        if (flagsOff) *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(this) + flagsOff) |= 1ull;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Latite UIControl::updatePos — recompute the absolute position from the
// parent's absolute + our parent-relative position (fixing the parent chain
// first when its dirty bit is set), then CLEAR our dirty bit so the game's
// layout pass does not overwrite the value. POD-only body (C2712).
bool UIControl::updatePos() {
    const int childOff = Offsets().UI_Children;
    const int parentOff = Offsets().UI_Parent;
    const int flagsOff = Offsets().UI_Flags;
    if (!childOff || !sdk_safe_ptr(this, childOff + 0x18)) return false;
    __try {
        UIControl* parent = parentOff
            ? *reinterpret_cast<UIControl**>(reinterpret_cast<uintptr_t>(this) + parentOff)
            : nullptr;
        Vec2 add{ 0.f, 0.f };
        if (parent) {
            uint64_t* pFlags = flagsOff
                ? reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(parent) + flagsOff)
                : nullptr;
            if (pFlags && (*pFlags & 1ull) && !parent->updatePos()) return false;
            Vec2* pPos = parent->getPosition();
            if (!pPos || !sdk_safe_ptr(pPos, sizeof(Vec2))) return false;
            add = *pPos;
        }
        Vec2* rel = getParentRelativePosition();
        Vec2* abs = getPosition();
        if (!rel || !sdk_safe_ptr(rel, sizeof(Vec2))) return false;
        if (!abs || !sdk_safe_ptr(abs, sizeof(Vec2))) return false;
        abs->x = rel->x + add.x;
        abs->y = rel->y + add.y;
        if (flagsOff) *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(this) + flagsOff) &= ~1ull;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {
// POD visitor ctx: propagates any failure up through the shared flag.
struct UpdateCtx { int failed; };
void updatePosVisit(UIControl* c, void* ud) {
    UpdateCtx* ctx = static_cast<UpdateCtx*>(ud);
    if (ctx->failed) return;
    if (!c->updatePos()) ctx->failed = 1;
}
} // namespace

void UIControl::updateDescendantsPos() {
    UpdateCtx ctx{ 0 };
    forEachDescendantRaw(&updatePosVisit, &ctx);
}

bool UIControl::shiftOffscreen(bool hide) {
    if (!sdk_safe_ptr(this, 0x50)) return false;
    __try {
        Vec2* pos = getPosition();
        if (!pos || !sdk_safe_ptr(pos, sizeof(Vec2))) return false;
        if (hide) {
            if (pos->x >= 20000.f) return false;   // already hidden
            pos->x += 20000.f;
        } else {
            if (pos->x < 20000.f) return false;     // already in place
            pos->x -= 20000.f;
        }
        updateDescendantsPos();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 1.26.50.04: screen name lives at view->visualTree(+0x50)->rootControl(+0x08)
// ->name(+0x20). Offsets were reversed from the binary (VisualTree+0x08 read by
// getControlByName sub_1445B9650 and sub_1445B9160; the string trio at
// UIControl+0x20/+0x30/+0x38 by sub_1445C7E30). See OffsetManager SV_*/VT_*/UI_*.
std::string ScreenView::getScreenName() {
    VisualTree* vt = getVisualTree();
    if (!vt || !sdk_safe_ptr(vt, 0x10)) return "";
    UIControl* root = vt->getRootControl();
    if (!root || !sdk_safe_ptr(root, 0x40)) return "";
    std::string out;
    if (!sdk_safe_string(root->getName(), out)) return "";
    return out;
}

} // SDK
