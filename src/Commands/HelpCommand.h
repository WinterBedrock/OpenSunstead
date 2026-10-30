#pragma once
#include "Command.h"
#include "CommandManager.h"

class HelpCommand : public Command {
public:
    HelpCommand() : Command("help", "Lists all available commands") {}
    void Execute(const std::vector<std::string>& args) override {
        CommandManager::GetInstance().PrintToChat("§eAvailable commands:");
        for (const auto& cmd : CommandManager::GetInstance().mCommands) {
            CommandManager::GetInstance().PrintToChat("§b" + CommandManager::GetInstance().mPrefix + cmd->mName + " §7- " + cmd->mDescription);
        }
    }
};
