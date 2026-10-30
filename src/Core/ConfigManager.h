#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include "Config.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Visual/ClickGUI.h"
#include "../Commands/CommandManager.h"
#include "../Modules/Visual/Notifications.h"

class ConfigManager {
public:
    static std::string GetSunsteadDir() {
        char* appdata = nullptr;
        size_t len = 0;
        _dupenv_s(&appdata, &len, "APPDATA");
        std::string base(appdata ? appdata : "");
        free(appdata);
        return base + "\\Sunstead\\";
    }

    static std::string GetConfigDirectory() {
        std::string path = GetSunsteadDir() + "configs\\";
        std::filesystem::create_directories(path);
        return path;
    }

    static std::vector<std::string> ListConfigs() {
        std::vector<std::string> list;
        std::string dir = GetConfigDirectory();
        try {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    list.push_back(entry.path().stem().string());
                }
            }
        } catch (...) {}
        return list;
    }

    static bool Save(const std::string& name) {
        std::string path = GetConfigDirectory() + name + ".json";
        nlohmann::json json;

        // Serialize prefix
        json["prefix"] = CommandManager::GetInstance().mPrefix;
        // The eject hotkey lives in Config, not on a module, so it needs its own
        // key here — the module loop below would never reach it.
        json["ejectKey"] = Config::UninjectKey;

        // Serialize modules
        nlohmann::json modulesJson = nlohmann::json::array();
        for (auto* mod : ModuleManager::GetInstance().GetModules()) {
            nlohmann::json modJson;
            modJson["name"] = mod->GetName();
            modJson["enabled"] = mod->IsEnabled();
            modJson["keybind"] = mod->GetKeybind();
            // HUD modules only — non-HUD modules don't have draggable pos/scale.
            if (mod->mIsHudModule) {
                modJson["posX"] = mod->mPosX;
                modJson["posY"] = mod->mPosY;
                modJson["scale"] = mod->mScale;
            }

            nlohmann::json settingsJson = nlohmann::json::array();
            for (auto* setting : mod->mSettings) {
                nlohmann::json setJson;
                setJson["name"] = setting->mName;
                setJson["type"] = static_cast<int>(setting->mType);
                if (setting->mType == SettingType::Bool) {
                    setJson["value"] = static_cast<BoolSetting*>(setting)->mValue;
                } else if (setting->mType == SettingType::Number) {
                    setJson["value"] = static_cast<NumberSetting*>(setting)->mValue;
                } else if (setting->mType == SettingType::Enum) {
                    setJson["value"] = static_cast<EnumSetting*>(setting)->mValue;
                } else if (setting->mType == SettingType::Color) {
                    ColorSetting* col = static_cast<ColorSetting*>(setting);
                    setJson["value"] = { col->mValue[0], col->mValue[1], col->mValue[2], col->mValue[3] };
                }
                settingsJson.push_back(setJson);
            }
            modJson["settings"] = settingsJson;
            modulesJson.push_back(modJson);
        }
        json["modules"] = modulesJson;

        // Serialize ClickGUI columns positions
        nlohmann::json columnsJson = nlohmann::json::array();
        for (const auto& pos : ClickGUI::GetCategoryPositions()) {
            nlohmann::json colJson;
            colJson["x"] = pos.x;
            colJson["y"] = pos.y;
            colJson["isExtended"] = pos.isExtended;
            columnsJson.push_back(colJson);
        }
        json["columns"] = columnsJson;

        std::ofstream file(path);
        if (!file.is_open()) return false;
        file << json.dump(4);
        file.close();
        return true;
    }

    static bool Load(const std::string& name) {
        std::string path = GetConfigDirectory() + name + ".json";
        if (!std::filesystem::exists(path)) return false;

        std::ifstream file(path);
        if (!file.is_open()) return false;
        nlohmann::json json;
        try {
            file >> json;
        } catch (...) {
            file.close();
            return false;
        }
        file.close();

        // Deserialize prefix
        if (json.contains("prefix") && json["prefix"].is_string()) {
            CommandManager::GetInstance().mPrefix = json["prefix"].get<std::string>();
        }
        if (json.contains("ejectKey") && json["ejectKey"].is_number()) {
            Config::UninjectKey = json["ejectKey"].get<int>();
        }

        // Deserialize modules
        if (json.contains("modules") && json["modules"].is_array()) {
            for (const auto& modJson : json["modules"]) {
                if (!modJson.contains("name") || !modJson["name"].is_string()) continue;
                std::string modName = modJson["name"].get<std::string>();
                auto* mod = ModuleManager::GetInstance().GetModuleByName(modName);
                if (!mod) continue;

                bool targetEnabled = false;
                bool hasEnabled = false;
                if (modJson.contains("enabled") && modJson["enabled"].is_boolean()) {
                    targetEnabled = modJson["enabled"].get<bool>();
                    hasEnabled = true;
                }
                if (modJson.contains("keybind") && modJson["keybind"].is_number()) {
                    mod->SetKeybind(modJson["keybind"].get<int>());
                }
                // Only restore layout for HUD elements.
                if (mod->mIsHudModule) {
                    if (modJson.contains("posX") && modJson["posX"].is_number())
                        mod->mPosX = modJson["posX"].get<float>();
                    if (modJson.contains("posY") && modJson["posY"].is_number())
                        mod->mPosY = modJson["posY"].get<float>();
                    if (modJson.contains("scale") && modJson["scale"].is_number())
                        mod->mScale = modJson["scale"].get<float>();
                }
 
                if (modJson.contains("settings") && modJson["settings"].is_array()) {
                    for (const auto& setJson : modJson["settings"]) {
                        if (!setJson.contains("name") || !setJson["name"].is_string()) continue;
                        std::string setName = setJson["name"].get<std::string>();
                        Setting* setting = nullptr;
                        for (auto* s : mod->mSettings) {
                            if (s->mName == setName) {
                                setting = s;
                                break;
                            }
                        }
                        if (!setting || !setJson.contains("value")) continue;
 
                        if (setting->mType == SettingType::Bool && setJson["value"].is_boolean()) {
                            static_cast<BoolSetting*>(setting)->mValue = setJson["value"].get<bool>();
                        } else if (setting->mType == SettingType::Number && setJson["value"].is_number()) {
                            static_cast<NumberSetting*>(setting)->mValue = setJson["value"].get<float>();
                        } else if (setting->mType == SettingType::Enum && setJson["value"].is_number()) {
                            // Stale configs can carry an index past a SHORTENED
                            // enum list (options get retired over time, e.g. the
                            // ClickGUI Animation entry). Clamping keeps the load
                            // safe instead of indexing out of range at draw time.
                            auto* e = static_cast<EnumSetting*>(setting);
                            int v = setJson["value"].get<int>();
                            const int maxIdx = (int)e->mValues.size() - 1;
                            if (maxIdx < 0) v = 0;
                            else if (v < 0) v = 0;
                            else if (v > maxIdx) v = maxIdx;
                            e->mValue = v;
                        } else if (setting->mType == SettingType::Color && setJson["value"].is_array() && setJson["value"].size() == 4) {
                            ColorSetting* col = static_cast<ColorSetting*>(setting);
                            for (int idx = 0; idx < 4; ++idx) {
                                col->mValue[idx] = setJson["value"][idx].get<float>();
                            }
                        }
                    }
                }

                if (hasEnabled) {
                    mod->SetEnabled(targetEnabled);
                }
            }
        }

        // Deserialize ClickGUI columns positions
        if (json.contains("columns") && json["columns"].is_array()) {
            std::vector<ClickGUI::CategoryPos> positions;
            for (const auto& colJson : json["columns"]) {
                ClickGUI::CategoryPos pos;
                if (colJson.contains("x") && colJson["x"].is_number()) pos.x = colJson["x"].get<float>();
                if (colJson.contains("y") && colJson["y"].is_number()) pos.y = colJson["y"].get<float>();
                if (colJson.contains("isExtended") && colJson["isExtended"].is_boolean()) pos.isExtended = colJson["isExtended"].get<bool>();
                positions.push_back(pos);
            }
            ClickGUI::SetCategoryPositions(positions);
        }

        return true;
    }
};
