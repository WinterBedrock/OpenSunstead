#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace SDK {
    enum class DeviceOS : int32_t {
        UNKNOWN = 0,
        ANDROID = 1,
        IOS = 2,
        OSX = 3,
        FIREOS = 4,
        GEARVR = 5,
        HOLOLENS = 6,
        WIN10 = 7,
        WINDOWS_32 = 8,
        DEDICATED = 9,
        TVOS = 10,
        ORBIS = 11,
        NINTENDO_SWITCH = 12,
        XBOX = 13,
        WINDOWS_PHONE = 14,
        LINUX = 15
    };

    enum class InputMode : int32_t {
        UNDEFINED = 0,
        MOUSE = 1,
        TOUCH = 2,
        GAME_PAD = 3,
        MOTION_CONTROLLER = 4,
        COUNT = 5
    };

    inline const char* DeviceOSToString(DeviceOS d) {
        switch (d) {
            case DeviceOS::UNKNOWN: return "Unknown";
            case DeviceOS::ANDROID: return "Android";
            case DeviceOS::IOS: return "iOS";
            case DeviceOS::OSX: return "OSX";
            case DeviceOS::FIREOS: return "FireOS";
            case DeviceOS::GEARVR: return "GearVR";
            case DeviceOS::HOLOLENS: return "Hololens";
            case DeviceOS::WIN10: return "Win10";
            case DeviceOS::WINDOWS_32: return "Win32";
            case DeviceOS::DEDICATED: return "Dedicated";
            case DeviceOS::TVOS: return "TVOS";
            case DeviceOS::ORBIS: return "Orbis";
            case DeviceOS::NINTENDO_SWITCH: return "NintendoSwitch";
            case DeviceOS::XBOX: return "Xbox";
            case DeviceOS::WINDOWS_PHONE: return "WindowsPhone";
            case DeviceOS::LINUX: return "Linux";
            default: return "Unknown";
        }
    }

    inline const char* InputModeToString(InputMode m) {
        switch (m) {
            case InputMode::UNDEFINED: return "Undefined";
            case InputMode::MOUSE: return "Mouse";
            case InputMode::TOUCH: return "Touch";
            case InputMode::GAME_PAD: return "Game Pad";
            case InputMode::MOTION_CONTROLLER: return "Motion Controller";
            case InputMode::COUNT: return "Count";
            default: return "Undefined";
        }
    }

    inline DeviceOS DeviceOSFromIndex(int32_t idx) {
        if (idx < 0) return DeviceOS::UNKNOWN;
        if (idx > static_cast<int32_t>(DeviceOS::LINUX)) return DeviceOS::UNKNOWN;
        return static_cast<DeviceOS>(idx);
    }

    inline InputMode InputModeFromIndex(int32_t idx) {
        if (idx < 0) return InputMode::UNDEFINED;
        if (idx > static_cast<int32_t>(InputMode::COUNT)) return InputMode::UNDEFINED;
        return static_cast<InputMode>(idx);
    }

    inline std::vector<std::string> DeviceOSNames() {
        return {
            DeviceOSToString(DeviceOS::UNKNOWN), DeviceOSToString(DeviceOS::ANDROID), DeviceOSToString(DeviceOS::IOS), DeviceOSToString(DeviceOS::OSX),
            DeviceOSToString(DeviceOS::FIREOS), DeviceOSToString(DeviceOS::GEARVR), DeviceOSToString(DeviceOS::HOLOLENS), DeviceOSToString(DeviceOS::WIN10),
            DeviceOSToString(DeviceOS::WINDOWS_32), DeviceOSToString(DeviceOS::DEDICATED), DeviceOSToString(DeviceOS::TVOS), DeviceOSToString(DeviceOS::ORBIS),
            DeviceOSToString(DeviceOS::NINTENDO_SWITCH), DeviceOSToString(DeviceOS::XBOX), DeviceOSToString(DeviceOS::WINDOWS_PHONE), DeviceOSToString(DeviceOS::LINUX)
        };
    }

    inline std::vector<std::string> InputModeNames() {
        return {
            InputModeToString(InputMode::UNDEFINED), InputModeToString(InputMode::MOUSE), InputModeToString(InputMode::TOUCH),
            InputModeToString(InputMode::GAME_PAD), InputModeToString(InputMode::MOTION_CONTROLLER)
        };
    }
}
