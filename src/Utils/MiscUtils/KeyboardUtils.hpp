#pragma once
#include <string>
#include <Windows.h>

namespace KeyboardUtils {
    inline std::string GetKeyName(int keyId) {
        if (keyId >= 'A' && keyId <= 'Z') {
            return std::string(1, (char)keyId);
        }
        if (keyId >= '0' && keyId <= '9') {
            return std::string(1, (char)keyId);
        }
        switch (keyId) {
            case VK_LBUTTON: return "LClick";
            case VK_RBUTTON: return "RClick";
            case VK_MBUTTON: return "MClick";
            case VK_ESCAPE: return "NONE";
            case VK_TAB: return "TAB";
            case VK_SHIFT: return "SHIFT";
            case VK_CONTROL: return "CTRL";
            case VK_MENU: return "ALT";
            case VK_SPACE: return "SPACE";
            case VK_RETURN: return "ENTER";
            case VK_BACK: return "BACKSPACE";
            case VK_DELETE: return "DELETE";
            case VK_INSERT: return "INSERT";
            case VK_HOME: return "HOME";
            case VK_END: return "END";
            case VK_PRIOR: return "PAGEUP";
            case VK_NEXT: return "PAGEDOWN";
            case VK_CAPITAL: return "CAPS";
            case VK_UP: return "UP";
            case VK_DOWN: return "DOWN";
            case VK_LEFT: return "LEFT";
            case VK_RIGHT: return "RIGHT";
            case VK_F1: return "F1";
            case VK_F2: return "F2";
            case VK_F3: return "F3";
            case VK_F4: return "F4";
            case VK_F5: return "F5";
            case VK_F6: return "F6";
            case VK_F7: return "F7";
            case VK_F8: return "F8";
            case VK_F9: return "F9";
            case VK_F10: return "F10";
            case VK_F11: return "F11";
            case VK_F12: return "F12";
            case VK_OEM_3: return "~";
            case 0: return "NONE";
            default: return "KEY " + std::to_string(keyId);
        }
    }
}
