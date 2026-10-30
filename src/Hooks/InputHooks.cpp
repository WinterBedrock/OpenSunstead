// Cursor / raw mouse input hooks — block the game from grabbing the cursor
// or feeding mouse input while a client GUI (ClickGUI, custom chat) is open.
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../SDK/Client/ClientInstance.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Visual/Chat.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "MinHook.h"
#include <string>

bool g_IsCursorGrabbed = true;

namespace {
    using namespace SDK;

    typedef void(__fastcall* clientInstanceGrabCursor_t)(SDK::ClientInstance*);
    clientInstanceGrabCursor_t oClientInstanceGrabCursor = nullptr;
    clientInstanceGrabCursor_t oClientInstanceReleaseCursor = nullptr;

    typedef void(__fastcall* gameCoreHandleMouseInput_t)(void*);
    gameCoreHandleMouseInput_t oGameCoreHandleMouseInput = nullptr;

    bool shouldBlockClientCursorGrab() {
        Module* clickGui = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
        if (::Chat::IsOpen()) return true;
        return (clickGui && clickGui->IsEnabled());
    }

    void __fastcall hkClientInstanceGrabCursor(SDK::ClientInstance* cInst) {
        g_IsCursorGrabbed = true;
        if (shouldBlockClientCursorGrab()) {
            g_IsCursorGrabbed = false;
            static bool logged = false;
            if (!logged) {
                LOG_INFO("ClientInstance::grabCursor blocked while GUI is open", "GameHooks");
                logged = true;
            }
            cInst->releaseCursor();
            return;
        }
        oClientInstanceGrabCursor(cInst);
    }

    void __fastcall hkClientInstanceReleaseCursor(SDK::ClientInstance* cInst) {
        static int fc = 0;
        if (++fc % 120 == 1) LOG_INFO("releaseCursor called (frame " + std::to_string(fc) + ")", "GameHooks");
        g_IsCursorGrabbed = false;
        oClientInstanceReleaseCursor(cInst);
    }

    void __fastcall hkGameCoreHandleMouseInput(void* a1) {
        if (shouldBlockClientCursorGrab()) {
            SDK::MouseDevice* mouse = SDK::MouseDevice::get();
            if (mouse) {
                mouse->inputs.clear();
                for (int i = 0; i < 7; ++i) mouse->buttonStates[i] = false;
                mouse->dx = 0;
                mouse->dy = 0;
            }
            return;
        }
        if (oGameCoreHandleMouseInput) oGameCoreHandleMouseInput(a1);
    }
}

void InstallInputHooks() {
    auto& sig = SigManager::GetInstance();

    uintptr_t grabCursorAddr = sig.ClientInstance_grabCursor;
    if (grabCursorAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(grabCursorAddr), &hkClientInstanceGrabCursor,
                          reinterpret_cast<LPVOID*>(&oClientInstanceGrabCursor)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(grabCursorAddr));
            LOG_INFO("ClientInstance::grabCursor hooked", "GameHooks");
        }
    }

    uintptr_t releaseCursorAddr = sig.ClientInstance_releaseCursor;
    if (releaseCursorAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(releaseCursorAddr), &hkClientInstanceReleaseCursor,
                          reinterpret_cast<LPVOID*>(&oClientInstanceReleaseCursor)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(releaseCursorAddr));
            LOG_INFO("ClientInstance::releaseCursor hooked", "GameHooks");
        }
    }

    uintptr_t mouseInputAddr = sig.GameCore_handleMouseInput;
    if (mouseInputAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(mouseInputAddr), &hkGameCoreHandleMouseInput,
                          reinterpret_cast<LPVOID*>(&oGameCoreHandleMouseInput)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(mouseInputAddr));
            LOG_INFO("GameCore::handleMouseInput hooked", "GameHooks");
        }
    }
}
