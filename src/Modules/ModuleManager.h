#pragma once
#include "Module.h"
#include <vector>
#include <memory>
#include <string>

class ModuleManager {
public:
    static ModuleManager& GetInstance() {
        static ModuleManager instance;
        return instance;
    }

    void Initialize();
    void OnTick();
    void OnUpdate();   // dispatched from the GAME TICK (hkUpdatePlayer), not render — required for combat/state-mutating modules
    void OnRender();
    void OnKey(int key);
    // Hold-bind poll: force-enables mHoldMode modules while their keybind is
    // held (even when the module itself is toggled OFF) and force-disables them
    // on release. Called at the top of OnTick/OnRender so a held module ticks
    // and renders in the same frame the key goes down.
    void PollHoldBinds();
    // Outgoing packet send hook (GameHooks::hkPacketSend).
    void OnPacketSend(class SDK::Packet* packet, int packetId, bool& cancel);

    std::vector<Module*> GetModules() const;
    std::vector<Module*> GetModulesInCategory(Category category) const;
    Module* GetModuleByName(const std::string& name) const;

private:
    ModuleManager() = default;
    std::vector<std::unique_ptr<Module>> modules;
};