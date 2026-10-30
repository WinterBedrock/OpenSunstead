// Storage for resolved function / global pointers consumed by the SDK headers.
// Filled once after SigManager::Initialize via SDKRuntime::Publish().
// Field offsets live in OffsetManager — not here.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <Windows.h>

#include "../Memory/SigManager.h"
#include "../Core/Logger.h"
#include "SDKRuntime.h"
#include "Client/GameMode.h"

uintptr_t g_PlatformGameCoreAddr = 0;
uintptr_t g_RenderMaterialGroupAddr = 0;
uintptr_t g_entt_assure = 0;
uintptr_t g_entt_assure_actorDataFlag = 0;
uintptr_t g_MoveInputComponent_get = 0;
uintptr_t g_LocalPlayer_applyTurnDelta = 0;
uintptr_t g_mouseDeviceAddr = 0;
uintptr_t g_MouseDevice_feed = 0;
// Synthetic right-click mailbox — see Packet.h requestRightClick().
// (Inside namespace SDK to match the extern declaration in Packet.h.)
namespace SDK { std::atomic<int> g_pendingRightClicks{0}; }
uintptr_t g_GuiData_displayClientMessage = 0;
uintptr_t g_ClientInstance_grabCursor = 0;
uintptr_t g_ClientInstance_releaseCursor = 0;
uintptr_t g_MinecraftPackets_createPacket = 0;
uintptr_t g_RakNetInstance_ctor = 0;
// TextPacket body builder (sub_143559070 on 1.26.50.04) — see Client/ChatSender.h.
uintptr_t g_TextPacket_buildChat = 0;
// Declared/defined in Inventory.cpp — published here from SigManager.
extern uintptr_t g_ContainerScreenController_handleAutoPlace;
extern uintptr_t g_ContainerScreenController_getSelectedSlotInfo;
extern uintptr_t g_ContainerScreenController_getItemStack;
extern uintptr_t g_ItemStackNetManager_requestDrop;
extern uintptr_t g_ContainerScreenController_tryAutoPlace;
extern uintptr_t g_ContainerScreenController_getSlotInfo;
uintptr_t g_Actor_setImmobile = 0;
// Vanilla break-progress-per-tick for the held item vs a given Block.
uintptr_t g_GameMode_getDestroyRate = 0;
// PlaceDelay master switch — GameMode.h reads this to zero the engine's
// between-placements cooldown stamp (Layout::GameMode::BuildCooldownLastNs)
// before every buildBlock. Default off so vanilla pacing is untouched.
bool SDK::GameMode::g_GameMode_SkipBuildCooldown = false;
// Misnomer: the "GameMode::attack" sig resolves Player::attack (0x142FB8DB0),
// not GameMode::attack (vtable-15 thunk 0x142D2D020). Only the combat-hit hook
// uses it; aura/triggerbot dispatch GameMode vtable slot 15 and must NOT verify
// against this address.
uintptr_t g_GameMode_attack = 0;
// Item-icon render path (see Client/ItemIcon.cpp).
uintptr_t g_ItemStackBase_ctorFromBlock = 0;
uintptr_t g_ItemStackBase_dtor = 0;
uintptr_t g_ItemStack_vtable = 0;
uintptr_t g_BaseActorRenderContext_ctor = 0;
uintptr_t g_BaseActorRenderContext_dtor = 0;
uintptr_t g_ItemRenderer_renderGuiItemNew = 0;
uintptr_t g_ItemRenderer_flushGuiItem = 0;
uintptr_t g_Tessellator_begin = 0;

uintptr_t g_Tessellator_vertex = 0;
uintptr_t g_Tessellator_color = 0;
uintptr_t g_MeshHelpers_renderMeshImmediately = 0;
uintptr_t g_Level_getRuntimeActorList = 0;
uintptr_t g_Actor_getUniqueID = 0;

namespace SDKRuntime {

void Publish() {
    auto& sig = SigManager::GetInstance();

    g_PlatformGameCoreAddr              = sig.Platform_GameCore;
    g_RenderMaterialGroupAddr           = sig.RenderMaterialGroup;
    g_Tessellator_begin                 = sig.Tessellator_begin;
    g_Tessellator_vertex                = sig.Tessellator_vertex;
    g_Tessellator_color                 = sig.Tessellator_color;
    g_MeshHelpers_renderMeshImmediately = sig.MeshHelpers_renderMeshImmediately;
    g_Level_getRuntimeActorList         = sig.Level_getRuntimeActorList;
    g_entt_assure                       = sig.entt_assure;
    g_entt_assure_actorDataFlag         = sig.entt_assure_actorDataFlag;
    g_MoveInputComponent_get            = sig.MoveInputComponent_get;
    g_mouseDeviceAddr                   = sig.mouseDeviceAddr;
    g_MouseDevice_feed                  = sig.MouseDevice_feed;
    g_LocalPlayer_applyTurnDelta        = sig.LocalPlayer_applyTurnDelta;
    g_GuiData_displayClientMessage      = sig.GuiData_displayClientMessage;
    g_MinecraftPackets_createPacket     = sig.MinecraftPackets_createPacket;
    g_RakNetInstance_ctor               = sig.RakNetInstance_ctor;
    g_TextPacket_buildChat              = sig.TextPacket_buildChat;
    g_Actor_getUniqueID                   = sig.Actor_getUniqueID;
    g_ContainerScreenController_handleAutoPlace = sig.ContainerScreenController_handleAutoPlace;
    g_ContainerScreenController_getSelectedSlotInfo = sig.ContainerScreenController_getSelectedSlotInfo;
    g_ContainerScreenController_getItemStack = sig.ContainerScreenController_getItemStack;
    g_ItemStackNetManager_requestDrop = sig.ItemStackNetManager_requestDrop;
    g_ContainerScreenController_tryAutoPlace = sig.ContainerScreenController_tryAutoPlace;
    g_ContainerScreenController_getSlotInfo = sig.ContainerScreenController_getSlotInfo;
    g_Actor_setImmobile                 = sig.Actor_setImmobile;
    g_GameMode_getDestroyRate           = sig.GameMode_getDestroyRate;
    g_GameMode_attack                   = sig.GameMode_attack;
    g_ItemStackBase_ctorFromBlock       = sig.ItemStackBase_ctorFromBlock;
    g_ItemStackBase_dtor                = sig.ItemStackBase_dtor;
    g_ItemStack_vtable                  = sig.ItemStack_vtable;
    g_BaseActorRenderContext_ctor       = sig.BaseActorRenderContext_ctor;
    g_BaseActorRenderContext_dtor       = sig.BaseActorRenderContext_dtor;
    g_ItemRenderer_renderGuiItemNew     = sig.ItemRenderer_renderGuiItemNew;
    g_ItemRenderer_flushGuiItem         = sig.ItemRenderer_flushGuiItem;
    if (g_ContainerScreenController_handleAutoPlace) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "ContainerScreenController::handleAutoPlace @ 0x%llX",
            (unsigned long long)g_ContainerScreenController_handleAutoPlace);
        LOG_INFO(buf, "SDK");
    } else {
        LOG_WARN("ContainerScreenController::handleAutoPlace NOT found — ChestStealer disabled", "SDK");
    }
    // Without the vtable[172] marker we cannot prove a controller is really a
    // ContainerScreenController, and handleAutoPlace's CFG-guarded indirect call
    // would __fastfail (uncatchable) on a mismatched object. Refuse to steal.
    if (!g_ContainerScreenController_getSelectedSlotInfo)
        LOG_WARN("CSC vtable marker NOT found — ChestStealer disabled (cannot verify controller type)", "SDK");
    if (!g_Actor_setImmobile)
        LOG_WARN("Actor::setImmobile NOT found — AntiImmobile disabled", "SDK");
    if (!g_GameMode_getDestroyRate)
        LOG_WARN("GameMode::getDestroyRate NOT found — OreMiner falls back to the Destroy Speed setting", "SDK");
    // Only the combat-hit hook consumes this; the aura/triggerbot go through the
    // GameMode vtable and do not depend on it.
    if (!g_GameMode_attack)
        LOG_WARN("Player::attack NOT found — the combat-hit signal will not fire", "SDK");
    if (!g_ItemRenderer_renderGuiItemNew || !g_ItemStackBase_ctorFromBlock ||
        !g_ItemStackBase_dtor || !g_BaseActorRenderContext_ctor)
        LOG_WARN("Item icon render path incomplete — BlockInfo texture disabled", "SDK");
    g_ClientInstance_grabCursor         = sig.ClientInstance_grabCursor;
    g_ClientInstance_releaseCursor      = sig.ClientInstance_releaseCursor;
    if (!g_TextPacket_buildChat)
        LOG_WARN("TextPacket::buildChat NOT found — Chat module sends nothing", "SDK");
}

} // namespace SDKRuntime
