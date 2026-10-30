#pragma once
#include "Command.h"
#include "../Core/ConfigManager.h"
#include <filesystem>

class ConfigCommand : public Command {
public:
    ConfigCommand() : Command("config", "Saves or loads client settings config.", {"c", "cfg"}) {}

    void Execute(const std::vector<std::string>& args) override {
        if (args.empty()) {
            CommandManager::GetInstance().PrintToChat("§cUsage: " + CommandManager::GetInstance().mPrefix + "config <save/load/list> [name]");
            return;
        }

        std::string sub = args[0];
        std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);

        if (sub == "save" || sub == "s") {
            std::string name = (args.size() > 1) ? args[1] : "default";
            if (ConfigManager::Save(name)) {
                CommandManager::GetInstance().PrintToChat("§fSuccessfully saved config: §z" + name);
                Notifications::AddNotification("Saved config " + name);
            } else {
                CommandManager::GetInstance().PrintToChat("§cFailed to save config: §e" + name);
            }
        }
        else if (sub == "load" || sub == "l") {
            std::string name = (args.size() > 1) ? args[1] : "default";
            if (ConfigManager::Load(name)) {
                CommandManager::GetInstance().PrintToChat("§fSuccessfully loaded config: §z" + name);
                Notifications::AddNotification("Loaded config " + name);
            } else {
                CommandManager::GetInstance().PrintToChat("§cFailed to load config: §e" + name);
            }
        }
        else if (sub == "list") {
            auto list = ConfigManager::ListConfigs();
            if (list.empty()) {
                CommandManager::GetInstance().PrintToChat("§7No configs found in Sunstead/configs/");
            } else {
                CommandManager::GetInstance().PrintToChat("§fAvailable configurations:");
                for (const auto& conf : list) {
                    CommandManager::GetInstance().PrintToChat("§7 - §z" + conf);
                }
            }
        }
        else {
            CommandManager::GetInstance().PrintToChat("§cUnknown subcommand. Use save, load, or list.");
        }
    }
};
