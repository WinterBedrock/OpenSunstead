#include "Inventory.h"
#include <algorithm>
#include "../../Memory/SigManager.h"
#include <Windows.h>
#include <cstring>

uintptr_t g_ContainerScreenController_handleAutoPlace = 0;
uintptr_t g_ContainerScreenController_tryExit = 0;
uintptr_t g_ContainerScreenController_getSelectedSlotInfo = 0;
uintptr_t g_ContainerScreenController_getItemStack = 0;
uintptr_t g_ItemStackNetManager_requestDrop = 0;
uintptr_t g_ContainerScreenController_tryAutoPlace = 0;
uintptr_t g_ContainerScreenController_getSlotInfo = 0;

namespace SDK {

static void* readPtr(void* address) {
    if (!address || !sdk_safe_ptr(address, sizeof(void*))) return nullptr;
    __try { return *reinterpret_cast<void**>(address); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static short readI16(void* address) {
    if (!address || !sdk_safe_ptr(address, sizeof(short))) return 0;
    __try { return *reinterpret_cast<short*>(address); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static unsigned short readU16(void* address) {
    if (!address || !sdk_safe_ptr(address, sizeof(unsigned short))) return 0;
    __try { return *reinterpret_cast<unsigned short*>(address); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void* resolveItem(void* stack) {
    if (!stack || !sdk_safe_ptr(stack, 0x18)) return nullptr;
    void* raw = readPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stack) + 8));
    if (!raw) return nullptr;
    void* item = readPtr(raw);
    if (item && sdk_safe_ptr(item, sizeof(void*)) && readPtr(item)) return item;
    return sdk_safe_ptr(raw, sizeof(void*)) && readPtr(raw) ? raw : nullptr;
}

static int callDamageValue(void* self, uintptr_t fn) {
    if (!self || !fn) return -1;
    __try { return static_cast<int>(reinterpret_cast<unsigned short(__fastcall*)(void*)>(fn)(self)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static int callMaxDamage(void* item) {
    if (!item || !sdk_safe_ptr(item, 0x160)) return 0;
    const int field = readU16(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(item) + 0x150));
    if (field > 0 && field < 100000) return field;
    __try {
        void** vt = *reinterpret_cast<void***>(item);
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * 0x25)) return 0;
        void* fn = vt[0x24];
        if (!fn) return 0;
        const int result = reinterpret_cast<int(__fastcall*)(void*)>(fn)(item);
        return result > 0 && result < 100000 ? result : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

int ItemStack::getDamageValue() {
    const uintptr_t fn = SigManager::GetInstance().ItemStackBase_getDamageValue;
    if (fn) {
        const int value = callDamageValue(this, fn);
        if (value >= 0 && value < 100000) return value;
    }
    const int maximum = getMaxDamage();
    const int aux = readI16(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) + 0x20));
    return maximum > 0 && aux > 0 && aux <= maximum ? aux : 0;
}

int ItemStack::getMaxDamage() {
    return callMaxDamage(resolveItem(this));
}

int ItemStack::getRemainingDamage() {
    const int maximum = getMaxDamage();
    return maximum > 0 ? std::clamp(maximum - getDamageValue(), 0, maximum) : 0;
}

static int markerSlot(void** vt, uintptr_t marker) {
    __try {
        for (int slot = 0; slot < 1024; ++slot)
            if (reinterpret_cast<uintptr_t>(vt[slot]) == marker) return slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

static bool checkController(void* ctrl, uintptr_t marker,
                            ContainerScreenController::RejectReason* reason,
                            ContainerScreenController::CscProbe* probe) {
    using Reason = ContainerScreenController::RejectReason;
    const auto reject = [reason](Reason value) {
        if (reason) *reason = value;
        return false;
    };
    if (probe) probe->ctrl = ctrl;
    if (!ctrl) return reject(Reason::kNullController);
    if (!marker) return reject(Reason::kMarkerUnresolved);
    if (!sdk_safe_ptr(ctrl, ::Layout::Csc::MinSize)) return reject(Reason::kNotReadable);

    __try {
        void** vt = *reinterpret_cast<void***>(ctrl);
        if (probe) probe->vtable = vt;
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * 64)) return reject(Reason::kNotReadable);
        const int slot = markerSlot(vt, marker);
        if (probe) {
            probe->markerSlot = slot;
            if (sdk_safe_ptr(vt, sizeof(void*) * (::Layout::Csc::TypeVIndex + 1)))
                probe->vt172 = reinterpret_cast<uintptr_t>(vt[::Layout::Csc::TypeVIndex]);
        }
        if (slot < 0) return reject(Reason::kVtableMarkerMismatch);
        if (!vt[39]) return reject(Reason::kAutoPlaceGateNull);
        void* model = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ctrl) + ::Layout::Csc::SlotModel);
        if (!model || !sdk_safe_ptr(model, sizeof(void*))) return reject(Reason::kSlotModelNull);
        void** modelVt = *reinterpret_cast<void***>(model);
        if (!modelVt || !sdk_safe_ptr(modelVt, sizeof(void*) * 9) || !modelVt[8])
            return reject(Reason::kSlotModelVtableNull);
        if (reason) *reason = Reason::kAccepted;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return reject(Reason::kNotReadable); }
}

bool ContainerScreenController::isContainerScreenController(void* ctrl) {
    return checkController(ctrl, g_ContainerScreenController_getSelectedSlotInfo, nullptr, nullptr);
}

bool ContainerScreenController::isContainerScreenController(void* ctrl, RejectReason* reason, CscProbe* probe) {
    return checkController(ctrl, g_ContainerScreenController_getSelectedSlotInfo, reason, probe);
}

const char* ContainerScreenController::rejectReasonText(RejectReason reason) {
    switch (reason) {
    case RejectReason::kAccepted: return "accepted";
    case RejectReason::kNullController: return "controller is null";
    case RejectReason::kNotReadable: return "controller is unreadable";
    case RejectReason::kMarkerUnresolved: return "controller marker is unresolved";
    case RejectReason::kVtableMarkerMismatch: return "not a container screen controller";
    case RejectReason::kAutoPlaceGateNull: return "auto-place gate is null";
    case RejectReason::kSlotModelNull: return "slot model is null";
    case RejectReason::kSlotModelVtableNull: return "slot model vtable is null";
    }
    return "unknown";
}

void* ContainerScreenController::getSlotModel() {
    if (!sdk_safe_ptr(this, ::Layout::Csc::MinSize)) return nullptr;
    void* model = readPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) + ::Layout::Csc::SlotModel));
    return model && sdk_safe_ptr(model, sizeof(void*)) ? model : nullptr;
}

static void* callSlotInfo(void* self, void* fn, const std::string* name, int slot) {
    if (!self || !fn || !name) return nullptr;
    __try {
        void* result = reinterpret_cast<void*(__fastcall*)(void*, const std::string&, int)>(fn)(self, *name, slot);
        return result && sdk_safe_ptr(result, 0x40) ? result : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void* ContainerScreenController::getSlotInfo(const std::string& name, int slot) {
    if (slot < 0 || slot > 53 || !g_ContainerScreenController_getSlotInfo ||
        !sdk_safe_ptr(reinterpret_cast<void*>(g_ContainerScreenController_getSlotInfo), 16) ||
        !isContainerScreenController(this)) return nullptr;
    return callSlotInfo(this, reinterpret_cast<void*>(g_ContainerScreenController_getSlotInfo), &name, slot);
}

static bool callAutoPlace(void* self, void* fn, const std::string* name, int slot, void* slotInfo) {
    if (!self || !fn || !name) return false;
    __try {
        return reinterpret_cast<unsigned char(__fastcall*)(void*, const std::string&, int, void*)>(fn)(self, *name, slot, slotInfo) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ContainerScreenController::tryAutoPlace(const std::string& name, int slot, void* slotInfo) {
    if (slot < 0 || slot > 53 || !g_ContainerScreenController_tryAutoPlace ||
        !sdk_safe_ptr(reinterpret_cast<void*>(g_ContainerScreenController_tryAutoPlace), 16) ||
        !isContainerScreenController(this)) return false;
    return callAutoPlace(this, reinterpret_cast<void*>(g_ContainerScreenController_tryAutoPlace), &name, slot, slotInfo);
}

static void callHandleAutoPlace(void* self, void* fn, const std::string* name, int slot) {
    if (!self || !fn || !name) return;
    __try { reinterpret_cast<void(__fastcall*)(void*, uintptr_t, const std::string&, int)>(fn)(self, 0x7FFFFFFF, *name, slot); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void* callItemStack(void* self, void* fn, const std::string* name, int slot) {
    if (!self || !fn || !name) return nullptr;
    __try {
        void* result = reinterpret_cast<void*(__fastcall*)(void*, const std::string&, int)>(fn)(self, *name, slot);
        return result && sdk_safe_ptr(result, 0x80) ? result : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void* readVirtual(void* self, int slot) {
    if (!self || slot < 0) return nullptr;
    void** vt = *reinterpret_cast<void***>(self);
    if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * (slot + 1))) return nullptr;
    return vt[slot];
}

void ContainerScreenController::handleAutoPlace(const std::string& name, int slot) {
    if (slot < 0 || slot > 53 || !g_ContainerScreenController_handleAutoPlace ||
        !sdk_safe_ptr(reinterpret_cast<void*>(g_ContainerScreenController_handleAutoPlace), 16) ||
        !isContainerScreenController(this)) return;
    callHandleAutoPlace(this, reinterpret_cast<void*>(g_ContainerScreenController_handleAutoPlace), &name, slot);
}

ItemStack* ContainerScreenController::getItemStack(const std::string& name, int slot) {
    if (slot < 0 || slot > 53 || !g_ContainerScreenController_getItemStack ||
        !sdk_safe_ptr(reinterpret_cast<void*>(g_ContainerScreenController_getItemStack), 16) ||
        !isContainerScreenController(this)) return nullptr;
    return static_cast<ItemStack*>(callItemStack(this,
        reinterpret_cast<void*>(g_ContainerScreenController_getItemStack), &name, slot));
}

void ContainerScreenController::tryExit() {
    if (!g_ContainerScreenController_tryExit || !isContainerScreenController(this)) return;
    __try { reinterpret_cast<void(__fastcall*)(void*)>(g_ContainerScreenController_tryExit)(this); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int ContainerManagerModel::sGetSlotVIndex = -1;

ItemStack* ContainerManagerModel::getSlot(int slot) {
    if (slot < 0 || slot > 53 || sGetSlotVIndex < 0 || !sdk_safe_ptr(this, sizeof(void*))) return nullptr;
    void* fn = readVirtual(this, sGetSlotVIndex);
    if (!fn || !sdk_safe_ptr(fn, 16)) return nullptr;
    __try {
        ItemStack* item = reinterpret_cast<ItemStack*(__fastcall*)(void*, int)>(fn)(this, slot);
        return item && sdk_safe_ptr(item, 0x80) ? item : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

}
