#pragma once
#include "Module.h"
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>

class ModuleManager {
public:
    static ModuleManager& GetInstance() {
        static ModuleManager instance;
        return instance;
    }

    void Initialize();
    void OnTick();
    void OnUpdate();
    void OnRender();
    void OnKey(int key);
    void PollHoldBinds();
    void OnPacketSend(class SDK::Packet* packet, int packetId, bool& cancel);

    const std::vector<Module*>& GetModules() const;
    std::vector<Module*> GetModulesInCategory(Category category) const;
    Module* GetModuleByName(const std::string& name) const;

private:
    ModuleManager() = default;
    std::vector<std::unique_ptr<Module>> modules;
    std::vector<Module*> moduleViews;
    std::unordered_map<std::string, Module*> modulesByName;
};
