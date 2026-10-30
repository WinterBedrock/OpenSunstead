#pragma once
#include <string>
#include <string_view>
#include <unordered_map>

namespace SDK {

class ScreenNames {
public:
    enum Type : int32_t {
        Unknown = 0,
        HudScreen,
        StartScreen,
        PauseScreen,
        InventoryScreen,
        ChestScreen,
        ShulkerBoxScreen,
        BarrelScreen,
        HopperScreen,
        DispenserScreen,
        DropperScreen,
        ContainerScreen,
        LargeChest,
        ChatScreen,
        PlayScreen,
        DebugScreen,
        ToastScreen,
        HudCrosshairScreen,
        WorldLoadingProgressScreen,
        WorldSavingProgressScreen,
        NetherLoadingProgressScreen,
        OverworldLoadingProgressScreen,
    };

    static const char* toString(Type type) {
        switch (type) {
            case HudScreen:                    return "hud_screen";
            case StartScreen:                  return "start_screen";
            case PauseScreen:                  return "pause_screen";
            case InventoryScreen:              return "inventory_screen";
            case ChestScreen:                  return "chest_screen";
            case ShulkerBoxScreen:             return "shulker_box_screen";
            case BarrelScreen:                 return "barrel_screen";
            case HopperScreen:                 return "hopper_screen";
            case DispenserScreen:              return "dispenser_screen";
            case DropperScreen:                return "dropper_screen";
            case ContainerScreen:              return "container_screen";
            case LargeChest:                   return "large_chest";
            case ChatScreen:                   return "chat_screen";
            case PlayScreen:                   return "play_screen";
            case DebugScreen:                  return "debug_screen";
            case ToastScreen:                  return "toast_screen";
            case HudCrosshairScreen:           return "hud_crosshair_screen";
            case WorldLoadingProgressScreen:   return "world_loading_progress_screen";
            case WorldSavingProgressScreen:    return "world_saving_progress_screen";
            case NetherLoadingProgressScreen:  return "nether_loading_progress_screen";
            case OverworldLoadingProgressScreen: return "overworld_loading_progress_screen";
            default:                           return "unknown";
        }
    }

    static Type fromString(const std::string& str) {
        static const std::unordered_map<std::string_view, Type> map = {
            {"hud_screen",                       HudScreen},
            {"start_screen",                     StartScreen},
            {"pause_screen",                     PauseScreen},
            {"inventory_screen",                 InventoryScreen},
            {"chest_screen",                     ChestScreen},
            {"shulker_box_screen",               ShulkerBoxScreen},
            {"barrel_screen",                    BarrelScreen},
            {"hopper_screen",                    HopperScreen},
            {"dispenser_screen",                 DispenserScreen},
            {"dropper_screen",                   DropperScreen},
            {"container_screen",                 ContainerScreen},
            {"large_chest",                      LargeChest},
            {"chat_screen",                      ChatScreen},
            {"play_screen",                      PlayScreen},
            {"debug_screen",                     DebugScreen},
            {"toast_screen",                     ToastScreen},
            {"hud_crosshair_screen",             HudCrosshairScreen},
            {"world_loading_progress_screen",    WorldLoadingProgressScreen},
            {"world_saving_progress_screen",     WorldSavingProgressScreen},
            {"nether_loading_progress_screen",   NetherLoadingProgressScreen},
            {"overworld_loading_progress_screen", OverworldLoadingProgressScreen},
        };
        auto it = map.find(str);
        return (it != map.end()) ? it->second : Unknown;
    }

    static bool isContainer(Type type) {
        switch (type) {
            case ChestScreen:
            case ShulkerBoxScreen:
            case BarrelScreen:
            case HopperScreen:
            case DispenserScreen:
            case DropperScreen:
            case ContainerScreen:
            case LargeChest:
                return true;
            default:
                return false;
        }
    }

    static bool isLoading(Type type) {
        switch (type) {
            case WorldLoadingProgressScreen:
            case WorldSavingProgressScreen:
            case NetherLoadingProgressScreen:
            case OverworldLoadingProgressScreen:
                return true;
            default:
                return false;
        }
    }

    static bool isGameplay(Type type) {
        switch (type) {
            case HudScreen:
            case PlayScreen:
            case ChatScreen:
            case HudCrosshairScreen:
                return true;
            default:
                return false;
        }
    }
};

} // namespace SDK