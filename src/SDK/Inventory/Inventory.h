#pragma once
#include "../Types.h"
#include "../../Memory/OffsetManager.h"
#include <string>


namespace SDK {

class ItemStack; // defined below

class ContainerScreenController {
public:
    // Named identity-check result so callers log the actual broken link.
    enum class RejectReason {
        kAccepted,
        kNullController,        // the ScreenView has no controller pointer
        kNotReadable,           // not enough committed bytes to even inspect
        kMarkerUnresolved,      // _getSelectedSlotInfo signature missed
        kVtableMarkerMismatch,  // marker absent from the vtable -> wrong class
        kAutoPlaceGateNull,     // vt[39] (the "may I auto-place" gate) is null
        kSlotModelNull,         // [ctrl+0x12C0] is null/unreadable
        kSlotModelVtableNull,   // model vtable[8] (the placed call) is null
    };

    // What the check actually saw, for diagnostics.
    struct CscProbe {
        void*       ctrl    = nullptr;  // the object examined
        void**      vtable  = nullptr;  // its vtable
        uintptr_t   vt172   = 0;        // value sitting in vtable[172] (base slot)
        int         markerSlot = -1;    // where the marker was found (-1 = nowhere)
    };

    // True only for a real ContainerScreenController (or subclass) that is safe
    // to call into. Never faults; returns false when unresolved.
    static bool isContainerScreenController(void* ctrl);
    // Same, and reports which check failed (kAccepted on success) plus what was
    // observed.
    static bool isContainerScreenController(void* ctrl, RejectReason* why,
                                            CscProbe* probe = nullptr);
    // One-line text for a RejectReason, for logs.
    static const char* rejectReasonText(RejectReason r);

    void handleAutoPlace(const std::string& collectionName, int slot);
    void handleAutoPlace(int slot) { handleAutoPlace(std::string("container_items"), slot); }
    ItemStack* getItemStack(const std::string& collectionName, int slot);
    // virtual _tryExit — vtable index 12, verified on this build (the function
    // references "gui.warning.exitGameWarning" and takes only `this`).
    void tryExit();

    // The slot-action model this delegates to (CSC+0x12C0, ItemStackNetManager-
    // Screen family). Null when absent/unreadable; caller owns any type proof.
    void* getSlotModel();

    void* getSlotInfo(const std::string& collectionName, int slot);

    bool tryAutoPlace(const std::string& collectionName, int slot, void* slotInfo);
};

// ItemStackBase view. We only need placement-relevant fields.
class ItemStack {
public:
    void*  getItemPtr()  { return *(void**)((uintptr_t)this + 0x08); }
    void*  getBlockPtr() { return *(void**)((uintptr_t)this + 0x18); }
    int    getCount()    { return (int)*(unsigned char*)((uintptr_t)this + 0x22); }

    // Damage taken (0 = pristine). ItemStackBase::getDamageValue NBT "Damage".
    int getDamageValue();
    // Max durability: Item vtable[0x24] / field Item+0x150 - 26.30
    int getMaxDamage();
    // Remaining = max - damage.
    int getRemainingDamage();

    // SEH-guarded raw Item* read, in its own function: MSVC C2712 forbids
    // __try in a std::string-building scope.
    void* getItemPtrSafe() {
        if (!sdk_safe_ptr(this, 0x80)) return nullptr;
        void* item = nullptr;
        __try { item = *(void**)((uintptr_t)this + 0x08); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        return item;
    }

    static bool looksLikeItemName(const std::string& s) {
        if (s.empty() || s.size() > 64) return false;
        for (unsigned char c : s) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == ':' || c == '.';
            if (!ok) return false;
        }
        return true;
    }

    static uint64_t fnv1a64(const std::string& s) {
        uint64_t h = 0xCBF29CE484222325ULL;
        for (unsigned char c : s) { h ^= c; h *= 0x100000001B3ULL; }
        return h;
    }

    // SEH-guarded 8-byte read (kept out of the std::string-building frame; C2712).
    static bool readU64(void* p, uint64_t& out) {
        if (!sdk_safe_ptr(p, 8)) return false;
        __try { out = *(uint64_t*)p; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // SEH-guarded single pointer dereference (own frame; C2712 keeps it out of
    // getItemName). Returns *(void**)p or nullptr on fault.
    static void* derefPtr(void* p) {
        if (!p || !sdk_safe_ptr(p, 8)) return nullptr;
        void* r = nullptr;
        __try { r = *(void**)p; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        return r;
    }

    std::string getItemName() {
        std::string out;

        const int stringOffset = Offsets().Item_mNameStrOffset;
        if (stringOffset < 8) return {};
        void* item = getItemPtrSafe();
        for (int attempt = 0; attempt < 2 && item; ++attempt, item = derefPtr(item)) {
            if (!sdk_safe_ptr(item, stringOffset + sizeof(std::string))) continue;
            if (!sdk_safe_string(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(item) + stringOffset), out))
                continue;
            if (looksLikeItemName(out)) return out;
            out.clear();
        }
        return {};
    }


    // Name-based weapon classification: swords and axes are the aura's melee
    // weapons; both deal full attack damage in Bedrock.
    bool isWeapon() {
        std::string n = getItemName();
        if (n.empty()) return false;
        for (auto& c : n) c = (char)tolower((unsigned char)c);
        return n.find("sword") != std::string::npos ||
               n.find("axe")   != std::string::npos; // axe / pickaxe both weapons
    }
    bool isSword() {
        std::string n = getItemName();
        if (n.empty()) return false;
        for (auto& c : n) c = (char)tolower((unsigned char)c);
        return n.find("sword") != std::string::npos;
    }
    bool isNonBlockItem() {
        if (!sdk_safe_ptr(this, 0x80)) return false;
        int c = getCount();
        if (c <= 0 || c > 64) return false;
        void* it = getItemPtrSafe();
        if (!it) return false;
        // Placeable blocks carry a Block* at +0x18; weapons/tools do not.
        void* b = nullptr;
        __try { b = *(void**)((uintptr_t)this + 0x18); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return b == nullptr;
    }

    // A stack placeable as a block (block class + non-zero count) — exactly
    // what GameMode::buildBlock consumes.
    bool isPlaceableBlock() {
        if (!sdk_safe_ptr(this, 0x80)) return false;
        int c = getCount();
        if (c <= 0 || c > 64) return false;
        void* b = getBlockPtr();
        return b != nullptr && sdk_safe_ptr(b, sizeof(void*));
    }
    // Any non-empty stack (item present, count>0).
    bool isValidStack() {
        if (!sdk_safe_ptr(this, 0x80)) return false;
        int c = getCount();
        if (c <= 0 || c > 64) return false;
        void* it = getItemPtr();
        return it != nullptr;
    }
};

class Container {
public:
    ItemStack* getItem(int slot) {
        if (slot < 0 || slot > 53) return nullptr;
        if (!sdk_safe_ptr(this, sizeof(void*))) return nullptr;
        void** vt = *(void***)this;
        if (!vt) return nullptr;
        constexpr int getItemSlot = 7;
        auto tryIdx = [&](int idx) -> ItemStack* {
            if (!sdk_safe_ptr(vt, sizeof(void*) * (idx + 1))) return nullptr;
            void* fn = vt[idx];
            if (!fn || !sdk_safe_ptr(fn, 16)) return nullptr;
            __try {
                using getFn = ItemStack*(__fastcall*)(void*, int);
                ItemStack* is = ((getFn)fn)(this, slot);
                if (is && sdk_safe_ptr(is, 0x80)) return is;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return nullptr;
        };
        return tryIdx(getItemSlot);
    }
};

class ContainerManagerModel {
public:
    // Verified vtable index for getSlot(int), or -1 until a signature confirms
    // the slot. Never a guess.
    static int sGetSlotVIndex;
    ItemStack* getSlot(int slot);
};

class PlayerInventory {
public:
    int  getSelectedSlot() {
        if (!sdk_safe_ptr(this, ::Layout::PlayerInv::SelectedSlot + sizeof(int))) return -1;
        int s = *(int*)((uintptr_t)this + ::Layout::PlayerInv::SelectedSlot);
        return (s >= 0 && s <= 8) ? s : -1;
    }
    int getInHandSlot() {
        if (!sdk_safe_ptr(this, ::Layout::PlayerInv::InHandSlot + sizeof(int))) return -1;
        int s = *(int*)((uintptr_t)this + ::Layout::PlayerInv::InHandSlot);
        return (s >= 0 && s <= 8) ? s : -1;
    }
    void setInHandSlot(int slot) {
        if (slot < 0 || slot > 8) return;
        if (!sdk_safe_ptr(this, ::Layout::PlayerInv::InHandSlot + sizeof(int))) return;
        __try { *(int*)((uintptr_t)this + ::Layout::PlayerInv::InHandSlot) = slot; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void setSelectedSlot(int slot) {
        if (slot < 0 || slot > 8) return;
        if (!sdk_safe_ptr(this, ::Layout::PlayerInv::InHandSlot + sizeof(int))) return;
        // Write BOTH mSelectedSlot (0x10) and mInHandSlot (0x1C) so the held item
        // actually changes and the item-use / attack path sees the new slot.
        __try {
            *(int*)((uintptr_t)this + ::Layout::PlayerInv::SelectedSlot) = slot;
            *(int*)((uintptr_t)this + ::Layout::PlayerInv::InHandSlot)   = slot;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    Container* getContainer() {
        constexpr uintptr_t kContainer = ::Layout::PlayerInv::Container;
        if (!sdk_safe_ptr(this, kContainer + sizeof(void*))) return nullptr;
        Container* c = *(Container**)((uintptr_t)this + kContainer);
        return (c && sdk_safe_ptr(c, sizeof(void*))) ? c : nullptr;
    }
};

} // namespace SDK
