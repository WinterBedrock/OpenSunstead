#pragma once

namespace SDK { class UIControl; }

class HotbarAnimation {
public:
    static void InstallHooks();
    static void OnScreenFrame(SDK::UIControl* root);
};
