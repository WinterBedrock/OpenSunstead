#include "Offhand.h"
#include "../../SDK/SDK.h"
#include "../Setting.h"
#include "../../Core/Logger.h"
#include <Windows.h>

using namespace SDK;

namespace {

// The setOffhandSlot virtual, EXACT index — 78 on 1.26.50.04. Pinned by
// REVERSING the LocalPlayer vtable (off_14E8E0BC0, 245 slots): slot 77 is
// Actor::getCarriedItem (Horion's old dump 161 shifted by 84) and slot 79 is
// Actor::getEquippedTotem (old 164 shifted by 85 — setCarriedItem was
// removed between them). sub_14476AF80 (slot 78) takes (this, ItemStack
// const&), reads the equipment component's slot 1, compares it with the new
// stack, writes it through the base setter and broadcasts the offhand-change
// event — exactly setOffhandSlot. The previous guess of 77 pointed at the
// harmless getCarriedItem getter, which is why the totem never equipped.
constexpr int kEquipSlot = 78;
int g_equipSlot = -1;          // verified-once flag (or -2 = hopeless)
long long g_lastProbeTick = -1;

int g_getItemIdx = -1;
int g_removeItemIdx = -1;

bool callEquipVirtual(Actor* lp, int slot, ItemStack* stack) {
    if (!lp || !stack) return false;
    void** vt = *(void***)lp;
    if (!vt || !sdk_safe_ptr(vt, ::Layout::VTable::Guard(slot + 1))) return false;
    void* fn = vt[slot];
    if (!fn || !sdk_safe_ptr(fn, 16)) return false;
    __try {
        ((void(__fastcall*)(Actor*, ItemStack*))fn)(lp, stack);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int probeGetItemIndex(Container* c) {
    if (!c || !sdk_safe_ptr(c, sizeof(void*))) return -1;
    void** vt = *(void***)c;
    if (!vt) return -1;
    for (int idx : { 7, 8, 6 }) {
        if (!sdk_safe_ptr(vt, sizeof(void*) * (idx + 1))) continue;
        void* fn = vt[idx];
        if (!fn || !sdk_safe_ptr(fn, 16)) continue;
        __try {
            using getFn = ItemStack*(__fastcall*)(void*, int);
            ItemStack* is = ((getFn)fn)(c, 0); // slot 0 always exists
            if (is && sdk_safe_ptr(is, 0x80)) return idx;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return -1;
}

int stackCount(ItemStack* is) {
    if (!is || !sdk_safe_ptr(is, 0x80)) return 0;
    if (!is->isValidStack()) return 0;
    return is->getCount();
}

bool callRemoveItem(Container* c, int idx, int slot, int count) {
    if (!c || idx < 0 || !sdk_safe_ptr(c, sizeof(void*))) return false;
    void** vt = *(void***)c;
    if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * (idx + 1))) return false;
    void* fn = vt[idx];
    if (!fn || !sdk_safe_ptr(fn, 16)) return false;
    __try {
        ((void(__fastcall*)(Container*, int, int))fn)(c, slot, count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehDecrementCount(ItemStack* is) {
    if (!is || !sdk_safe_ptr(is, 0x80)) return false;
    __try {
        unsigned char* p = (unsigned char*)((uintptr_t)is + 0x22);
        if (*p > 1) { --*p; return true; }
        *p = 0; // last item: mark empty
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const char* wantedNameFragment(int itemMode) {
    return itemMode == 0 ? "totem" : "shield";
}

long long sehGetTicks(Actor* lp) {
    __try { return lp->getTicksExisted(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool stackNameContains(ItemStack* is, const char* frag) {
    if (!is || !sdk_safe_ptr(is, 0x80)) return false;
    std::string n = is->getItemName();
    if (n.empty()) return false;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    return n.find(frag) != std::string::npos;
}

int offhandState(Actor* lp, int itemMode) {
    void* cont = lp->getOffhandContainer();
    if (!cont) return -1;
    __try {
        Container* c = (Container*)cont;
        ItemStack* off = c->getItem(1);
        if (!off || !off->isValidStack()) return 0;      // empty / absent
        return stackNameContains(off, wantedNameFragment(itemMode)) ? 1 : 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

} // namespace

void Offhand::OnEnable() {
    mEquipSlot = -1;
    mLastFailTick = -1;
    g_equipSlot = -1;
    g_lastProbeTick = -1;
    g_getItemIdx = -1;
    g_removeItemIdx = -1;
}

void Offhand::OnDisable() {
    mEquipSlot = -1;
    mLastFailTick = -1;
}

void Offhand::OnTick() {
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    Actor* lp = ci->getLocalPlayer();
    if (!lp) return;

    const int itemMode = GetItem();

    if (GetOnlyInv()) {
        if (SDK::ScreenView::currentScreenName() != "inventory_screen") return;
    }

    if (GetOnlyLow()) {
        auto hp = lp->getHealth();
        if (!hp || *hp > GetHpThr()) return;
    }

    TryEquip(lp, itemMode);
}

bool Offhand::TryEquip(Actor* lp, int itemMode) {
    const int st = offhandState(lp, itemMode);
    if (st == 1) return true;        // done
    if (st == 2) return false;       // user's own item — do not replace
    // st == 0 (empty) or -1 (unreadable) -> try to equip.

    // Debounce failed probes: one retry per 20 ticks.
    const long long tick = sehGetTicks(lp);
    if (g_equipSlot == -2 && g_lastProbeTick >= 0 && tick - g_lastProbeTick < 20) return false;

    // Find the desired stack anywhere in the 36-slot inventory.
    PlayerInventory* inv = lp->getSupplies();
    Container* supplies = inv ? inv->getContainer() : nullptr;
    if (!supplies) return false;
    const char* frag = wantedNameFragment(itemMode);
    int srcSlot = -1;
    ItemStack* srcStack = nullptr;
    for (int slot = 0; slot < 36; ++slot) {
        ItemStack* is = supplies->getItem(slot);
        if (is && is->isValidStack() && stackNameContains(is, frag)) {
            srcSlot = slot; srcStack = is; break;
        }
    }
    if (!srcStack) return false; // nothing to equip

    // Baseline counts for the before/after verification.
    const int srcBefore = stackCount(srcStack);

    // Equip through the EXACT virtual, verified by the offhand state after
    // the call. A failure means the layout moved — retry later, no fallback.
    bool equipped = false;
    if (g_equipSlot != -2) {
        equipped = callEquipVirtual(lp, kEquipSlot, srcStack) &&
                   offhandState(lp, itemMode) == 1;
    }
    if (!equipped) {
        if (g_equipSlot != -2) {
            g_equipSlot = -2;   // the exact slot did not work; retry later
            g_lastProbeTick = tick;
            LOG_WARN("Offhand: setOffhandSlot(78) failed; retrying", "Offhand");
        }
        return false;
    }
    g_equipSlot = kEquipSlot;

    // Offhand now holds the item. Consume the source stack — but ONLY after
    // verifying the decrement actually happened (the duplication bug came
    // from consuming unconditionally after an unverified call chain).
    if (g_getItemIdx < 0) {
        g_getItemIdx = probeGetItemIndex(supplies);
        g_removeItemIdx = (g_getItemIdx >= 0) ? g_getItemIdx + 6 : -1;
    }

    bool consumed = false;
    if (g_removeItemIdx >= 0) {
        if (callRemoveItem(supplies, g_removeItemIdx, srcSlot, 1)) {
            // Verify: the source stack must now hold one fewer.
            ItemStack* re = supplies->getItem(srcSlot);
            if (stackCount(re) == srcBefore - 1 || (srcBefore == 1 && stackCount(re) == 0)) {
                consumed = true;
            }
        }
    }
    if (!consumed) {
        // removeItem not usable on this build: direct count write as the
        // guaranteed path (never leaves a duplicate).
        sehDecrementCount(srcStack);
    }
    return offhandState(lp, itemMode) == 1;
}

bool Offhand::TryEquipTotem(Actor* lp) {
    if (!lp) return false;
    return TryEquip(lp, 0);   // itemMode 0 = totem
}
