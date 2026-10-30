#pragma once
#include "../Types.h"
#include "../../Memory/OffsetManager.h"
#include <string>

// Sunstead inventory SDK - Bedrock 1.26.30.05, Latite-matched layout; all
// native access is SEH-guarded.
// Player::supplies (PlayerInventory*) @0x5B8; selectedSlot(int) @0x10,
// inventory(Container*) @0xB8; Container::getItem = vtable[7] (probed 7/8/6);
// ItemStackBase (0x80): item+0x08 (WeakPtr), block+0x18 (non-null => places a
// block), aux+0x20, count+0x22, valid+0x23.

namespace SDK {

class ItemStack; // defined below

// ContainerScreenController. handleAutoPlace (1.26.42.01, RVA 0x4d22980) is
// non-virtual and fatal on a wrong `this`: it calls vtable[39] (the "may I
// auto-place" gate), then derefs [this+0x12C0] with NO null check and calls
// that object's vtable[8] — a CFG-guarded icall. With garbage at +0x12C0 the
// call fails CFG and raises __fastfail(GUARD_ICALL_CHECK_FAILURE), which SEH
// CANNOT catch: instant process death. isContainerScreenController() closes
// the hole: the _getSelectedSlotInfo marker (a virtual no subclass overrides)
// must appear SOMEWHERE in the vtable — base slot 172 (185 slots on 1.26.50.04/
// 1.26.51.01) but 64/120/230/... in subclasses that add virtuals, so no fixed
// index works — and every field the call derefs is validated first.
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
    // Item currently in (collectionName, slot), or nullptr for empty/unavailable.
    // The game returns a shared empty-stack singleton for empty slots, so the
    // caller should still gate on isValidStack().
    ItemStack* getItemStack(const std::string& collectionName, int slot);
    // virtual _tryExit — vtable index 12, verified on this build (the function
    // references "gui.warning.exitGameWarning" and takes only `this`).
    void tryExit();

    // The slot-action model this delegates to (CSC+0x12C0, ItemStackNetManager-
    // Screen family). Null when absent/unreadable; caller owns any type proof.
    void* getSlotModel();

    // _getSlotInfo — resolve {collectionName, slot} to the slot info blob.
    // Never null: unresolvable slots yield a static empty info object, safe
    // to feed to tryAutoPlace.
    void* getSlotInfo(const std::string& collectionName, int slot);

    // _tryAutoPlace — the core shift-click move, both interaction models;
    // handleAutoPlace only reaches it on the touch model, hence the direct
    // call with a getSlotInfo result.
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

    // Item::mName is a std::string whose exact offset drifts between builds, so
    // candidate offsets are auto-probed and the first plausible name cached.
    // A plausible Bedrock item name is lowercase ascii, no spaces, possibly
    // ':' or '_' (e.g. "minecraft:diamond_sword") — random string-shaped
    // blobs get rejected. No __try here (C2712: this builds std::strings);
    // reads go through getItemPtrSafe() and the SEH-guarded sdk_safe_string().
    static bool looksLikeItemName(const std::string& s) {
        if (s.empty() || s.size() > 64) return false;
        for (unsigned char c : s) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == ':' || c == '.';
            if (!ok) return false;
        }
        return true;
    }

    // FNV-1a 64-bit exactly as MCBE's HashedString ctor computes it (basis
    // 0xCBF29CE484222325, prime 0x100000001B3, over the raw bytes). Reversed
    // from HashedString::HashedString in this build.
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

    // Locate Item::mName by probing the Item object for a plausible string at
    // candidate offsets; the first working base offset is cached. The
    // SigManager-resolved offset (from Item::buildDescriptionName) is tried
    // FIRST as the fast path.
    std::string getItemName() {
        std::string out;

        // +0x08 may be the Item* directly OR a WeakPtr<Item>, so try the raw
        // value and one extra dereference; the scan decides which is real.
        void* raw = getItemPtrSafe();
        void* items[2] = { raw, derefPtr(raw) };

        // Cache: which item-ptr interpretation + string offset last worked.
        static uintptr_t s_strOff = 0;   // cached std::string offset (0 = unprobed)
        static int       s_which  = 0;   // cached item-ptr interpretation index

        // Read the std::string at (item + strOff) and accept it if it looks
        // like a Bedrock item name. On this build mName is a PLAIN std::string
        // (data@item+0x78, size@+0x88), NOT a {hash;string} HashedString, so
        // an FNV hash match is not required.
        auto tryString = [&](void* item, uintptr_t strOff) -> bool {
            if (!item) return false;
            if (!sdk_safe_ptr(item, strOff + sizeof(std::string))) return false;
            std::string s;
            if (!sdk_safe_string((void*)((uintptr_t)item + strOff), s)) return false;
            if (!looksLikeItemName(s)) return false;
            out = s;
            return true;
        };

        // Fast path: cached offset from a previous successful resolve.
        if (s_strOff != 0) {
            if (tryString(items[s_which], s_strOff)) return out;
            if (s_which == 0 && tryString(items[1], s_strOff)) { s_which = 1; return out; }
            if (s_which == 1 && tryString(items[0], s_strOff)) { s_which = 0; return out; }
            // Some BlockItems / special items keep mName at a different offset
            // than the cached one — try a short known list before giving up.
            static const uintptr_t kAlt[] = {
                0x78, 0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0,
                0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0x100
            };
            for (int w = 0; w < 2; ++w) {
                for (uintptr_t off : kAlt) {
                    if (off == s_strOff) continue;
                    if (tryString(items[w], off)) {
                    // Don't overwrite cache — keep the common path fast.
                        return out;
                    }
                }
            }
            return {};
        }

        // SigManager-resolved std::string offset (from Item::buildDescriptionName).
        int sigStr = Offsets().Item_mNameStrOffset;
        if (sigStr >= 8) {
            for (int w = 0; w < 2; ++w)
                if (tryString(items[w], (uintptr_t)sigStr)) {
                    s_strOff = (uintptr_t)sigStr; s_which = w; return out;
                }
        }

        // Cold-cache fallback scan (once per process until a name is found).
        for (int w = 0; w < 2; ++w)
            for (uintptr_t off = 0x18; off <= 0x260; off += 8)
                if (tryString(items[w], off)) {
                    s_strOff = off; s_which = w; return out;
                }
        return {};
    }

    // Diagnostic dump of what getItemName() sees for this stack, so a log can
    // pinpoint the real name offset on failure: "raw=<ptr> deref=<ptr> |
    // w<W>@0x<OFF>='<name>'" on success, or "NO MATCH" plus the first few
    // readable string slots.
    std::string debugNameInfo() {
        char hdr[96];
        void* raw = getItemPtrSafe();
        void* drf = derefPtr(raw);
        sprintf(hdr, "raw=%p deref=%p | ", raw, drf);
        std::string info = hdr;

        std::string name = getItemName();
        if (!name.empty()) { info += "resolved='" + name + "'"; return info; }

        info += "NO MATCH; readable slots:";
        void* items[2] = { raw, drf };
        int shown = 0;
        for (int w = 0; w < 2 && shown < 12; ++w) {
            if (!items[w]) continue;
            for (uintptr_t off = 0x18; off <= 0x120 && shown < 12; off += 8) {
                if (!sdk_safe_ptr(items[w], off + sizeof(std::string))) continue;
                std::string s;
                if (!sdk_safe_string((void*)((uintptr_t)items[w] + off), s)) continue;
                if (s.empty() || s.size() > 48) continue;
                bool printable = true;
                for (unsigned char c : s) if (c < 0x20 || c > 0x7E) { printable = false; break; }
                if (!printable) continue;
                char ent[80];
                sprintf(ent, " w%d@0x%X='", w, (unsigned)off);
                info += ent; info += s; info += "'";
                ++shown;
            }
        }
        return info;
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
    // Melee-weapon classification independent of the item NAME string: the
    // name-based path builds a std::string that can come back empty, so this
    // POD-only variant uses SEH-guarded raw reads only. A "weapon" here is any
    // valid NON-block stack (weapons carry no Block* at +0x18).
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

// FillingContainer / Inventory. getItem is a virtual; the working index is
// probed once and cached (GameMode's vtable is +1 in this image vs the Latite
// header, so the inventory vtable could differ too - we don't assume).
class Container {
public:
    ItemStack* getItem(int slot) {
        if (slot < 0 || slot > 53) return nullptr;
        if (!sdk_safe_ptr(this, sizeof(void*))) return nullptr;
        void** vt = *(void***)this;
        if (!vt) return nullptr;
        static int s_idx = 0;           // 0 = not yet probed
        static const int kCand[] = { 7, 8, 6 };
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
        if (s_idx != 0) {
            ItemStack* is = tryIdx(s_idx);
            if (is) return is;
            s_idx = 0; // cached index went bad - re-probe
        }
        for (int i = 0; i < 3; ++i) {
            ItemStack* is = tryIdx(kCand[i]);
            if (is) { s_idx = kCand[i]; return is; }
        }
        return nullptr;
    }
};

// Open-container model on the player while a chest/UI is open.
// Calling guessed vtable slots passes garbage arguments, and any CFG-guarded
// indirect call the callee makes with them raises __fastfail — uncatchable.
// So getSlot only calls an explicitly verified slot and otherwise reports
// "unavailable"; chest contents come from
// ContainerScreenController::getItemStack() instead.
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
