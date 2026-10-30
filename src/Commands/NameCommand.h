#pragma once
#include "Command.h"
#include "CommandManager.h"
#include "../Utils/MCSDK.h"
#include <string>

// NameCommand -- copies the player's CURRENT nick to the clipboard.
//   .name   (aliases: nick, nickname, username, ign)
// Read-only on purpose: the NameProtect fake-nick stays editable from its
// module card in the ClickGUI; the chat command just answers "what is my nick"
// by copying the live name the server sees.
// SEH + std::string can't share a frame (C2712): the guarded resolve returns
// the Actor*, the name read itself runs outside the __try (getRawName returns
// a std::string and is internally guarded — the same call TargetHUD makes on
// live actors every frame).
static SDK::Actor* sehResolveLocalPlayer() {
    __try {
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        if (!ci || !sdk_safe_ptr(ci, sizeof(void*))) return nullptr;
        SDK::Actor* lp = ci->getLocalPlayer();
        return (lp && sdk_safe_ptr(lp, 0x20)) ? lp : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

class NameCommand : public Command {
public:
    NameCommand() : Command("name", "Copies your current nick to the clipboard",
                            { "nick", "nickname", "username", "ign" }) {}

    void Execute(const std::vector<std::string>& args) override {
        (void)args;
        auto& cm = CommandManager::GetInstance();

        SDK::Actor* lp = sehResolveLocalPlayer();
        if (!lp) {
            cm.PrintToChat("§cNot in a world, join a world first.");
            return;
        }
        const std::string nick = lp->getRawName();
        if (nick.empty()) {
            cm.PrintToChat("§cCould not read your nick.");
            return;
        }

        if (CopyToClipboard(nick)) {
            cm.PrintToChat("§7Nick: §z" + nick + " §7copied to clipboard§r");
        } else {
            cm.PrintToChat("§7Nick: §z" + nick + " §c(clipboard copy failed)§r");
        }
    }

private:
    static bool CopyToClipboard(const std::string& text) {
        if (!OpenClipboard(nullptr)) return false;
        bool ok = false;
        if (EmptyClipboard()) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
            if (h) {
                char* p = (char*)GlobalLock(h);
                if (p) {
                    memcpy(p, text.c_str(), text.size() + 1);
                    GlobalUnlock(h);
                    ok = SetClipboardData(CF_TEXT, h) != nullptr;   // system owns h on success
                    if (!ok) GlobalFree(h);
                } else {
                    GlobalFree(h);
                }
            }
        }
        CloseClipboard();
        return ok;
    }
};
