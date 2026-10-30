#pragma once
#include "Command.h"
#include "CommandManager.h"

class PrefixCommand : public Command {
public:
    PrefixCommand() : Command("prefix", "Changes the command prefix") {}
    void Execute(const std::vector<std::string>& args) override {
        if (args.empty()) {
            CommandManager::GetInstance().PrintToChat("§cUsage: " + CommandManager::GetInstance().mPrefix + "prefix <character>");
            return;
        }
        CommandManager::GetInstance().mPrefix = args[0];
        CommandManager::GetInstance().PrintToChat("Command prefix §7changed§r to: §e" + args[0]);
    }
};
