#include "Inventory.h"
#include "../../Memory/SigManager.h"
#include <Windows.h>
#include <cstring>

// Resolved by SigManager when available; 0 = handleAutoPlace/tryExit are no-ops.
uintptr_t g_ContainerScreenController_handleAutoPlace = 0;
uintptr_t g_ContainerScreenController_tryExit = 0;
// vtable[172] marker proving "this really is a ContainerScreenController".
uintptr_t g_ContainerScreenController_getSelectedSlotInfo = 0;
// ContainerScreenController::getItemStack(collectionName, slot) -> ItemStack*
uintptr_t g_ContainerScreenController_getItemStack = 0;
// ItemStackNetManagerScreen::requestDrop(this, {collection,slot}, {mode,amount,flag})
uintptr_t g_ItemStackNetManager_requestDrop = 0;
// ContainerScreenController::_tryAutoPlace / _getSlotInfo (see Inventory.h)
uintptr_t g_ContainerScreenController_tryAutoPlace = 0;
uintptr_t g_ContainerScreenController_getSlotInfo = 0;

namespace SDK {

// SEH helpers — no C++ objects (C2712).

static void* sehReadPtr(void* p) {
    if (!p || !sdk_safe_ptr(p, 8)) return nullptr;
    void* r = nullptr;
    __try { r = *(void**)p; } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return r;
}

static short sehReadI16(void* p) {
    if (!p || !sdk_safe_ptr(p, 2)) return 0;
    short v = 0;
    __try { v = *(short*)p; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return v;
}

static unsigned short sehReadU16(void* p) {
    if (!p || !sdk_safe_ptr(p, 2)) return 0;
    unsigned short v = 0;
    __try { v = *(unsigned short*)p; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return v;
}

// Item* from ItemStackBase: +0x08 is WeakPtr<Item> (Item** control) or raw Item*.
static void* sehResolveItem(void* stack) {
    if (!stack || !sdk_safe_ptr(stack, 0x18)) return nullptr;
    void* raw = sehReadPtr((void*)((uintptr_t)stack + 0x08));
    if (!raw) return nullptr;
    // Prefer WeakPtr deref when it yields a plausible vtable object.
    void* d = sehReadPtr(raw);
    if (d && sdk_safe_ptr(d, sizeof(void*))) {
        void* vt = sehReadPtr(d);
        if (vt && sdk_safe_ptr(vt, 8)) return d;
    }
    if (sdk_safe_ptr(raw, sizeof(void*))) {
        void* vt = sehReadPtr(raw);
        if (vt && sdk_safe_ptr(vt, 8)) return raw;
    }
    return nullptr;
}

// CompoundTag* userData @ ItemStackBase+0x10 (unique_ptr / raw).
static void* sehGetUserData(void* stack) {
    if (!stack || !sdk_safe_ptr(stack, 0x18)) return nullptr;
    return sehReadPtr((void*)((uintptr_t)stack + 0x10));
}

static int sehCallGetDamageValue(void* self, uintptr_t fn) {
    if (!self || !fn) return -1;
    __try {
        using fn_t = unsigned short(__fastcall*)(void*);
        return (int)((fn_t)fn)(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Item::getMaxDamage virtual 0x24 → movzx eax,[rcx+0x150] (IDA @0x142139C20).
static int sehCallItemMaxDamage(void* item) {
    if (!item || !sdk_safe_ptr(item, 0x160)) return 0;
    // Fast path: verified field on 1.26.30.05.
    int field = (int)sehReadU16((void*)((uintptr_t)item + 0x150));
    if (field > 0 && field < 100000) return field;

    __try {
        void** vt = *(void***)item;
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * 0x25)) return 0;
        void* fn = vt[0x24];
        if (!fn) return 0;
        using fn_t = int(__fastcall*)(void*);
        int v = ((fn_t)fn)(item);
        if (v > 0 && v < 100000) return v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return field > 0 ? field : 0;
}

int ItemStack::getDamageValue() {
    // 1) ItemStackBase::getDamageValue — NBT "Damage" (IDA @0x1420FCCF0).
    uintptr_t fn = SigManager::GetInstance().ItemStackBase_getDamageValue;
    if (fn) {
        int v = sehCallGetDamageValue(this, fn);
        if (v >= 0 && v < 100000) return v;
    }

    // 2) Manual: if userData present, game already returned 0 when tag missing
    //    (undamaged). Keep 0. Don't invent aux garbage.

    // 3) Legacy aux @+0x20 ONLY when item is damageable (max>0) and aux in range.
    int mx = getMaxDamage();
    if (mx > 0) {
        int aux = (int)sehReadI16((void*)((uintptr_t)this + 0x20));
        if (aux > 0 && aux <= mx) return aux;
    }
    return 0;
}

int ItemStack::getMaxDamage() {
    void* item = sehResolveItem(this);
    if (!item) return 0;
    return sehCallItemMaxDamage(item);
}

// Remaining durability (max - getDamageValue()).
int ItemStack::getRemainingDamage() {
    int mx = getMaxDamage();
    if (mx <= 0) return 0;
    int dmg = getDamageValue();
    if (dmg < 0) dmg = 0;
    if (dmg > mx) dmg = mx;
    return mx - dmg;
}

// ContainerScreenController identity + safe entry points.
// 1.26.50.04: _getSelectedSlotInfo sits at vtable[172] in the family's base
// vtable (0x14E907250, 185 slots; ctor sub_144DEF480). this+0x12C0 is the slot
// model; handleAutoPlace derefs it with no null check and calls its vtable[8]
// through a CFG-guarded icall before anything else. +0x0D48 is a screen-name
// std::string on this build (not 1.26.42's NonOwnerPointer<IClientInstance>),
// so the vtable marker plus the model walk are the validation that matters.
static constexpr uintptr_t kCscModelField = 0x12C0;
static constexpr size_t    kCscMinSize    = 0x12D8;
// Marker slot in the BASE vtable (172 of 185 on 1.26.50.04/1.26.51.01);
// subclasses shift it to 64/120/230/... so it is not a fixed identity check.
// Kept for the log.
static constexpr int       kCscBaseMarkerSlot = 172;

// vtable marker scan: _getSelectedSlotInfo appears exactly once in every
// vtable of the family (no subclass overrides it), just at a different slot
// each. Finding it anywhere is a sound family test; found slots are cached
// per vtable (vtables never move), so the steady-state cost is one comparison.
struct VtMarkerEntry { void** vt; int slot; };
static VtMarkerEntry s_markerCache[8];
static int           s_markerCacheCount = 0;

// SEH-guarded scan. Reads past the end of the vtable land in .rdata (harmless);
// the guard exists for a `ctrl` whose vtable pointer is garbage.
static int sehFindMarkerSlot(void** vt, uintptr_t marker) {
    if (!vt) return -1;
    __try {
        for (int i = 0; i < 1024; ++i)
            if ((uintptr_t)vt[i] == marker) return i;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return -1;
}

static int findMarkerSlot(void** vt, uintptr_t marker, bool* wasCached) {
    if (wasCached) *wasCached = false;
    if (!vt) return -1;
    for (int i = 0; i < s_markerCacheCount; ++i) {
        if (s_markerCache[i].vt == vt) {
            if (wasCached) *wasCached = true;
            return s_markerCache[i].slot;
        }
    }
    const int slot = sehFindMarkerSlot(vt, marker);
    if (slot >= 0 && s_markerCacheCount < 8) {
        s_markerCache[s_markerCacheCount].vt = vt;
        s_markerCache[s_markerCacheCount].slot = slot;
        ++s_markerCacheCount;
    }
    return slot;
}

static bool sehCheckCsc(void* ctrl, uintptr_t marker,
                        ContainerScreenController::RejectReason* why,
                        ContainerScreenController::CscProbe* probe) {
    using R = ContainerScreenController::RejectReason;
    const auto fail = [&](R r) -> bool { if (why) *why = r; return false; };
    if (probe) probe->ctrl = ctrl;

    if (!ctrl)               return fail(R::kNullController);
    if (!marker)             return fail(R::kMarkerUnresolved);
    if (!sdk_safe_ptr(ctrl, kCscMinSize))
                             return fail(R::kNotReadable);
    __try {
        void** vt = *(void***)ctrl;
        if (probe) probe->vtable = vt;
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * 64))
            return fail(R::kNotReadable);

        // Decisive: the family marker must appear in this object's vtable. The
        // slot is subclass-dependent, so scan rather than compare a fixed index.
        bool cached = false;
        const int slot = findMarkerSlot(vt, marker, &cached);
        if (probe) {
            probe->markerSlot = slot;
            // vt[172] is reported even on success: it identifies which family
            // member we are looking at when the slot is not the base's 172.
            if (sdk_safe_ptr(vt, sizeof(void*) * (kCscBaseMarkerSlot + 1)))
                probe->vt172 = (uintptr_t)vt[kCscBaseMarkerSlot];
        }
        if (slot < 0)        return fail(R::kVtableMarkerMismatch);
        if (!vt[39])         return fail(R::kAutoPlaceGateNull);

        // Everything handleAutoPlace itself derefs before its first icall.
        void* model = *(void**)((uintptr_t)ctrl + kCscModelField);
        if (!model || !sdk_safe_ptr(model, sizeof(void*)))
            return fail(R::kSlotModelNull);
        void** mvt = *(void***)model;
        if (!mvt || !sdk_safe_ptr(mvt, sizeof(void*) * 9))
            return fail(R::kSlotModelNull);
        if (!mvt[8])         return fail(R::kSlotModelVtableNull);
        if (why) *why = R::kAccepted;
        (void)cached;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return fail(R::kNotReadable); }
}

bool ContainerScreenController::isContainerScreenController(void* ctrl) {
    return sehCheckCsc(ctrl, g_ContainerScreenController_getSelectedSlotInfo,
                       nullptr, nullptr);
}

bool ContainerScreenController::isContainerScreenController(void* ctrl, RejectReason* why,
                                                            CscProbe* probe) {
    return sehCheckCsc(ctrl, g_ContainerScreenController_getSelectedSlotInfo, why, probe);
}

// One-line reason text for logs.
const char* ContainerScreenController::rejectReasonText(RejectReason r) {    switch (r) {
    case RejectReason::kAccepted:            return "accepted";
    case RejectReason::kNullController:      return "controller is null (ScreenView has no controller)";
    case RejectReason::kNotReadable:         return "controller not safely readable / SEH fault";
    case RejectReason::kMarkerUnresolved:    return "_getSelectedSlotInfo marker unresolved";
    case RejectReason::kVtableMarkerMismatch:return "marker not found anywhere in vtable (not a ContainerScreenController)";
    case RejectReason::kAutoPlaceGateNull:   return "vtable[39] auto-place gate is null";
    case RejectReason::kSlotModelNull:       return "[ctrl+0x12C0] slot model is null";
    case RejectReason::kSlotModelVtableNull: return "slot model vtable[8] is null";
    }
    return "unknown";
}

// The slot-action model (CSC+0x12C0): the ItemStackNetManagerScreen-family
// manager, confirmed as tryAutoPlace's `this` by the CSC auto-place core
// (sub_144DD2ED0). Verify members by vtable scan before calling — a wrong-type
// guess corrupts the request queue rather than faulting.
void* ContainerScreenController::getSlotModel() {
    if (!sdk_safe_ptr(this, kCscMinSize)) return nullptr;
    void* model = nullptr;
    __try { model = *(void**)((uintptr_t)this + kCscModelField); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return (model && sdk_safe_ptr(model, sizeof(void*))) ? model : nullptr;
}

// (this, name, slot) -> slot info blob. Resolved by SigManager; 0 = no-op,
// returns null (callers fall back to handleAutoPlace).
static void* sehGetSlotInfoCall(void* self, void* fn, const std::string* name, int slot) {
    if (!self || !fn || !name) return nullptr;
    __try {
        using fn_t = void*(__fastcall*)(void*, const std::string&, int);
        void* r = ((fn_t)fn)(self, *name, slot);
        return (r && sdk_safe_ptr(r, 0x40)) ? r : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void* ContainerScreenController::getSlotInfo(const std::string& collectionName, int slot) {
    if (slot < 0 || slot > 53) return nullptr;
    if (!g_ContainerScreenController_getSlotInfo) return nullptr;
    if (!sdk_safe_ptr((void*)g_ContainerScreenController_getSlotInfo, 16)) return nullptr;
    if (!isContainerScreenController(this)) return nullptr;
    return sehGetSlotInfoCall(this, (void*)g_ContainerScreenController_getSlotInfo,
                              &collectionName, slot);
}

// (this, name, slot, slotInfo) -> bool moved. Resolved by SigManager; 0 = false.
static bool sehTryAutoPlaceCall(void* self, void* fn, const std::string* name, int slot, void* slotInfo) {
    if (!self || !fn || !name) return false;
    __try {
        using fn_t = unsigned char(__fastcall*)(void*, const std::string&, int, void*);
        return ((fn_t)fn)(self, *name, slot, slotInfo) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ContainerScreenController::tryAutoPlace(const std::string& collectionName, int slot, void* slotInfo) {
    if (slot < 0 || slot > 53) return false;
    if (!g_ContainerScreenController_tryAutoPlace) return false;
    if (!sdk_safe_ptr((void*)g_ContainerScreenController_tryAutoPlace, 16)) return false;
    if (!isContainerScreenController(this)) return false;
    return sehTryAutoPlaceCall(this, (void*)g_ContainerScreenController_tryAutoPlace,
                               &collectionName, slot, slotInfo);
}

static bool sehAutoPlaceCall(void* self, void* fn, uintptr_t amount, const std::string* name, int slot) {
    if (!self || !fn || !name) return false;
    __try {
        using fn_t = void(__fastcall*)(void*, uintptr_t, const std::string&, int);
        ((fn_t)fn)(self, amount, *name, slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// (this, collectionName, slot) -> ItemStack*
static void* sehGetItemStackCall(void* self, void* fn, const std::string* name, int slot) {
    if (!self || !fn || !name) return nullptr;
    __try {
        using fn_t = void*(__fastcall*)(void*, const std::string&, int);
        void* r = ((fn_t)fn)(self, *name, slot);
        return (r && sdk_safe_ptr(r, 0x80)) ? r : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool sehTryExitCall(void* self, void* fn) {
    if (!self || !fn) return false;
    __try {
        using fn_t = void*(__fastcall*)(void*);
        ((fn_t)fn)(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void* sehReadVFunc(void* self, int idx) {
    if (!self || idx < 0) return nullptr;
    __try {
        void** vt = *(void***)self;
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * (size_t)(idx + 1))) return nullptr;
        return vt[idx];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void ContainerScreenController::handleAutoPlace(const std::string& collectionName, int slot) {
    if (slot < 0 || slot > 53) return;
    if (!g_ContainerScreenController_handleAutoPlace) return;
    if (!sdk_safe_ptr((void*)g_ContainerScreenController_handleAutoPlace, 16)) return;
    // The one check that matters — see the header comment.
    if (!isContainerScreenController(this)) return;
    sehAutoPlaceCall(this, (void*)g_ContainerScreenController_handleAutoPlace,
                     (uintptr_t)0x7FFFFFFF, &collectionName, slot);
}

ItemStack* ContainerScreenController::getItemStack(const std::string& collectionName, int slot) {
    if (slot < 0 || slot > 53) return nullptr;
    if (!g_ContainerScreenController_getItemStack) return nullptr;
    if (!sdk_safe_ptr((void*)g_ContainerScreenController_getItemStack, 16)) return nullptr;
    if (!isContainerScreenController(this)) return nullptr;
    return (ItemStack*)sehGetItemStackCall(
        this, (void*)g_ContainerScreenController_getItemStack, &collectionName, slot);
}

void ContainerScreenController::tryExit() {
    // Same identity requirement: _tryExit is reached through the object's own
    // vtable, so the object had better be the type that vtable belongs to.
    if (!isContainerScreenController(this)) return;
    if (g_ContainerScreenController_tryExit &&
        sdk_safe_ptr((void*)g_ContainerScreenController_tryExit, 16)) {
        sehTryExitCall(this, (void*)g_ContainerScreenController_tryExit);
        return;
    }
    // Fixed index 12, verified on this build: the function takes only `this`
    // (so no garbage-argument hazard) and references "gui.warning.exitGameWarning".
    void* fn = sehReadVFunc(this, 12);
    if (!fn || !sdk_safe_ptr(fn, 16)) return;
    sehTryExitCall(this, fn);
}

// ContainerManagerModel::getSlot — only ever calls an index that was explicitly
// verified. No probing: see the header for why guessing here is fatal.
int ContainerManagerModel::sGetSlotVIndex = -1;

ItemStack* ContainerManagerModel::getSlot(int slot) {
    if (slot < 0 || slot > 53) return nullptr;
    if (sGetSlotVIndex < 0) return nullptr;          // unknown -> unavailable
    if (!sdk_safe_ptr(this, sizeof(void*))) return nullptr;
    void* fn = sehReadVFunc(this, sGetSlotVIndex);
    if (!fn || !sdk_safe_ptr(fn, 16)) return nullptr;
    __try {
        using getFn = ItemStack*(__fastcall*)(void*, int);
        ItemStack* is = ((getFn)fn)(this, slot);
        if (is && sdk_safe_ptr(is, 0x80)) return is;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

} // namespace SDK

