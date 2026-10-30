#pragma once
#include <Windows.h>

// Renderer module control flags (defined in DXHooks.cpp). The Renderer module
// (Misc) writes these; the DirectX hooks read them.
//   g_ForceVSync : when true, hkPresent forces SyncInterval=1 (vsync ON) and
//                  clears ALLOW_TEARING, capping FPS to the monitor refresh.
//   g_ForceDX11  : when true (read at RenderManager::Initialize), the overlay is
//                  forced onto the pure-DX11 path instead of D3D11On12 — the
//                  Latite "Force DX11" behaviour. Toggling it OFF needs a game
//                  restart (the backend can't be swapped back live).
extern bool g_ForceVSync;
extern bool g_ForceDX11;

namespace DXHooks {
    void Initialize();
    void Shutdown();
}
