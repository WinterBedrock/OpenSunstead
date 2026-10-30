#include "DXHooks.h"

#include "HookManager.h"
#include "../Render/RenderManager.h"
#include "../Memory/OffsetValidator.h"
#include "../Core/Config.h"
#include "../Core/Sunstead.h"
#include "../Core/Logger.h"
#include "../Modules/ModuleManager.h"
#include "GameHooks.h"
#include "../Modules/Visual/Chams.h"
#include "kiero.h"
#include "MinHook.h"
#include <dxgi.h>
#include <dxgi1_3.h>
#include <d3d11.h>
#include <d3d12.h>
#include <stdio.h>
#include <string>

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif

extern ID3D12CommandQueue* g_CommandQueue = nullptr;

// The game's swapchain as last seen at Present + the SEH-guarded backbuffer
// index probe shared by the camera queue producer and consumer (FrameView.h).
void* g_gameSwapChain = nullptr;

// POD-only (SEH): QI the flip-model swapchain to IDXGISwapChain3 and read the
// backbuffer index. Returns 0xFFFFFFFF when unavailable (non-flip swapchain,
// dead object, pre-init). Called once per Present and once per camera push.
unsigned QueryPresentBackBufferIndex() {
    __try {
        IDXGISwapChain* sc = (IDXGISwapChain*)g_gameSwapChain;
        if (!sc) return 0xFFFFFFFFu;
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) || !sc3)
            return 0xFFFFFFFFu;
        const UINT idx = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
        return (unsigned)idx;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFFu;
    }
}

// Renderer-module control flags (declared in DXHooks.h).
bool g_ForceVSync = false;
bool g_ForceDX11  = false;

typedef void(__stdcall* ExecuteCommandLists_t)(ID3D12CommandQueue* queue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);
static ExecuteCommandLists_t oExecuteCommandLists = nullptr;

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
static Present_t oPresent = nullptr;

typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
static ResizeBuffers_t oResizeBuffers = nullptr;

typedef HRESULT(__stdcall* ResizeBuffers1_t)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, const UINT *pCreationNodeMask, IUnknown *const *ppPresentQueue);
static ResizeBuffers1_t oResizeBuffers1 = nullptr;

static void __stdcall hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    if (!g_CommandQueue && queue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        g_CommandQueue = queue;
        LOG_INFO("Captured D3D12 CommandQueue (DIRECT)", "DXHooks");
    }
    // Chams observes one live graphics command list per submit so it can hook
    // SetPipelineState exactly once (the call itself is a two-branch no-op
    // once installed). Lists submitted every frame means pre-inject lists are
    // never missed.
    if (ppCommandLists && NumCommandLists > 0 && ppCommandLists[0])
        Chams::NoteCommandList(ppCommandLists[0]);
    oExecuteCommandLists(queue, NumCommandLists, ppCommandLists);
}

static HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // The Present detour is installed BEFORE the game-side hooks exist, so the
    // first frames can fire while the inject thread is still scanning
    // signatures / building modules / loading the config. Doing ANY of the
    // overlay work in that window is the "inject comes up half-dead" bug:
    // RenderManager::Initialize swaps the WndProc, the UI reads modules whose
    // settings ConfigManager::Load then mutates concurrently, and the first
    // frames race the tail of init. Wait until GameHooks is fully ready.
    // Hooks are still being installed, but the loading overlay has to be on
    // screen for the whole scan — otherwise the player only ever sees the
    // success checkmark (init finishes, then the first Present draws Success
    // for one frame and fades it). Draw ONLY the overlay here: no module
    // ticks, no WndProc swap, no config. RenderManager::Initialize is safe to
    // call early — it only builds the ImGui device.
    if (!GameHooks::HooksReady()) {
        if (Sunstead::g_InjectionStartMs != 0 &&
            Sunstead::g_InjectionState == Sunstead::InjectionState::Scanning) {
            if (!RenderManager::GetInstance().IsInitialized())
                RenderManager::GetInstance().Initialize(pSwapChain);
            if (RenderManager::GetInstance().IsInitialized())
                RenderManager::GetInstance().OnRender(pSwapChain);
        }
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    // Swapchain + backbuffer index fist
    const unsigned bbNow = QueryPresentBackBufferIndex();
    GameHooks::BeginPresentFrame(bbNow);

    // One-shot offset validation: probes every OffsetManager field against the
    // live object graph and logs any that read implausible memory. Validate()
    // returns -1 until the game has created ClientInstance (the first Present
    // fires on the loading screen, before GameHooks scans signatures), so this
    // retries every frame until the report is final.
    static bool s_offsetsValidated = false;
    if (!s_offsetsValidated && OffsetValidator::GetInstance().Validate() >= 0) {
        s_offsetsValidated = true;
    }

    if (!RenderManager::GetInstance().IsInitialized()) {
        RenderManager::GetInstance().Initialize(pSwapChain);
    }

    // Streamer Mode (NameProtect)
    uintptr_t localPlayer = HookManager::GetInstance().GetLocalPlayer(); 
    if (localPlayer) {
        std::string* nameTag = reinterpret_cast<std::string*>(localPlayer + 3008);
        if (nameTag && !nameTag->empty() && *nameTag != "SunsteadPlayer") {
            *nameTag = "SunsteadPlayer";
        }
    }

    // Snapshot the current-frame camera transform BEFORE drawing overlays. The
    // game writes GameRenderer::lastViewMatrix only after the world-render call
    // returns (verified in the binary), so this Present-time capture is the
    // first point where the matrix matches the world just rendered. Capturing
    // it inside renderLevel instead read the previous frame's matrix, which is
    // what made ESP/Tracers/NameTags boxes swim/lag while rotating the camera.
    GameHooks::CaptureFrameViewNow();

    if (RenderManager::GetInstance().IsInitialized()) {
        RenderManager::GetInstance().OnRender(pSwapChain);
    }

    // Force VSync (Renderer module). When on, present with SyncInterval=1 and
    // strip the ALLOW_TEARING present flag so the frame is synced to the monitor
    // refresh regardless of the game's own vsync option. Mirror of Latite's
    // force-DISABLE-vsync path (which sets SyncInterval=0 + ALLOW_TEARING).
    UINT syncInterval = SyncInterval;
    UINT presentFlags = Flags;
    if (g_ForceVSync) {
        syncInterval = 1;
        presentFlags &= ~DXGI_PRESENT_ALLOW_TEARING;
    }

    return oPresent(pSwapChain, syncInterval, presentFlags);
}

static HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    RenderManager::GetInstance().PreResize();
    return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

static HRESULT __stdcall hkResizeBuffers1(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, const UINT *pCreationNodeMask, IUnknown *const *ppPresentQueue) {
    RenderManager::GetInstance().PreResize();
    return oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
}

void DXHooks::Initialize() {
    WNDCLASSEXW wnd{ 0 };
    wnd.cbSize = sizeof(WNDCLASSEXW);
    wnd.lpfnWndProc = DefWindowProcW;
    wnd.lpszClassName = L"DummyWindow";
    wnd.hInstance = GetModuleHandleW(nullptr);
    RegisterClassExW(&wnd);

    HWND hWnd = CreateWindowExW(0, L"DummyWindow", L"", 0, 0, 0, 0, 0, nullptr, nullptr, wnd.hInstance, nullptr);
    if (!hWnd) {
        LOG_ERROR("Failed to create dummy window", "DXHooks");
        return;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &swapChain, &device, &featureLevel, &context);

    if (SUCCEEDED(hr) && swapChain) {
        void** vTable = *(void***)swapChain;
        LOG_INFO("Hooking Present (index 8) & ResizeBuffers (index 13)", "DXHooks");
        MH_CreateHook(vTable[8], &hkPresent, reinterpret_cast<LPVOID*>(&oPresent));
        MH_EnableHook(vTable[8]);

        MH_CreateHook(vTable[13], &hkResizeBuffers, reinterpret_cast<LPVOID*>(&oResizeBuffers));
        MH_EnableHook(vTable[13]);

        MH_CreateHook(vTable[39], &hkResizeBuffers1, reinterpret_cast<LPVOID*>(&oResizeBuffers1));
        MH_EnableHook(vTable[39]);
        
        swapChain->Release();
        device->Release();
        context->Release();
    } else {
        LOG_ERROR("Failed to create D3D11 device and swapchain", "DXHooks");
    }

    // Try DX12 to get command queue
    ID3D12Device* device12 = nullptr;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&device12))) {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* dummyQueue = nullptr;
        if (SUCCEEDED(device12->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), (void**)&dummyQueue))) {
            void** cqTable = *(void***)dummyQueue;
            MH_CreateHook(cqTable[10], &hkExecuteCommandLists, reinterpret_cast<LPVOID*>(&oExecuteCommandLists));
            MH_EnableHook(cqTable[10]);
            LOG_INFO("Hooked D3D12 ExecuteCommandLists (index 10)", "DXHooks");
            dummyQueue->Release();
        }
        device12->Release();
    }

    DestroyWindow(hWnd);
    UnregisterClassW(L"DummyWindow", wnd.hInstance);
}

void DXHooks::Shutdown() {
    kiero::shutdown();
}
