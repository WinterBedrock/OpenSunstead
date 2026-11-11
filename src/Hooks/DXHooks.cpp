#include "DXHooks.h"


#include "../Render/RenderManager.h"

#include "../Core/Config.h"
#include "../Core/Sunstead.h"
#include "../Core/Logger.h"
#include "../Modules/ModuleManager.h"
#include "GameHooks.h"
#include "../Modules/Visual/Chams.h"
#include "kiero.h"
#include "HookEngine.h"
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

void* g_gameSwapChain = nullptr;

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
    if (ppCommandLists && NumCommandLists > 0 && ppCommandLists[0])
        Chams::NoteCommandList(ppCommandLists[0]);
    oExecuteCommandLists(queue, NumCommandLists, ppCommandLists);
}

static HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
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


    if (!RenderManager::GetInstance().IsInitialized()) {
        RenderManager::GetInstance().Initialize(pSwapChain);
    }


    GameHooks::CaptureFrameViewNow();

    if (RenderManager::GetInstance().IsInitialized()) {
        RenderManager::GetInstance().OnRender(pSwapChain);
    }

    return oPresent(pSwapChain, SyncInterval, Flags);
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
        HookEngine::Create(vTable[8], &hkPresent, reinterpret_cast<LPVOID*>(&oPresent));

        HookEngine::Create(vTable[13], &hkResizeBuffers, reinterpret_cast<LPVOID*>(&oResizeBuffers));

        HookEngine::Create(vTable[39], &hkResizeBuffers1, reinterpret_cast<LPVOID*>(&oResizeBuffers1));

        swapChain->Release();
        device->Release();
        context->Release();
    } else {
        LOG_ERROR("Failed to create D3D11 device and swapchain", "DXHooks");
    }

    ID3D12Device* device12 = nullptr;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&device12))) {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* dummyQueue = nullptr;
        if (SUCCEEDED(device12->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), (void**)&dummyQueue))) {
            void** cqTable = *(void***)dummyQueue;
            HookEngine::Create(cqTable[10], &hkExecuteCommandLists, reinterpret_cast<LPVOID*>(&oExecuteCommandLists));
            LOG_INFO("Hooked D3D12 ExecuteCommandLists (index 10)", "DXHooks");
            dummyQueue->Release();
        }
        ID3D12CommandAllocator* dummyAllocator = nullptr;
        ID3D12GraphicsCommandList* dummyList = nullptr;
        void** devTable = *(void***)device12;
        void** listTable = nullptr;
        if (SUCCEEDED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      __uuidof(ID3D12CommandAllocator), (void**)&dummyAllocator)) &&
            SUCCEEDED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dummyAllocator,
                                                  nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&dummyList)) &&
            dummyList) {
            dummyList->Close();
            listTable = *(void***)dummyList;
        }
        Chams::HookDeviceVtables(devTable, listTable);
        if (dummyList) dummyList->Release();
        if (dummyAllocator) dummyAllocator->Release();
        device12->Release();
    }

    DestroyWindow(hWnd);
    UnregisterClassW(L"DummyWindow", wnd.hInstance);
}

void DXHooks::Shutdown() {
    kiero::shutdown();
}
