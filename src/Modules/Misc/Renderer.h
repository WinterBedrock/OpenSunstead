#pragma once
#include "../Module.h"

// Renderer — a settings-only Misc module (like Interface): its toggles apply
// whether or not the module itself is "enabled". Exposes two options ported from
// Latite:
//   * Force VSync         — caps the frame rate to the monitor refresh by forcing
//                           SwapChain::Present(SyncInterval=1) in the DX hook.
//   * Force switch to Dx11 — forces the overlay/game onto the pure-DX11 backend
//                            (Latite behaviour). Enabling removes the live DX12
//                            device so the game recreates as DX11; DISABLING it
//                            requires a game restart (the backend can't be swapped
//                            back live), so we notify the user.
class Renderer : public Module {
public:
    Renderer() : Module("Renderer", "Force VSync / Force DirectX 11", Category::Misc, 0) {
        AddSetting(new BoolSetting("Force VSync", "Cap FPS to the monitor refresh (SyncInterval=1)", false));      // [0]
        AddSetting(new BoolSetting("Force Dx11", "Force the DirectX 11 backend (disabling needs a game restart)", false)); // [1]
    }

    // Applied every frame from OnRender regardless of enabled state (like
    // Interface), so the flags always reflect the current toggle values.
    void OnRender() override;

    bool GetForceVSync() const { return static_cast<BoolSetting*>(mSettings[0])->mValue; }
    bool GetForceDx11()  const { return static_cast<BoolSetting*>(mSettings[1])->mValue; }

private:
    bool mPrevForceDx11 = false;
    bool mInitialized = false;
};
