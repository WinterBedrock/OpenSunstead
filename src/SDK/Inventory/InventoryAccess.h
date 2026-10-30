#pragma once
// Sunstead SDK — SEH-guarded inventory access. Requires the complete Actor +
// PlayerInventory/Container types; included from SDK.h after both.

#include "../Types.h"
#include "../Actor/Actor.h"
#include "Inventory.h"

namespace SDK {
namespace Inv {

// PlayerInventory::mContainer for the local player. POD-only body.
inline Container* container(Actor* lp) {
    if (!lp) return nullptr;
    __try {
        PlayerInventory* inv = lp->getSupplies();
        if (!inv) return nullptr;
        return inv->getContainer();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Currently-held hotbar slot, or -1 when unreadable.
inline int selectedSlot(Actor* lp) {
    if (!lp) return -1;
    __try {
        PlayerInventory* inv = lp->getSupplies();
        return inv ? inv->getSelectedSlot() : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Switch the held slot via PacketUtils::sendHotbarSlot so the SERVER learns
// about it too — a bare field write leaves the server attributing the next
// use/attack to the old item. Defined out-of-line in SDK.h.
bool setSelectedSlot(Actor* lp, int slot);

// ItemStack in an arbitrary container slot (0..8 hotbar, 9..35 main inventory).
inline ItemStack* itemAt(Actor* lp, int slot) {
    if (!lp || slot < 0) return nullptr;
    __try {
        PlayerInventory* inv = lp->getSupplies();
        if (!inv) return nullptr;
        Container* c = inv->getContainer();
        if (!c) return nullptr;
        return c->getItem(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Lowercased item name for a slot, or empty when the slot is empty / the name
// could not be resolved. Builds a std::string, so deliberately NOT inside a
// __try — getItemName() is internally guarded (see ItemStack).
inline std::string lowerNameAt(Container* c, int slot) {
    if (!c) return {};
    ItemStack* is = c->getItem(slot);
    if (!is) return {};
    std::string n = is->getItemName();
    for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
    return n;
}

// Scan slots [begin, end) and return the first whose lowercased name contains
// `needle`, or -1. `hint` is checked first (callers cache their last hit).
inline int findByName(Actor* lp, const char* needle, int begin, int end, int hint = -1) {
    if (!needle) return -1;
    Container* c = container(lp);
    if (!c) return -1;
    if (hint >= begin && hint < end) {
        std::string n = lowerNameAt(c, hint);
        if (!n.empty() && n.find(needle) != std::string::npos) return hint;
    }
    for (int slot = begin; slot < end; ++slot) {
        if (slot == hint) continue;
        std::string n = lowerNameAt(c, slot);
        if (!n.empty() && n.find(needle) != std::string::npos) return slot;
    }
    return -1;
}

} // namespace Inv
} // namespace SDK
