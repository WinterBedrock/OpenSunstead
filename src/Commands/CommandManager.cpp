#include "CommandManager.h"
#include "../Utils/MCSDK.h"
#include "../Modules/ModuleManager.h"
#include "../Core/Sunstead.h"
#include "../Hooks/ColorCodeHook.h"
#include <sstream>
#include <algorithm>

#include "HelpCommand.h"
#include "VersionCommand.h"
#include "IpCommand.h"
#include "PrefixCommand.h"
#include "BindCommand.h"
#include "UnbindCommand.h"
#include "BindsCommand.h"
#include "ConfigCommand.h"
#include "NearCommand.h"
#include "NameCommand.h"
#include "GotoCommand.h"
#include "SeedCommand.h"
#include "PlayerCommand.h"

void CommandManager::Initialize() {
    mCommands.push_back(std::make_shared<HelpCommand>());
    mCommands.push_back(std::make_shared<VersionCommand>());
    mCommands.push_back(std::make_shared<IpCommand>());
    mCommands.push_back(std::make_shared<PrefixCommand>());
    mCommands.push_back(std::make_shared<BindCommand>());
    mCommands.push_back(std::make_shared<UnbindCommand>());
    mCommands.push_back(std::make_shared<BindsCommand>());
    mCommands.push_back(std::make_shared<ConfigCommand>());
    mCommands.push_back(std::make_shared<NearCommand>());
    mCommands.push_back(std::make_shared<NameCommand>());
    mCommands.push_back(std::make_shared<GotoCommand>());
    mCommands.push_back(std::make_shared<SeedCommand>());
    mCommands.push_back(std::make_shared<PlayerCommand>());
}


void CommandManager::ExecuteCommand(const std::string& message) {
    if (message.length() <= mPrefix.length()) return;
    
    std::string cmdLine = message.substr(mPrefix.length());
    std::stringstream ss(cmdLine);
    std::string cmdName;
    ss >> cmdName;

    std::transform(cmdName.begin(), cmdName.end(), cmdName.begin(), ::tolower);

    std::vector<std::string> args;
    std::string arg;
    while (ss >> arg) {
        args.push_back(arg);
    }

    for (const auto& cmd : mCommands) {
        bool match = (cmd->mName == cmdName);
        if (!match) {
            for (const auto& alias : cmd->mAliases) {
                if (alias == cmdName) {
                    match = true;
                    break;
                }
            }
        }
        if (match) {
            cmd->Execute(args);
            return;
        }
    }


    PrintToChat("§cUnknown command. Type §e" + mPrefix + "help §cfor list of commands.");
}

void CommandManager::PrintToChat(const std::string& message) {
    const std::string& clientName = Sunstead::ClientName;
    // ColorCodeHook::ThemeCode() is our own §z (client theme colour, animated)
    // and degrades to §f when the colour-code hook could not be installed.
    SDK::Chat::displayClientMessage(
        "§l§0[" + std::string(ColorCodeHook::ThemeCode()) + clientName + "§0] §r" + message);
}
