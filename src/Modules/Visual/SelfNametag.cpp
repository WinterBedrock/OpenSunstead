#include "SelfNametag.h"
#include "../../Memory/SigManager.h"
#include "../../Core/Logger.h"

void SelfNametag::OnEnable() {
    uintptr_t addr = SigManager::GetInstance().thirdPersonNametag;
    if (addr) {
        uintptr_t jzAddr = addr; // anchor points at the `jz skip-local-player` (74 ??); NOP it so our own nametag renders in 3rd person
        DWORD oldProtect;
        if (VirtualProtect(reinterpret_cast<LPVOID>(jzAddr), 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(originalBytes, reinterpret_cast<void*>(jzAddr), 2);
            bytesSaved = true;

            // NOP the 2 bytes
            *reinterpret_cast<unsigned char*>(jzAddr) = 0x90;
            *reinterpret_cast<unsigned char*>(jzAddr + 1) = 0x90;

            VirtualProtect(reinterpret_cast<LPVOID>(jzAddr), 2, oldProtect, &oldProtect);
            //LOG_INFO("Patched jump instruction", "SelfNametag");
        }
    }
}

void SelfNametag::OnDisable() {
    uintptr_t addr = SigManager::GetInstance().thirdPersonNametag;
    if (addr && bytesSaved) {
        uintptr_t jzAddr = addr; // anchor points at the `jz skip-local-player` (74 ??); NOP it so our own nametag renders in 3rd person
        DWORD oldProtect;
        if (VirtualProtect(reinterpret_cast<LPVOID>(jzAddr), 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(reinterpret_cast<void*>(jzAddr), originalBytes, 2);
            VirtualProtect(reinterpret_cast<LPVOID>(jzAddr), 2, oldProtect, &oldProtect);
            //LOG_INFO("Restored jump instruction", "SelfNametag");
        }
    }
}