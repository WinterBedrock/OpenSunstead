#pragma once
#include <Windows.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include "imgui.h"

class RenderManager {
public:
    static RenderManager& GetInstance() {
        static RenderManager instance;
        return instance;
    }

    void Initialize(IDXGISwapChain* swapChain);
    void OnRender(IDXGISwapChain* swapChain);
    void PreResize();
    void Shutdown();
    bool IsInitialized() const { return initialized; }

    // Regular body fonts (Interface setting).
    ImFont* defaultFont = nullptr;      // Product Sans
    ImFont* minecraftFont = nullptr;    // Mojangles
    ImFont* comfortaaFont = nullptr;    // Comfortaa
    ImFont* bangersFont = nullptr;      // Bangers (comic FX labels)
    ImFont* manropeFont = nullptr;      // Manrope Regular
    ImFont* geistFont = nullptr;        // Geist Light
    // Bold variants (ClickGUI category headers, the "Bold Font" toggle).
    ImFont* defaultFontBold = nullptr;
    ImFont* minecraftFontBold = nullptr;
    ImFont* comfortaaFontBold = nullptr;
    ImFont* manropeFontBold = nullptr;  // Manrope Bold
    ImFont* geistFontBold = nullptr;    // Geist Bold

    ImFont* pickFont(int idx, bool bold) const;
    ImFont* iconFont = nullptr;

    // D3D11 access for hooks
    ID3D11Device* GetDevice() const { return pDevice; }
    ID3D11DeviceContext* GetContext() const { return pContext; }
    ID3D12Device* GetD3D12Device() const { return d3d12Device; }
    bool IsDX12() const { return isDX12; }


private:
    RenderManager() = default;

    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;
    
    // For D3D11On12
    bool isDX12 = false;
    ID3D12Device* d3d12Device = nullptr;
    ID3D11On12Device* d3d11On12Device = nullptr;
    
    UINT buffersCounts = 0;
    ID3D11RenderTargetView** renderTargetViews = nullptr;
    ID3D11Resource** wrappedBackBuffers = nullptr;

    bool initialized = false;

};