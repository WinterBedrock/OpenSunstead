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
#include "Visual/FpsGraph.h"
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
#include "Misc/Renderer.h"
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
#include "Visual/MouseKeystrokes.h"
#include "Visual/NameProtect.h"
#include "Visual/Xray.h"
#include "Visual/ChinaHat.h"
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
#include "../Utils/MiscUtils/ColorUtils.hpp"
#include <Windows.h>
#include <imgui.h>

// Keeps mHoldMode in sync with a module's "Bind Type" enum (Toggle/Hold), the
// convention Zoom/Jetpack/Freelook/TabList follow. Without this the flag could
// only be updated from inside the module's OnTick — which the tick loop skips
// for a DISABLED module, so a hold bind on a toggled-off module would never
// engage. Scanning for the setting by name keeps it generic: modules without a
// "Bind Type" enum keep whatever their constructor set.
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
    // The ClickGUI owns the keyboard while it is open: a hold bind whose key
    // happens to be held (or gets pressed) over the menu must not force-enable
    // its module — that is exactly the "hold triggers with the GUI open" bug.
    Module* gui = GetModuleByName("ClickGUI");
    const bool guiOpen = gui && gui->IsEnabled();
    const bool gameplay = !guiOpen && SDK::isGameplayInputActive();
    for (auto& mod : modules) {
        if (!mod) continue;
        syncHoldBindType(mod.get());
        Module* m = mod.get();
        if (!m->mHoldMode) {
            // Leaving hold mode must never leave a stale forced-enable behind.
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
            // The user disabling the module mid-hold is respected: once the
            // span is engaged we never re-enable until the key is released.
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
    // Xray FIRST: its optional "Darkness" plate must land at the bottom of
    // the shared background draw list, under every later module's boxes.
    // DEBUG ONLY — the retexture sweep is not release-ready yet.
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
    modules.push_back(std::make_unique<FpsGraph>());
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
    modules.push_back(std::make_unique<Renderer>());
    modules.push_back(std::make_unique<ChatLogger>());
    modules.push_back(std::make_unique<PlayerAlarm>());
    modules.push_back(std::make_unique<PacketLogger>());
    modules.push_back(std::make_unique<Blink>());
    // Translate sits ahead of Chat so a Delay cancel is visible when Chat
    // decides whether to record the original line.
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
    modules.push_back(std::make_unique<ChinaHat>());
    modules.push_back(std::make_unique<JumpCircles>());
    modules.push_back(std::make_unique<HealthWarning>());
    modules.push_back(std::make_unique<EffectWarning>());
    modules.push_back(std::make_unique<ClickEffect>());
    modules.push_back(std::make_unique<MouseKeystrokes>());
    modules.push_back(std::make_unique<NameProtect>());
    modules.push_back(std::make_unique<SuperHeroFX>());
    modules.push_back(std::make_unique<Chams>());
    modules.push_back(std::make_unique<Trail>());
    modules.push_back(std::make_unique<Player>());

    // Snapshot every module's construction-time HUD position BEFORE the config
    // load overwrites mPosX/mPosY. The HUD editor Reset button restores these.
    for (auto& mod : modules) mod->SnapshotDefaultPos();

    // Inject the universal "Visible" ArrayList toggle into every module that can
    // actually appear in the ArrayList. The menu/editor and settings-only modules
    // are never listed there, so we skip them to avoid a meaningless toggle.
    for (auto& mod : modules) {
        const std::string& n = mod->GetName();
        if (n == "ClickGUI" || n == "Renderer") continue;
        mod->EnsureVisibleSetting();
    }

    // enabled by default
    GetModuleByName("Watermark")->SetEnabled(true);
    GetModuleByName("Notifications")->SetEnabled(true);
    GetModuleByName("ArrayList")->SetEnabled(true);
    GetModuleByName("ToggleSounds")->SetEnabled(true);
    GetModuleByName("Chat")->SetEnabled(true);
    GetModuleByName("Renderer")->SetEnabled(true);
}

// SEH must live in a function with no C++ objects (no destructors), so the
// per-module tick call is isolated here. The name is captured as a raw const
// char* before the call so logging survives a crash inside the module.
static int g_lastExcCode = 0;
static void* g_lastExcAddr = nullptr;
// Breadcrumbs: a per-thread note of which module callback is running, so the
// crash report can name the culprit instead of only giving an address. The
// buffers are thread_local and formatted BEFORE entering the __try, because
// std::string work is not allowed in a frame that needs SEH unwinding (C2712)
// and a faulting frame is no place to be allocating anyway.
static thread_local char t_crumb[96];

static void setCrumb(const char* phase, Module* mod) {
    // Null-safe: several call sites pass GetModuleByName(...) straight in,
    // which returns nullptr when the module is missing. Dereferencing it here
    // faulted inside Sunstead.dll while the breadcrumb still named whatever
    // module ran last (e.g. "OnUpdate('Chams')"), misattributing the crash.
    const char* n = (mod != nullptr) ? mod->GetName().c_str() : "<null>";
    // snprintf, not string concatenation: this runs for every module every frame.
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

// Blink post-disable drain. The module lookup stays OUTSIDE the __try:
// GetModuleByName takes a std::string key, and a temporary with a destructor
// inside __try is C2712.
static void sehBlinkDrain(Module* blinkMod) {
    setCrumb("BlinkDrain", blinkMod);
    Blink* blink = static_cast<Blink*>(blinkMod);
    if (!blink || blink->IsEnabled() || !blink->IsDraining()) return;
    __try { blink->DrainTick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ModuleManager::OnTick() {
    // Hold binds first: a held module must be force-enabled before the loop
    // below so it ticks in the very frame the key goes down.
    PollHoldBinds();
    // Each module gets its own SEH guard so a single crashing module (bad
    // offset / stale pointer) can no longer take down the entire tick for
    // every other module. The previous code wrapped the whole loop in one SEH
    // вЂ” one throw killed OnTick for ALL modules that frame, which is why
    // Zoom/Jetpack appeared "broken": they never ran because an earlier
    // module in the list crashed first.
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
    // Runs on the GAME UPDATE THREAD (UpdatePlayerFromCameraSystemUtil::_updatePlayer).
    // State-mutating actions like GameMode::attack MUST run here, not from the
    // render hook (renderLevel) вЂ” otherwise the attack executes but the damage
    // never registers (this was the Aura "runs but no hit" bug).
    // Xray's retexture sweep drains HERE (always, module enabled or not - the
    // post-disable sweep must keep running to restore the real textures): it
    // fires BlockSource::setBlock, i.e. a world mutation, which belongs on the
    // game update thread. See Xray.cpp "RETEXTURE SWEEP".
    // Explicit crumb: without it a fault in here is attributed to whatever
    // module ran last on the previous tick (e.g. Chams).
    setCrumb("Sweep", GetModuleByName("Xray"));
    Xray_StepRetextureSweep();
    // Blink's post-disable paced flush (no-op while enabled or queue empty).
    // OnTick skips disabled modules, so the drain cannot live there; the old
    // blocking OnDisable flush froze the game instead.
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
    // Hold binds also poll on the render thread: this is per-FRAME, so the
    // force-enable lands in the same frame the key goes down instead of
    // waiting for the next 20 Hz tick.
    PollHoldBinds();
    static std::unordered_map<std::string, int> crashCountR;
    // Retired head/skin textures: release the ones whose grace period is
    // over (deferred so a mid-frame module close can never leave a pending
    // draw referencing a freed SRV — the tab-spam driver crash).
    SkinUtils::flushRetiredTextures();
    // Reset the "this frame's left click was handled by the module itself"
    // latch (Module::mClickConsumed) before any module runs, so HudEditor —
    // which runs after this loop — only sees flags set during THIS frame.
    for (auto& mod : modules)
        if (mod) mod->mClickConsumed = false;
    for (auto& mod : modules) {
        const std::string& nm = mod->GetName();
        // ClickGUI/TabList/HUD modules render in the always-render loop below
        // so their CLOSE (fade-out) animation keeps playing after they are
        // disabled. Zoom and Aura too: their zoom-out / visual exit animation
        // lives in OnRender.
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
        // Always render these so their CLOSE (fade-out) animation can play
        // after the module is disabled. Their OnRender early-returns once the
        // animation value settles back to ~0 while disabled. HUD modules
        // (Scaffold's block counter etc.) are here for the same reason: the
        // appear/disappear animation is driven inside OnRender, which would
        // otherwise never run while disabled - the counter would pop in and
        // out with no animation at all. Zoom is here so its FOV release
        // animation (the zoom-out after disable) keeps running.
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
    // The custom chat owns the keyboard while its input is open — toggle and
    // action binds must never fire on keys the user is typing.
    if (Chat::IsOpen()) return;
    for (auto& mod : modules) {
        if (key == mod->GetKeybind() && key != 0) {
            // mKeybindToggles=false: key is an action bind (throw / etc.), not a
            // module toggle — skip Toggle/notification/sound so the press stays snappy.
            if (!mod->mHoldMode && mod->mKeybindToggles) {
                mod->Toggle();
                if (GetModuleByName("ToggleSounds") && GetModuleByName("ToggleSounds")->IsEnabled()) {
                    ToggleSounds::PlayToggleSound(mod->IsEnabled());
                }
                // The toast carries just the module NAME + the type: Nova
                // builds its two-line "name / name + status" card from it,
                // Stead its classic "Name Enabled/Disabled" title.
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

std::vector<Module*> ModuleManager::GetModules() const {
    std::vector<Module*> result;
    for (auto& mod : modules) result.push_back(mod.get());
    return result;
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
    // Case-insensitive match so commands like ".bind killaura" / ".t Killaura"
    // work regardless of how the user typed the module name.
    auto ieq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        return true;
    };
    // exact match first (fast path), then case-insensitive fallback
    for (auto& mod : modules)
        if (mod->GetName() == name) return mod.get();
    for (auto& mod : modules)
        if (ieq(mod->GetName(), name)) return mod.get();
    return nullptr;
}
