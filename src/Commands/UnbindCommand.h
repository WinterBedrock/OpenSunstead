#pragma once
#include "Command.h"
#include "CommandManager.h"
#include "../Modules/ModuleManager.h"

class UnbindCommand : public Command {
public:
    UnbindCommand() : Command("unbind", "Unbinds a module from its key") {}
    void Execute(const std::vector<std::string>& args) override {
        if (args.empty()) {
            CommandManager::GetInstance().PrintToChat("§cUsage: " + CommandManager::GetInstance().mPrefix + "unbind <module|all>");
            return;
        }

        std::string modName = args[0];
        if (modName == "all") {
            int unbound = 0;
            for (Module* mod : ModuleManager::GetInstance().GetModules()) {
                if (mod->GetKeybind() != 0) {
                    mod->SetKeybind(0);
                    ++unbound;
                }
            }

            CommandManager::GetInstance().PrintToChat("§7Unbound all modules (§z" + std::to_string(unbound) + " binds)");
            return;
        }

        Module* mod = ModuleManager::GetInstance().GetModuleByName(modName);
        if (!mod) {
            CommandManager::GetInstance().PrintToChat("§cModule not found: " + modName);
            return;
        }

        mod->SetKeybind(0);
        CommandManager::GetInstance().PrintToChat("§7Unbound module: §z" + mod->GetName());
    }
};
