#pragma once
#include "Command.h"
#include "CommandManager.h"
#include "../SDK/Client/ClientInstance.h"
#include <string>

class IpCommand : public Command {
public:
    IpCommand() : Command("ip", "Shows current server IP, DNS and port") {}

    void Execute(const std::vector<std::string>& /*args*/) override {
        SDK::RakNetConnector* rnc = SDK::RakNetConnector::get();
        if (!rnc) {
            CommandManager::GetInstance().PrintToChat("§cNot connected to a server.");
            return;
        }

        std::string dns = rnc->getDns();
        std::string ip  = rnc->getIpAddress();
        uint16_t port   = rnc->getPort();

        // Prefer DNS hostname; fall back to numeric IP for the primary label.
        std::string host = !dns.empty() ? dns : ip;
        std::string numeric = !ip.empty() ? ip : dns;

        if (host.empty() && numeric.empty()) {
            CommandManager::GetInstance().PrintToChat("§cNot connected to a server.");
            return;
        }

        if (numeric.empty()) numeric = host;

        CommandManager::GetInstance().PrintToChat("IP: §e" + host + " §7(" + numeric + ")§r");
        CommandManager::GetInstance().PrintToChat("Port: " + std::to_string(port));
    }
};
