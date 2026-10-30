#pragma once
#include <Windows.h>

namespace Config {
    // Default uninject: Ctrl+L, hold 3s (see RenderManager eject progress).
    // UninjectKey is the non-modifier key; Ctrl must also be held.
    inline int UninjectKey = 'L';
    inline int UninjectMod = VK_CONTROL;
    inline float UninjectHoldSeconds = 3.0f;

    // Visual preferences
    inline int GuiKey = VK_INSERT;
}
