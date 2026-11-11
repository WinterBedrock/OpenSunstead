#include "ModuleManager.h"
#include "../Core/CrashHandler.h"
#include "../SDK/Network/Packet.h"
#include <unordered_map>
#include <cctype>
#include <cstdio>
#include "Player/Freelook.h"
#include "Player/AnyOffhand.h"
#include "Player/Offhand.h"
#include "Movement/NoClip.h"
#include "Visual/CameraClip.h"
#include "Visual/FogColor.h"
#include "Visual/TimeChanger.h"
#include "Visual/Watermark.h"
#include "Visual/Notifications.h"
#include "Visual/ClickGUI.h"
#include "Visual/ArrayList.h"
#include "Visual/FPS.h"
#include "Visual/Ping.h"
#include "Visual/BPS.h"
#include "Visual/MotionBlur.h"
#include "Visual/Keystrokes.h"
#include "Visual/CpsCounter.h"
#include "Visual/Fullbright.h"
#include "Visual/NoFire.h"
#include "Visual/Xray.h"
#include "Visual/ItemPhysics.h"
#include "Visual/Swing.h"
#include "Visual/ViewModel.h"
#include "Visual/SelfNametag.h"
#include "Misc/ToggleSounds.h"
#include "Visual/Zoom.h"
#include "Combat/Reach.h"
#include "Combat/Sprint.h"
#include "Combat/HitBoxes.h"
#include "Combat/AutoClicker.h"
#include "Combat/Aura.h"
#include "Combat/Switcher.h"
#include "Combat/AimAssist.h"
#include "Combat/AimBot.h"
#include "Combat/TriggerBot.h"
#include "Misc/EditionFaker.h"
#include "Misc/Disabler.h"
#include "Misc/PacketLogger.h"
#include "Misc/Blink.h"
#include "Misc/KickSounds.h"
#include "Misc/DeviceSpoofer.h"
#include "Misc/AntiBot.h"
#include "Visual/Chat.h"
#include "Misc/ChatLogger.h"
#include "Misc/Translate.h"
#include "Misc/PlayerAlarm.h"
#include "Visual/Coordinates.h"
#include "Visual/BlockInfo.h"
#include "Visual/ESP.h"
#include "Visual/TrueSight.h"
#include "Visual/SuperHeroFX.h"
#include "Visual/Chams.h"
#include "Visual/Trail.h"
#include "Visual/Player.h"


#include "Visual/BlockESP.h"
#include "Visual/Tracers.h"
#include "Visual/BlockTracers.h"
#include "Visual/NameTags.h"
#include "Visual/TabList.h"
#include "Misc/OptimizeFocus.h"
#include "Player/Velocity.h"
#include "Player/Timer.h"
#include "Player/GhostHand.h"
#include "Player/AutoTool.h"
#include "Player/OreMiner.h"
#include "Player/ChestStealer.h"
#include "Player/InventoryManager.h"
#include "Player/FastEat.h"
#include "Player/ChestAura.h"
#include "Player/NoFall.h"
#include "Player/FreeCam.h"
#include "Player/FastMine.h"
#include "Player/AntiEffect.h"
#include "Player/PlaceDelay.h"
#include "Movement/Jetpack.h"
#include "Movement/Jesus.h"
#include "Movement/InventoryMove.h"
#include "Movement/Fly.h"
#include "Movement/Speed.h"
#include "Movement/AirJump.h"
#include "Movement/Scaffold.h"
#include "Movement/Anchor.h"
#include "Movement/Twerk.h"
#include "Movement/StarveSprint.h"
#include "Movement/NoSlow.h"
#include "Movement/AntiImmobile.h"
#include "Movement/Baritone.h"
#include "Misc/Spammer.h"
#include "Misc/Matrix.h"
#include "Visual/GuiScale.h"
#include "Visual/ClickEffect.h"
#include "Visual/MouseStrokes.h"
#include "Visual/NameProtect.h"
#include "Visual/Hat.h"
#include "Visual/JumpCircles.h"
#include "Visual/HealthWarning.h"
#include "Visual/EffectWarning.h"
#include "Visual/TargetHUD.h"
#include "../SDK/SDK.h"
#include "../Core/Sunstead.h"
#include "../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../Utils/MiscUtils/SkinUtils.hpp"
#include "../Utils/MiscUtils/ColorUtils.hpp"
#include "../Utils/MiscUtils/D2D.hpp"
#include "../Core/Logger.h"
#include <Windows.h>
#include <imgui.h>

static void syncHoldBindType(Module* mod) {
    if (!mod) return;
    for (Setting* s : mod->mSettings) {
        if (!s || s->mType != SettingType::Enum) continue;
        auto* e = static_cast<EnumSetting*>(s);
        if (e->mName != "Bind Type") continue;
        if (e->mValues.size() == 2 && e->mValues[0] == "Toggle" && e->mValues[1] == "Hold")
            mod->mHoldMode = (e->mValue == 1);
        return;
    }
}

void ModuleManager::PollHoldBinds() {
    Module* gui = GetModuleByName("ClickGUI");
    const bool guiOpen = gui && gui->IsEnabled();
    const bool gameplay = !guiOpen && SDK::isGameplayInputActive();
    for (auto& mod : modules) {
        if (!mod) continue;
        syncHoldBindType(mod.get());
        Module* m = mod.get();
        if (!m->mHoldMode) {
            if (m->mHoldForced) {
                m->mHoldForced = false;
                if (m->IsEnabled()) m->SetEnabled(false);
            }
            m->mHoldEngaged = false;
            m->mHoldKeyDown = false;
            continue;
        }
        const int key = m->GetKeybind();
        const bool down = gameplay && key != 0 && (GetAsyncKeyState(key) & 0x8000) != 0;
        m->mHoldKeyDown = down;

        if (down) {
            if (m->IsEnabled()) {
                m->mHoldEngaged = true;
            } else if (!m->mHoldEngaged) {
                m->mHoldEngaged = true;
                m->mHoldForced = true;
                m->SetEnabled(true);
            }
        } else {
            if (m->mHoldForced) {
                m->mHoldForced = false;
                if (m->IsEnabled()) m->SetEnabled(false);
            }
            m->mHoldEngaged = false;
        }
    }
}

void ModuleManager::Initialize() {
#ifdef SUNSTEAD_DEBUG
    modules.push_back(std::make_unique<Xray>());
#endif
    modules.push_back(std::make_unique<Watermark>());
    modules.push_back(std::make_unique<Notifications>());
    modules.push_back(std::make_unique<ClickGUI>());
    modules.push_back(std::make_unique<ArrayList>());
    modules.push_back(std::make_unique<ESP>());
    modules.push_back(std::make_unique<TrueSight>());


    modules.push_back(std::make_unique<BlockESP>());
    modules.push_back(std::make_unique<Tracers>());
    modules.push_back(std::make_unique<BlockTracers>());
    modules.push_back(std::make_unique<NameTags>());
    modules.push_back(std::make_unique<TabList>());
    modules.push_back(std::make_unique<TargetHUD>());
    modules.push_back(std::make_unique<OptimizeFocus>());
    modules.push_back(std::make_unique<FPS>());
    modules.push_back(std::make_unique<Ping>());

    modules.push_back(std::make_unique<BPS>());
    modules.push_back(std::make_unique<MotionBlur>());
    modules.push_back(std::make_unique<Keystrokes>());
    modules.push_back(std::make_unique<CpsCounter>());
    modules.push_back(std::make_unique<Fullbright>());
    modules.push_back(std::make_unique<NoFire>());
    modules.push_back(std::make_unique<ItemPhysics>());
    modules.push_back(std::make_unique<Swing>());
    modules.push_back(std::make_unique<ViewModel>());
    modules.push_back(std::make_unique<Reach>());
    modules.push_back(std::make_unique<SelfNametag>());
    modules.push_back(std::make_unique<ToggleSounds>());
    modules.push_back(std::make_unique<Zoom>());
    modules.push_back(std::make_unique<EditionFaker>());
    modules.push_back(std::make_unique<Disabler>());
    modules.push_back(std::make_unique<Sprint>());
    modules.push_back(std::make_unique<HitBoxes>());
    modules.push_back(std::make_unique<AutoClicker>());
    modules.push_back(std::make_unique<Aura>());
    modules.push_back(std::make_unique<AimAssist>());
    modules.push_back(std::make_unique<AimBot>());
    modules.push_back(std::make_unique<TriggerBot>());
    modules.push_back(std::make_unique<Switcher>());
    modules.push_back(std::make_unique<AntiBot>());

    modules.push_back(std::make_unique<ChatLogger>());
    modules.push_back(std::make_unique<PlayerAlarm>());
    modules.push_back(std::make_unique<PacketLogger>());
    modules.push_back(std::make_unique<Blink>());
    modules.push_back(std::make_unique<Translate>());
    modules.push_back(std::make_unique<Chat>());
    modules.push_back(std::make_unique<Matrix>());
    modules.push_back(std::make_unique<KickSounds>());
    modules.push_back(std::make_unique<Freelook>());
    modules.push_back(std::make_unique<AnyOffhand>());
    modules.push_back(std::make_unique<Offhand>());
    modules.push_back(std::make_unique<NoClip>());
    modules.push_back(std::make_unique<CameraClip>());
    modules.push_back(std::make_unique<FogColor>());
    modules.push_back(std::make_unique<TimeChanger>());
    modules.push_back(std::make_unique<DeviceSpoofer>());
    modules.push_back(std::make_unique<Coordinates>());
    modules.push_back(std::make_unique<BlockInfo>());
    modules.push_back(std::make_unique<Velocity>());
    modules.push_back(std::make_unique<Timer>());
    modules.push_back(std::make_unique<GhostHand>());
    modules.push_back(std::make_unique<AutoTool>());
    modules.push_back(std::make_unique<OreMiner>());
    modules.push_back(std::make_unique<FastMine>());
    modules.push_back(std::make_unique<ChestStealer>());
    modules.push_back(std::make_unique<InventoryManager>());
    modules.push_back(std::make_unique<FastEat>());
    modules.push_back(std::make_unique<ChestAura>());
    modules.push_back(std::make_unique<NoFall>());
    modules.push_back(std::make_unique<AntiEffect>());
    modules.push_back(std::make_unique<PlaceDelay>());
    modules.push_back(std::make_unique<FreeCam>());
    modules.push_back(std::make_unique<Jetpack>());
    modules.push_back(std::make_unique<Jesus>());
    modules.push_back(std::make_unique<InventoryMove>());
    modules.push_back(std::make_unique<Fly>());
    modules.push_back(std::make_unique<Speed>());
    modules.push_back(std::make_unique<AirJump>());
    modules.push_back(std::make_unique<Scaffold>());
    modules.push_back(std::make_unique<Anchor>());
    modules.push_back(std::make_unique<Twerk>());
    modules.push_back(std::make_unique<StarveSprint>());
    modules.push_back(std::make_unique<NoSlow>());
    modules.push_back(std::make_unique<AntiImmobile>());
    modules.push_back(std::make_unique<Baritone>());
    modules.push_back(std::make_unique<Spammer>());
    modules.push_back(std::make_unique<GuiScale>());
    modules.push_back(std::make_unique<Hat>());
    modules.push_back(std::make_unique<JumpCircles>());
    modules.push_back(std::make_unique<HealthWarning>());
    modules.push_back(std::make_unique<EffectWarning>());
    modules.push_back(std::make_unique<ClickEffect>());
    modules.push_back(std::make_unique<MouseStrokes>());
    modules.push_back(std::make_unique<NameProtect>());
    modules.push_back(std::make_unique<SuperHeroFX>());
    modules.push_back(std::make_unique<Chams>());
    modules.push_back(std::make_unique<Trail>());
    modules.push_back(std::make_unique<Player>());

    for (auto& mod : modules) mod->SnapshotDefaultPos();

    for (auto& mod : modules) {
        const std::string& n = mod->GetName();
        if (n == "ClickGUI") continue;
        mod->EnsureVisibleSetting();
    }

    moduleViews.clear();
    moduleViews.reserve(modules.size());
    modulesByName.clear();
    modulesByName.reserve(modules.size());
    for (const auto& mod : modules) {
        moduleViews.push_back(mod.get());
        modulesByName.emplace(mod->GetName(), mod.get());
    }

    GetModuleByName("Watermark")->SetEnabled(true);
    GetModuleByName("Notifications")->SetEnabled(true);
    GetModuleByName("ArrayList")->SetEnabled(true);
    GetModuleByName("ToggleSounds")->SetEnabled(true);
    GetModuleByName("AntiBot")->SetEnabled(true);
    GetModuleByName("Chat")->SetEnabled(true);

}

static int g_lastExcCode = 0;
static void* g_lastExcAddr = nullptr;
static thread_local char t_crumb[96];

static void setCrumb(const char* phase, Module* mod) {
    const char* n = (mod != nullptr) ? mod->GetName().c_str() : "<null>";
    std::snprintf(t_crumb, sizeof(t_crumb), "%s('%s')", phase, n);
    CrashHandler::SetContext(t_crumb);
}

static int sehOnTickModule(Module* mod) {
    __try {
        mod->OnTick();
        return 0;
    } __except (g_lastExcCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static int sehOnUpdateModule(Module* mod) {
    __try {
        mod->OnUpdate();
        return 0;
    } __except (g_lastExcCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static int sehOnRenderModule(Module* mod) {
    __try {
        mod->OnRender();
        return 0;
    } __except (g_lastExcCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static void sehBlinkDrain(Module* blinkMod) {
    setCrumb("BlinkDrain", blinkMod);
    Blink* blink = static_cast<Blink*>(blinkMod);
    if (!blink || blink->IsEnabled() || !blink->IsDraining()) return;
    __try { blink->DrainTick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ModuleManager::OnTick() {
    PollHoldBinds();
    static std::unordered_map<std::string, int> crashCount;
    for (auto& mod : modules) {
        if (!mod || !mod->IsEnabled()) continue;
        const std::string& name = mod->GetName();
        setCrumb("OnTick", mod.get());
            if (sehOnTickModule(mod.get()) != 0) {
            int& n = crashCount[name];
            if (++n <= 3) {
                LOG_WARN("OnTick('" + name + "') threw (suppressed, count=" + std::to_string(n) + ", code=0x" + std::to_string(g_lastExcCode) + ")", "ModuleManager");
            }
        }
    }
}


void ModuleManager::OnUpdate() {
    setCrumb("Sweep", GetModuleByName("Xray"));
    //Xray_StepRetextureSweep();
    sehBlinkDrain(GetModuleByName("Blink"));
    static std::unordered_map<std::string, int> crashCountU;
    for (auto& mod : modules) {
        if (!mod || !mod->IsEnabled()) continue;
        const std::string& name = mod->GetName();
        setCrumb("OnUpdate", mod.get());
            if (sehOnUpdateModule(mod.get()) != 0) {
            int& n = crashCountU[name];
            if (++n <= 3) {
                LOG_WARN("OnUpdate('" + name + "') threw (suppressed, count=" + std::to_string(n) + ", code=0x" + std::to_string(g_lastExcCode) + ")", "ModuleManager");
            }
        }
    }
    CrashHandler::ClearContext();
}

void ModuleManager::OnPacketSend(SDK::Packet* packet, int packetId, bool& cancel) {
    if (!packet) return;
    for (auto& mod : modules) {
        if (!mod || !mod->IsEnabled()) continue;
        mod->OnPacketSend(packet, packetId, cancel);
        if (cancel) return;
    }
}
void ModuleManager::OnRender() {
    PollHoldBinds();
    static std::unordered_map<std::string, int> crashCountR;
    SkinUtils::flushRetiredTextures();
    for (auto& mod : modules)
        if (mod) mod->mClickConsumed = false;
    for (auto& mod : modules) {
        const std::string& nm = mod->GetName();
        if (mod->IsEnabled() && nm != "ClickGUI" && nm != "TabList" && nm != "Zoom" &&
            nm != "Aura" && !mod->mIsHudModule) {
            const std::string& name = nm;
            setCrumb("OnRender", mod.get());
            if (sehOnRenderModule(mod.get()) != 0) {
                int& n = crashCountR[name];
                if (++n <= 3) {
                    LOG_WARN("OnRender('" + name + "') threw (suppressed, count=" + std::to_string(n) + ", code=0x" + std::to_string(g_lastExcCode) + ")", "ModuleManager");
                }
            }
        }
    }
    for (auto& mod : modules) {
        const std::string& nm = mod->GetName();
        if (nm == "ClickGUI" || nm == "TabList" || nm == "Zoom" || nm == "Aura" ||
            mod->mIsHudModule) {
            const std::string& name = nm;
            setCrumb("OnRender", mod.get());
            if (sehOnRenderModule(mod.get()) != 0) {
                int& n = crashCountR[name];
                if (++n <= 3) {
                    LOG_WARN("OnRender('" + name + "') threw (suppressed, count=" + std::to_string(n) + ", code=0x" + std::to_string(g_lastExcCode) + ")", "ModuleManager");
                }
            }
        }
    }
}

void ModuleManager::OnKey(int key) {
    if (Chat::IsOpen()) return;
    for (auto& mod : modules) {
        if (key == mod->GetKeybind() && key != 0) {
            if (!mod->mHoldMode && mod->mKeybindToggles) {
                mod->Toggle();
                if (GetModuleByName("ToggleSounds") && GetModuleByName("ToggleSounds")->IsEnabled()) {
                    ToggleSounds::PlayToggleSound(mod->IsEnabled());
                }
                Notifications::AddNotification(
                    mod->GetName(),
                    mod->IsEnabled() ? NotifType::ToggleOn : NotifType::ToggleOff);
            }
        }
        if (mod->IsEnabled()) {
            mod->OnKey(key);
        }
    }
}

const std::vector<Module*>& ModuleManager::GetModules() const {
    return moduleViews;
}

std::vector<Module*> ModuleManager::GetModulesInCategory(Category category) const {
    std::vector<Module*> result;
    for (auto& mod : modules) {
        if (mod->GetCategory() == category) {
            result.push_back(mod.get());
        }
    }
    return result;
}

Module* ModuleManager::GetModuleByName(const std::string& name) const {
    if (const auto it = modulesByName.find(name); it != modulesByName.end())
        return it->second;

    std::string folded;
    folded.reserve(name.size());
    for (const unsigned char c : name)
        folded.push_back(static_cast<char>(std::tolower(c)));
    for (const auto& [moduleName, mod] : modulesByName) {
        if (moduleName.size() != folded.size()) continue;
        bool equal = true;
        for (size_t i = 0; i < folded.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(moduleName[i])) != folded[i]) {
                equal = false;
                break;
            }
        }
        if (equal) return mod;
    }
    return nullptr;
}
