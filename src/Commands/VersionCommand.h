#pragma once
#include "Command.h"
#include "../Utils/MCSDK.h"
#include "../Core/UpdateChecker.h"

class VersionCommand : public Command {
public:
    VersionCommand() : Command("version", "Shows client version", { "ver", "about" }) {}
    void Execute(const std::vector<std::string>& args) override {
        const std::string& ver = Sunstead::ClientVersion;
        std::string supported = Sunstead::GetSupportedVersionsString();
        CommandManager::GetInstance().PrintToChat("The §eSunstead §rProject §6" + ver + " §7(Supports " + supported + ")§r. \n§0- §rauthors: eventex213, sniffz0ne, CrieXD1337.");

        UpdateChecker& upd = UpdateChecker::GetInstance();
        if (upd.IsUpdateAvailable())
            CommandManager::GetInstance().PrintToChat("§eUpdate available: §6v" + upd.GetLatestVersion() +
                " §7(you have " + ver + ")§r — download from §fGitHub Releases §7or the §fDiscord§7.");
    }
};