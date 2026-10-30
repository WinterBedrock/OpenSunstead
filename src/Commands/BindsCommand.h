#pragma once
#include "Command.h"
#include "CommandManager.h"
#include "../Modules/ModuleManager.h"
#include "../Core/Config.h"
#include "../Utils/MiscUtils/KeyboardUtils.hpp"
#include <string>

// jst list of modules binds
class BindsCommand : public Command {
public:
    BindsCommand() : Command("binds", "List all module keybinds") {}

    void Execute(const std::vector<std::string>& args) override {
        (void)args;

        auto& cm = CommandManager::GetInstance();
        auto& mm = ModuleManager::GetInstance();

        int total = 0;
        for (int cat = 0; cat < 5; ++cat) {
            for (Module* mod : mm.GetModulesInCategory((Category)cat)) {
                if (!mod || mod->GetKeybind() == 0) continue;
                cm.PrintToChat("§7- §f" + mod->GetName() + "§7: §z" +
                               KeyboardUtils::GetKeyName(mod->GetKeybind()));
                ++total;
            }
        }

        std::string modName = "Ctrl";
        if (Config::UninjectMod == VK_SHIFT) modName = "Shift";
        else if (Config::UninjectMod == VK_MENU) modName = "Alt";
        else if (Config::UninjectMod == 0) modName.clear();
        const std::string modPart = modName.empty() ? "" : modName + " + ";
        cm.PrintToChat("§7- §fUninject§7: §z" + modPart +
                       KeyboardUtils::GetKeyName(Config::UninjectKey));

        if (total == 0)
            cm.PrintToChat("§7No module keybinds set, use §z" + cm.mPrefix +
                           "bind <module> <key>§7.");
    }
};
