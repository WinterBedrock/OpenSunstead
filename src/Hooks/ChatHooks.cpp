// Chat hooks. Vanilla chat lines that start with the command prefix are
// swallowed here and rerouted to CommandManager; everything else keeps its
// path to the network. displayClientMessage feeds the custom Chat module's
// history.
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/Client/Chat.h"
#include "../Commands/CommandManager.h"
#include "../Modules/Visual/Chat.h"
#include "../Memory/OffsetManager.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "MinHook.h"
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace {
    // ChatScreenController::sendChatMessage (1.26.50+): one-arg method; the
    // typed line is the std::string member at this+Layout::ChatScreen::Text.
    typedef __int64(__fastcall* chatSendOneArg_t)(void* thisPtr);
    typedef unsigned char(__fastcall* ClientInstanceScreenModel_sendChatMessage_t)(void* self, void* textPtr);
    ClientInstanceScreenModel_sendChatMessage_t oClientInstanceScreenModelSendChatMessage = nullptr;

    // One-shot diagnostic (strips to nothing in release) for intercept issues.
    void logChatIntercept(const char* tag, const char* text, bool matched) {
#ifdef SUNSTEAD_LOGGING
        static int s_logged = 0;
        if (s_logged >= 8) return;
        ++s_logged;
        char b[300];
        std::snprintf(b, sizeof(b), "%s text=\"%s\" prefixMatch=%d oPtr=%p",
                      tag, text, (int)matched, (void*)SDK::Chat::g_oSendChatMessage);
        LOG_INFO(b, "Chat");
#else
        (void)tag; (void)text; (void)matched;
#endif
    }

    static bool sehCopyChatText(void* textPtr, char* out, size_t cap) {
        __try {
            const std::string* s = static_cast<const std::string*>(textPtr);
            const size_t n = s->size();
            if (n == 0 || n >= cap) { out[0] = '\0'; return false; }
            std::memcpy(out, s->data(), n);
            out[n] = '\0';
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out[0] = '\0';
            return false;
        }
    }

    // Commands must NOT run inside the sendChatMessage hook: the reply goes
    // through displayClientMessage, which re-enters the chat screen's message
    // append path while the submit is still on the stack and fast-fails inside
    // the ChatEvent string destructors (a heap-block validation abort, not a
    // C++ exception). Queue here, run on the next renderLevel frame instead.
    static std::mutex s_deferredCmdMutex;
    static std::deque<std::string> s_deferredCmds;

    static void queueChatCommand(const std::string& text) {
        std::lock_guard<std::mutex> lock(s_deferredCmdMutex);
        s_deferredCmds.push_back(text);
        while (s_deferredCmds.size() > 16) s_deferredCmds.pop_front();
        // Stamp it so the Chat module lets the reply into the vanilla chat.
        ::Chat::NoteVanillaCommand();
    }

    // No C++ object with a destructor may live in this frame — MSVC forbids
    // __try in functions needing unwinding — hence the const&.
    static int sehExecuteCommand(const std::string& text) {
        __try {
            CommandManager::GetInstance().ExecuteCommand(text);
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 1;
        }
    }

    // No local echo on a swallowed command line: echoing it duplicated the
    // just-typed ".X" into chat.
    __int64 __fastcall hkChatScreenController_sendChatMessage(void* thisPtr) {
        char text[256] = {0};
        const std::string& prefix = CommandManager::GetInstance().mPrefix;
        const bool ok = thisPtr &&
            sehCopyChatText((void*)((uintptr_t)thisPtr + Layout::ChatScreen::Text),
                            text, sizeof(text)) &&
            !prefix.empty() && std::strncmp(text, prefix.c_str(), prefix.size()) == 0;
        logChatIntercept("ChatScreenController", text, ok);
        if (ok) {
            queueChatCommand(text);
            return 0;   // swallow: a client command never reaches the network
        }
        return SDK::Chat::g_oSendChatMessage
            ? SDK::Chat::g_oSendChatMessage(thisPtr)
            : 0;
    }

    unsigned char __fastcall hkClientInstanceScreenModel_sendChatMessage(void* thisPtr, void* textPtr) {
        char text[256] = {0};
        const std::string& prefix = CommandManager::GetInstance().mPrefix;
        const bool ok = textPtr && sehCopyChatText(textPtr, text, sizeof(text)) &&
            !prefix.empty() && std::strncmp(text, prefix.c_str(), prefix.size()) == 0;
        logChatIntercept("ScreenModel", text, ok);
        if (ok) {
            queueChatCommand(text);
            return 0;   // "not handled" — the message never reaches the network
        }
        return oClientInstanceScreenModelSendChatMessage
            ? oClientInstanceScreenModelSendChatMessage(thisPtr, textPtr)
            : 0;
    }

    typedef void(__fastcall* GuiData_displayClientMessage_t)(void*, const std::string&, void*, bool);
    GuiData_displayClientMessage_t oGuiDataDisplayClientMessage = nullptr;

    static int sehCallDisplayClientMessage(GuiData_displayClientMessage_t fn, void* gd,
                                           const std::string& msg, void* opt, bool flag) {
        __try {
            fn(gd, msg, opt, flag);
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 1;
        }
    }

    void __fastcall hkGuiDataDisplayClientMessage(void* gd, const std::string& msg, void* opt, bool flag) {
        if (::Chat::RecordMessage(msg)) return;   // Chat module owns the chat now
        if (!oGuiDataDisplayClientMessage) return;
        {
            static int s_traced = 0;
            if (s_traced < 4) {
                ++s_traced;
                char b[256];
                std::snprintf(b, sizeof(b), "displayClientMessage fwd gd=%p size=%zu flag=%d",
                              gd, msg.size(), (int)flag);
                LOG_INFO(b, "Chat");
            }
        }
        // A fault inside game code here used to kill the process: the game's
        // own ChatEvent leaks instead and one chat line is lost.
        if (sehCallDisplayClientMessage(oGuiDataDisplayClientMessage, gd, msg, opt, flag) != 0)
            LOG_WARN("GuiData::displayClientMessage faulted (suppressed, chat line lost)", "GameHooks");
    }
}

void gh::runDeferredChatCommands() {
    std::deque<std::string> taken;
    {
        std::lock_guard<std::mutex> lock(s_deferredCmdMutex);
        taken.swap(s_deferredCmds);
    }
    for (const auto& cmd : taken) {
        if (sehExecuteCommand(cmd) != 0) {
            LOG_WARN("deferred chat command threw (suppressed)", "GameHooks");
            break;   // a fault leaves the command system suspect; drop the rest
        }
    }
}

void GameHooks::SendChatMessage(const std::string& text) {
    SDK::Chat::sendMessage(text);
}

void InstallChatHooks() {
    uintptr_t sendChatMessageAddr = SigManager::GetInstance().ChatScreenController_sendChatMessage;
    if (sendChatMessageAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(sendChatMessageAddr), &hkChatScreenController_sendChatMessage,
                          reinterpret_cast<LPVOID*>(&SDK::Chat::g_oSendChatMessage)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(sendChatMessageAddr));
            LOG_INFO("chat hook installed @ " + std::to_string(sendChatMessageAddr), "GameHooks");
        } else {
            LOG_ERROR("chat hook CreateHook FAILED @ " + std::to_string(sendChatMessageAddr), "GameHooks");
        }
    } else {
        LOG_WARN("chat signature not found", "GameHooks");
    }

    uintptr_t screenModelSendAddr = SigManager::GetInstance().ClientInstanceScreenModel_sendChatMessage;
    if (screenModelSendAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(screenModelSendAddr), &hkClientInstanceScreenModel_sendChatMessage,
                          reinterpret_cast<LPVOID*>(&oClientInstanceScreenModelSendChatMessage)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(screenModelSendAddr));
            LOG_INFO("ClientInstanceScreenModel::sendChatMessage hooked (vanilla-chat commands) @ " +
                     std::to_string(screenModelSendAddr), "GameHooks");
        } else {
            LOG_ERROR("ClientInstanceScreenModel::sendChatMessage CreateHook FAILED @ " +
                      std::to_string(screenModelSendAddr), "GameHooks");
        }
    } else {
        LOG_WARN("ClientInstanceScreenModel::sendChatMessage signature not found; vanilla-chat commands inactive", "GameHooks");
    }

    uintptr_t displayMsgAddr = SigManager::GetInstance().GuiData_displayClientMessage;
    if (displayMsgAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(displayMsgAddr), &hkGuiDataDisplayClientMessage,
                          reinterpret_cast<LPVOID*>(&oGuiDataDisplayClientMessage)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(displayMsgAddr));
            LOG_INFO("GuiData::displayClientMessage hooked (Chat history)", "GameHooks");
        } else {
            LOG_ERROR("FAILED to hook GuiData::displayClientMessage", "GameHooks");
        }
    } else {
        LOG_WARN("GuiData::displayClientMessage signature not found; Chat history inactive", "GameHooks");
    }
}
