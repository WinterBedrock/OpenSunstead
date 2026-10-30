#pragma once
#include "Command.h"
#include "CommandManager.h"
#include "../Modules/ModuleManager.h"
#include <algorithm>

class BindCommand : public Command {
public:
    BindCommand() : Command("bind", "Binds a module to a key", {"b"}) {}
    void Execute(const std::vector<std::string>& args) override {
        if (args.size() < 2) {
            CommandManager::GetInstance().PrintToChat("§cUsage: " + CommandManager::GetInstance().mPrefix + "bind <module> <key>");
            return;
        }

        std::string modName = args[0];
        std::string keyName = args[1];

        Module* mod = ModuleManager::GetInstance().GetModuleByName(modName);
        if (!mod) {
            CommandManager::GetInstance().PrintToChat("§cModule not found: " + modName);
            return;
        }

        std::transform(keyName.begin(), keyName.end(), keyName.begin(), ::toupper);

        int key = 0;
        if (keyName.length() == 1) {
            key = keyName[0];
        } else {
            if (keyName == "SPACE") key = VK_SPACE;
            else if (keyName == "SHIFT" || keyName == "LSHIFT") key = VK_LSHIFT;
            else if (keyName == "RSHIFT") key = VK_RSHIFT;
            else if (keyName == "CTRL" || keyName == "LCTRL") key = VK_LCONTROL;
            else if (keyName == "RCTRL") key = VK_RCONTROL;
            else if (keyName == "ALT" || keyName == "LALT") key = VK_LMENU;
            else if (keyName == "RALT") key = VK_RMENU;
            else if (keyName == "ENTER") key = VK_RETURN;
            else if (keyName == "TAB") key = VK_TAB;
            else if (keyName == "UP") key = VK_UP;
            else if (keyName == "DOWN") key = VK_DOWN;
            else if (keyName == "LEFT") key = VK_LEFT;
            else if (keyName == "RIGHT") key = VK_RIGHT;
            else if (keyName == "INSERT" || keyName == "INS") key = VK_INSERT;
        }

        if (key == 0) {
            CommandManager::GetInstance().PrintToChat("§cInvalid key: " + keyName);
            return;
        }

        mod->SetKeybind(key);
        CommandManager::GetInstance().PrintToChat("§7Bound module §z" + mod->GetName() + " §7to key §b" + keyName);
    }
};
