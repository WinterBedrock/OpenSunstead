#pragma once
#include "Command.h"
#include "CommandManager.h"
#include "../Utils/MCSDK.h"
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

class NearCommand : public Command {
public:
    NearCommand() : Command("near", "Lists nearby entities and their details") {}

    void Execute(const std::vector<std::string>& args) override {
        SDK::ClientInstance* cInst = SDK::ClientInstance::get();
        if (!cInst) {
            CommandManager::GetInstance().PrintToChat("§cClient instance is not available.");
            return;
        }

        SDK::Actor* localPlayer = cInst->getLocalPlayer();
        if (!localPlayer) {
            CommandManager::GetInstance().PrintToChat("§cLocal player is not available.");
            return;
        }

        SDK::Level* level = cInst->getMinecraftGame() ? cInst->getMinecraftGame()->getPrimaryClientInstance()->getMinecraft()->getLevel() : nullptr;
        if (!level) {
            CommandManager::GetInstance().PrintToChat("§cLevel is not available.");
            return;
        }

        std::vector<SDK::Actor*> actors = level->getRuntimeActorList();
        std::vector<std::string> lines;
        lines.reserve(16);

        Vec3 selfPos = localPlayer->getPos();
        for (SDK::Actor* actor : actors) {
            if (!actor || actor == localPlayer) continue;
            Vec3 pos = actor->getPos();
            Vec3 delta = pos - selfPos;
            float distance = std::sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            if (distance > 32.0f) continue;

            std::ostringstream ss;
            ss << "§7" << actor->getRawName() << " §8@ §f" << std::fixed << std::setprecision(1)
               << pos.x << ", " << pos.y << ", " << pos.z
               << " §8(dist " << distance << ")";
            lines.push_back(ss.str());
        }

        if (lines.empty()) {
            CommandManager::GetInstance().PrintToChat("§7No nearby entities found.");
            return;
        }

        CommandManager::GetInstance().PrintToChat("§eNearby entities:");
        for (const std::string& line : lines) {
            CommandManager::GetInstance().PrintToChat(line);
        }
    }
};
