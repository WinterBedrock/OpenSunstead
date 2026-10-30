#pragma once
// SDK::Chat — client chat I/O (server-bound send + local display). Lives in
// the SDK so modules can use chat without depending on GameHooks, which only
// resolves the GuiData/createPacket signatures, hooks
// ChatScreenController::sendChatMessage and publishes the fallback controller
// pointer used when the packet path is unavailable.

#include "ChatSender.h"
#include "ClientInstance.h"
#include <string>
#include <cstdio>

// Populated by GameHooks::Initialize from SigManager.
extern uintptr_t g_GuiData_displayClientMessage;
extern uintptr_t g_MinecraftPackets_createPacket;

namespace SDK {
namespace Chat {

// ChatScreenController::sendChatMessage is a one-arg method on 1.26.50.04+:
// the typed line is a std::string MEMBER of the controller (this+0xD60), not a
// parameter — see the hook in GameHooks.cpp and Layout::ChatScreen::Text.
using SendChatControllerFn = __int64 (__fastcall*)(void* thisPtr);
inline SendChatControllerFn g_oSendChatMessage = nullptr;

// Send a chat message AS the local player (TextPacket -> PacketSender).
// Returns true if the packet was dispatched.
inline bool sendMessage(const std::string& text) {
    if (text.empty()) return false;
    ClientInstance* ci = ClientInstance::get();
    if (!ci) return false;
    void* ps = ci->getPacketSender();
    return ps && chat::sendChat((void*)ci, ps, text);
}

// Send a server COMMAND ("/help ...") as a CommandRequestPacket (ID 77).
// See ChatSender.h for the IDA-verified packet layout.
inline bool sendCommand(const std::string& command) {
    if (command.empty()) return false;
    ClientInstance* ci = ClientInstance::get();
    if (!ci) return false;
    void* ps = ci->getPacketSender();
    return ps && chat::sendCommand((void*)ci, ps, command);
}

// Local-only client message (GuiData::displayClientMessage). Not sent to the
// server — used for command output and similar UI text.
inline bool displayClientMessage(const std::string& text) {
    if (!g_GuiData_displayClientMessage) return false;
    ClientInstance* ci = ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) return false;
    GuiData* gd = ci->getGuiData();
    if (!gd || !sdk_safe_ptr(gd, 0x40)) return false;
    // One-shot diagnostic: log how GuiData was resolved (vtable vs fields).
    {
        static int s_checked = 0;
        if (s_checked < 4) {
            ++s_checked;
            GuiData* via650 = *(GuiData**)((uintptr_t)ci + 0x650);
            GuiData* via648 = *(GuiData**)((uintptr_t)ci + 0x648);
            char b[256];
            std::snprintf(b, sizeof(b), "GuiData: vt218=%p CI+0x650=%p CI+0x648=%p",
                          (void*)gd, (void*)via650, (void*)via648);
            if (gd == via650 && gd != via648) LOG_INFO(b, "SDK");
            else LOG_WARN(b, "SDK");
        }
    }
    gd->displayClientMessage(text);
    return true;
}

} // namespace Chat
} // namespace SDK
