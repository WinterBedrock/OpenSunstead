#include "RenderManager.h"
#include "../SDK/Client/GameIcon.h"
#include "../Hooks/DXHooks.h"   // g_ForceVSync / g_ForceDX11
#include "../Modules/ModuleManager.h"
#include "../Modules/Visual/ClickGUI.h"
#include "../Modules/Visual/CommandMenu.h"
#include "../Modules/Visual/Chat.h"
#include "../Commands/CommandManager.h"
#include "HudEditor.h"
#include "../Modules/Visual/NameTags.h"
#include "../Modules/Visual/Notifications.h"
#include "../Utils/MiscUtils/D2D.hpp"
#include "../Core/Config.h"
#include "../Core/ConfigManager.h"
#include "../Core/Logger.h"
#include "../Core/Sunstead.h"
#include "../Core/UpdateChecker.h"
#include "../Utils/MCSDK.h"
#include "../SDK/Network/Packet.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <stdio.h>
#include <filesystem>
#include <string>
#include <fstream>
#include <cmath>
#include <algorithm>
#include "../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../Utils/MiscUtils/ColorUtils.hpp"
#include "../Utils/MiscUtils/KeyboardUtils.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern ID3D12CommandQueue* g_CommandQueue;

WNDPROC oWndProc;
HWND window = NULL;

static std::string getRoamingStatePath() {
    char* appdata = nullptr;
    size_t len = 0;
    _dupenv_s(&appdata, &len, "APPDATA");
    if (!appdata) return "";
    return std::string(appdata) + "\\..\\Local\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\RoamingState\\";
}

// SEH-guarded check: returns true when a local player is present.
// ClientInstance::getLocalPlayer() goes through vtable[0x1F]; on some builds
// (1.26.30.x) that virtual call throws an access violation while the player
// entity is still being set up on world entry. The renderLevel hook already
// swallows that via tryResolveAndSwapPickRange ("getPickRange resolution
// threw (suppressed)"). WndProc must do the same, otherwise the very first
// key press in-world dereferences a bad pointer and crashes the game.
// (Same pattern Coordinates.cpp uses in getCoordinatesSafe.)
static bool hasLocalPlayerSafe() {
    __try {
        SDK::ClientInstance* cInst = SDK::ClientInstance::get();
        if (!cInst) return false;
        SDK::Actor* lp = cInst->getLocalPlayer();
        if (!lp || !sdk_safe_ptr((void*)lp, sizeof(void*))) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Shared with the chat-open branch below: swallow the opening keypress's
// follow-up WM_CHAR so the pre-filled "/" (or any open-key char) never
// reaches the chat's InputText (it would insert into the typed text).
static bool g_suppressNextOpenChar = false;
static void markSuppressNextOpenChar() { g_suppressNextOpenChar = true; }

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    Module* clickGui = ModuleManager::GetInstance().GetModuleByName("ClickGUI");

    // Chat module: OUR open menu is a UI screen just like the ClickGUI — keys
    // must go to ImGui and never reach the game (no movement, no vanilla
    // chat/pause). IsOpen() is a plain flag read, safe at WndProc time.
    const bool chatMenuOpen = ::Chat::IsOpen();
    bool isClickGuiOpen = (clickGui && clickGui->IsEnabled());
    bool isUiOpen = isClickGuiOpen || chatMenuOpen || CommandMenu::IsOpen();   // the HUD editor mode is gone — HUD drag needs no key blocking

    if (isUiOpen) {
        // The opening keypress's WM_CHAR (the '/' that opened the command
        // prefill) must NOT reach the chat input — it would eat the typed
        // text position. Swallow exactly one WM_CHAR after an open.
        if (uMsg == WM_CHAR && g_suppressNextOpenChar) {
            g_suppressNextOpenChar = false;
            return 0;
        }
        // Chat menu open: keys feed OUR hand-rolled input (the ClickGUI-search
        // machinery). They must not reach ImGui (no InputText anymore — its
        // caret was the "|" duplicate) and never the game.
        if (chatMenuOpen && (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP ||
                             uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP ||
                             uMsg == WM_CHAR)) {
            if (uMsg == WM_KEYDOWN) ::Chat::FeedKeyDown((unsigned)wParam);
            return 0;
        }
        // Command palette open: every key feeds the overlay and never reaches
        // the game or the ClickGUI machinery.
        if (CommandMenu::IsOpen() && (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP ||
                                      uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP ||
                                      uMsg == WM_CHAR)) {
            if (uMsg == WM_KEYDOWN) CommandMenu::FeedKeyDown((unsigned)wParam);
            return 0;
        }
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
        SDK::MouseDevice* mouse = SDK::MouseDevice::get();
        if (mouse) {
            mouse->inputs.clear();
            for (int i = 0; i < 7; ++i) {
                mouse->buttonStates[i] = false;
            }
            mouse->dx = 0;
            mouse->dy = 0;
        }
        // Drop mouse clicks, movement, and wheel inputs.
        if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP || uMsg == WM_LBUTTONDBLCLK ||
            uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP || uMsg == WM_RBUTTONDBLCLK ||
            uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP || uMsg == WM_MBUTTONDBLCLK ||
            uMsg == WM_MOUSEMOVE || uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL ||
            uMsg == WM_INPUT || uMsg == WM_SETCURSOR) {
            return 1;
        }

        // Chat menu open: the ImGui window above already saw the event — the
        // game must not see ANY key (ESC is handled inside the chat window;
        // the ClickGUI search/binding machinery below is not applicable).
        if (chatMenuOpen && (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP ||
                             uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP ||
                             uMsg == WM_CHAR)) {
            return 0;
        }

        // "Block Keys" (ClickGUI setting, default OFF): when ON, keyboard input
        // is swallowed so movement/chat/Esc cannot escape to the game while the
        // menu is open; when OFF the keys fall through to Minecraft (walk around
        // with the menu open) and only the GUI bind key still toggles the panel
        // shut via the fall-through handler below. A focused InputText (profile
        // name modal) or a keybind capture force-blocks regardless — typing a
        // name must never also walk the player.
        bool blockKeys = false;
        if (clickGui) {
            for (auto* setting : clickGui->mSettings) {
                if (setting && setting->mName == "Block Keys") {
                    blockKeys = static_cast<BoolSetting*>(setting)->mValue;
                    break;
                }
            }
        }
        if (ClickGUI::IsBinding() || ClickGUI::IsStringEditing()) blockKeys = true;

        if (blockKeys && (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN ||
                          uMsg == WM_SYSKEYUP || uMsg == WM_CHAR)) {
            // While a keybind row is capturing, swallow everything. Otherwise
            // rebinding the menu to the key it already uses would toggle the panel
            // shut on the same press, and Escape (used to clear a bind) would close
            // the menu instead of reaching the capture loop.
            if (ClickGUI::IsBinding()) {
                return 0;
            } else if (clickGui && wParam == clickGui->GetKeybind() && wParam != 0) {
                // Let the Keybind handler below process this to toggle GUI off
            } else if (wParam == VK_ESCAPE) {
                if (uMsg == WM_KEYDOWN) {
                    // First press with a live query just clears the search;
                    // only an empty query closes the panel.
                    if (isClickGuiOpen && ClickGUI::EscapeClearsSearch()) return 0;
                    if (isClickGuiOpen) clickGui->SetEnabled(false);
                }
                return 0;
            } else if (isClickGuiOpen && uMsg == WM_KEYDOWN && wParam != 0) {
                // Live search: the panel has no input field — WM_KEYDOWN goes
                // straight into the module filter (ToUnicode-translated inside
                // ClickGUI, so layout/shift/cyrillic all work). The keybind
                // branch above already let the toggle key through.
                ClickGUI::FeedSearchKeyDown((unsigned int)wParam);
                return 0;
            } else {
                return 0;
            }
        } else if (!blockKeys && isClickGuiOpen &&
                   (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN ||
                    uMsg == WM_SYSKEYUP || uMsg == WM_CHAR)) {
            // Block Keys OFF = "movement mode": ONLY W/A/S/D reach Minecraft so
            // you can walk with the menu open; every other key is swallowed
            // (chat/E/inventory never open). ALL keys still feed the live
            // search — including WASD. The GUI bind key and Insert fall through
            // so the panel can still be closed.
            if (ClickGUI::IsBinding() || ClickGUI::IsStringEditing()) return 0;
            const bool movement = (wParam == 'W' || wParam == 'A' ||
                                  wParam == 'S' || wParam == 'D');
            const bool toggle = (clickGui && wParam == clickGui->GetKeybind() && wParam != 0) ||
                                wParam == VK_INSERT;
            if (uMsg == WM_KEYDOWN && wParam != 0 && !toggle)
                ClickGUI::FeedSearchKeyDown((unsigned int)wParam);
            if (!movement && !toggle) return 0;
            if (movement) return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam); // walk, no close
            // Bind key: falls through to the toggle handler below.
            // Insert (when it is not the bind): explicit close + swallow, so
            // the panel always has a way out in movement mode.
            if (wParam == VK_INSERT && uMsg == WM_KEYDOWN && isClickGuiOpen && clickGui &&
                wParam != clickGui->GetKeybind()) {
                clickGui->SetEnabled(false);
                return 0;
            }
        }
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
        return true;

    if (uMsg == WM_KEYDOWN) {
        // Uninject is Ctrl+L hold 3s — handled in OnRender (not instant keydown).

        // Allow the ClickGUI toggle only in HUD screen (or if already open to close).
        // The HUD editor has no keybind: it is opened from the side button on the
        // pause/chat screens (see Render/HudEditor.cpp).
        bool inHud = (SDK::ScreenView::currentScreenName() == "hud_screen");
        if (clickGui && wParam == clickGui->GetKeybind() && wParam != 0 && (inHud || clickGui->IsEnabled())) {
            clickGui->Toggle();
            Notifications::AddNotification(clickGui->GetName(),
                                          clickGui->IsEnabled() ? NotifType::ToggleOn : NotifType::ToggleOff);
            return 0;
        }

        if (!isUiOpen && SDK::ScreenView::currentScreenName() == "hud_screen") {
            // The command-hints overlay owns the PREFIX key (default '.'): on the
            // HUD screen it ALWAYS opens the Rise-style command hints panel —
            // not a module, so there is no enable gate to trip over. The overlay
            // eats the press, so the vanilla chat never opens for that key. Any
            // other chat-open key still reaches the Chat module below.
            {
                const std::string& prefix = CommandManager::GetInstance().mPrefix;
                int prefixVk = 0;
                if (prefix.size() == 1) {
                    const SHORT vk = VkKeyScanA(prefix[0]);
                    if (vk != -1 && (vk & 0xFF) != 0) prefixVk = vk & 0xFF;
                }
                if (prefixVk != 0 && (int)wParam == prefixVk) {
                    CommandMenu::Open();
                    markSuppressNextOpenChar();
                    return 0;
                }
            }
            // Chat module: hijack the bound chat key — open OUR menu and swallow
            // the press so the vanilla chat screen never opens (HandlesOpenKey
            // itself checks module-on + keybind + hud screen).
            if (::Chat::HandlesOpenKey((int)wParam)) {
                // '/' is the vanilla "open as a command" key: the line must
                // already start with a slash. The bound chat key opens blank.
                ::Chat::instance()->Open(::Chat::PrefillsSlash((int)wParam) ? "/" : "");
                // Suppression flag lives in the isUiOpen block above; reach it
                // through a tiny helper so both branches share the state.
                markSuppressNextOpenChar();
                return 0;
            }
            if (hasLocalPlayerSafe()) {
                ModuleManager::GetInstance().OnKey(wParam);
            }
        }
    }

    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

void RenderManager::Initialize(IDXGISwapChain* pSwapChain) {
    if (initialized) return;

    // Force DX11 (Renderer module, Latite-faithful). If the game is running DX12
    // and the user enabled Force DX11, remove the DX12 device once — Minecraft
    // then recreates its swap chain on the DX11 backend, and our next Initialize
    // takes the pure-DX11 path below. Turning the toggle back OFF cannot swap the
    // live backend, so a game restart is required (the module notifies the user).
    if (g_ForceDX11 && !dx12Removed) {
        ID3D12Device* probe12 = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&probe12)) && probe12) {
            ID3D12Device5* dev5 = nullptr;
            if (SUCCEEDED(probe12->QueryInterface(__uuidof(ID3D12Device5), (void**)&dev5)) && dev5) {
                LOG_INFO("Force DX11: removing DX12 device to force DX11 recreation", "RenderManager");
                dev5->RemoveDevice();
                dev5->Release();
            }
            probe12->Release();
            dx12Removed = true;
            return; // bail; the game will recreate as DX11 and re-enter Initialize
        }
        if (probe12) probe12->Release();
    }

    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice)) && pDevice) {
        isDX12 = false;
    } else if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d3d12Device)) && d3d12Device) {
        isDX12 = true;
        if (!g_CommandQueue) {
            LOG_WARN("DX12 game detected, but CommandQueue is not captured yet. Delaying init", "RenderManager");
            d3d12Device->Release();
            d3d12Device = nullptr;
            return;
        }
    } else {
        LOG_ERROR("Failed to get any device", "RenderManager");
        return;
    }

    DXGI_SWAP_CHAIN_DESC sd;
    pSwapChain->GetDesc(&sd);
    window = sd.OutputWindow;
    if (!oWndProc) {
        oWndProc = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
    }

    if (!isDX12) {
        pDevice->GetImmediateContext(&pContext);
        buffersCounts = 1;
        renderTargetViews = new ID3D11RenderTargetView*[1];
        
        ID3D11Texture2D* pBackBuffer = nullptr;
        if (FAILED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer))) {
            LOG_ERROR("Failed to get D3D11 backbuffer", "RenderManager");
            return;
        }
        pDevice->CreateRenderTargetView(pBackBuffer, NULL, &renderTargetViews[0]);
        pBackBuffer->Release();
    } else {
        buffersCounts = sd.BufferCount;
        renderTargetViews = new ID3D11RenderTargetView*[buffersCounts];
        wrappedBackBuffers = new ID3D11Resource*[buffersCounts];

        UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
        HRESULT hr = D3D11On12CreateDevice(d3d12Device, deviceFlags, nullptr, 0, (IUnknown**)&g_CommandQueue, 1, 0, &pDevice, &pContext, nullptr);
        if (FAILED(hr)) {
            LOG_ERROR("D3D11On12CreateDevice failed. HRESULT: 0x" + std::to_string(hr), "RenderManager");
            return;
        }

        if (FAILED(pDevice->QueryInterface(__uuidof(ID3D11On12Device), (void**)&d3d11On12Device))) {
            LOG_ERROR("Failed to get D3D11On12Device", "RenderManager");
            return;
        }

        D3D11_RESOURCE_FLAGS backBuffer11Flags = {};
        backBuffer11Flags.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        for (UINT i = 0; i < buffersCounts; i++) {
            ID3D12Resource* pBackBuffer12 = nullptr;
            pSwapChain->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&pBackBuffer12);

            d3d11On12Device->CreateWrappedResource(pBackBuffer12, &backBuffer11Flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT, __uuidof(ID3D11Resource), (void**)&wrappedBackBuffers[i]);
            pDevice->CreateRenderTargetView(wrappedBackBuffers[i], nullptr, &renderTargetViews[i]);
            
            pBackBuffer12->Release();
        }
    }

    // Initialize ImGui ONLY AFTER everything else has succeeded 100%
    if (ImGui::GetCurrentContext() == nullptr) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        
        // Load fonts from deployed asset files (AssetDeployer extracts on inject).
        // Also try assets/font/ (repo layout) so local dev picks up bold variants.
        std::string assetDir = ConfigManager::GetSunsteadDir() + "assets\\";
        std::string fontDir  = assetDir + "font\\";
        // System-font FALLBACK, merged into every theme font: the theme TTFs
        // only ship Latin glyphs, so anything else (Cyrillic nicknames in
        // chat/nametags, Greek, currency, typographic punctuation...) would
        // render as ImGui's '?' replacement. Merging Segoe UI into each font
        // (at the destination's implicit reference size, MergeMode + SizePixels 0)
        // bakes those glyphs into the SAME atlas font — FindGlyph resolves
        // them natively everywhere (Chat, NameTags, ClickGUI, ArrayList).
        auto mergeSystemFallback = [&]() {
            static const char* sysPath = []() -> const char* {
                static char p[MAX_PATH] = {};
                UINT n = GetWindowsDirectoryA(p, MAX_PATH);
                if (n > 0 && n + 32 < MAX_PATH) {
                    strcat_s(p + n, sizeof(p) - n, "\\Fonts\\segoeui.ttf");
                    return p;
                }
                return "";
            }();
            if (!sysPath[0] || !std::filesystem::exists(sysPath)) return;
            static const ImWchar mergeRanges[] = {
                0x0100, 0x024F,  // Latin Extended-A/B (Polish, Czech, ...)
                0x0370, 0x03FF,  // Greek
                0x0400, 0x04FF,  // Cyrillic (Russian, Ukrainian, Bulgarian)
                0x0500, 0x052F,  // Cyrillic Supplement
                0x1E00, 0x1EFF,  // Latin Extended Additional (Vietnamese)
                0x2000, 0x206F,  // General Punctuation (quotes, dashes, ellipsis)
                0x20A0, 0x20CF,  // Currency symbols
                0,
            };
            ImFontConfig mergeCfg;
            mergeCfg.MergeMode = true;
            mergeCfg.FontDataOwnedByAtlas = true;
            io.Fonts->AddFontFromFileTTF(sysPath, 0.0f, &mergeCfg, mergeRanges);
        };
        auto loadFont = [&](const char* name, float size) -> ImFont* {
            ImFontConfig fontCfg;
            fontCfg.FontDataOwnedByAtlas = false;
            std::string a = assetDir + name;
            std::string b = fontDir + name;
            if (std::filesystem::exists(a)) {
                ImFont* f = io.Fonts->AddFontFromFileTTF(a.c_str(), size, &fontCfg);
                if (f) mergeSystemFallback();
                return f;
            }
            if (std::filesystem::exists(b)) {
                ImFont* f = io.Fonts->AddFontFromFileTTF(b.c_str(), size, &fontCfg);
                if (f) mergeSystemFallback();
                return f;
            }
            return nullptr;
        };
        {
            // Body (Interface): prefer regular, fall back to Bold if regular missing.
            defaultFont   = loadFont("Product-Sans.ttf", 18.0f);
            if (!defaultFont) defaultFont = loadFont("Product-Sans-Bold.ttf", 18.0f);
            minecraftFont = loadFont("Mojangles.ttf", 18.0f);
            comfortaaFont = loadFont("Comfortaa.ttf", 18.0f);
            bangersFont   = loadFont("Bangers-Regular.ttf", 18.0f);
            // Manrope: Regular as the body font (Bold/SemiBold fallbacks).
            manropeFont   = loadFont("Manrope-Regular.ttf", 18.0f);
            if (!manropeFont) manropeFont = loadFont("Manrope-SemiBold.ttf", 18.0f);
            if (!manropeFont) manropeFont = loadFont("Manrope-Bold.ttf", 18.0f);
            // Geist entry = Light (only Light/Bold sheets exist).
            geistFont     = loadFont("Geist-Light.ttf", 18.0f);

            // Bold variants for section headers / the "Bold Font" toggle.
            defaultFontBold   = loadFont("Product-Sans-Bold.ttf", 18.0f);
            minecraftFontBold = loadFont("Mojangles-Bold.ttf", 18.0f);
            comfortaaFontBold = loadFont("Comfortaa-Bold.ttf", 18.0f);
            manropeFontBold   = loadFont("Manrope-Bold.ttf", 18.0f);
            geistFontBold     = loadFont("Geist-Bold.ttf", 18.0f);

            // Fallbacks if a bold file is missing.
            if (!defaultFontBold)   defaultFontBold   = defaultFont;
            if (!minecraftFontBold) minecraftFontBold = minecraftFont;
            if (!comfortaaFontBold) comfortaaFontBold = comfortaaFont;
            if (!manropeFontBold)   manropeFontBold   = manropeFont;
            if (!geistFontBold)     geistFontBold     = geistFont;
            if (!geistFont)         geistFont         = geistFontBold;
            if (!manropeFont)       manropeFont       = manropeFontBold;
            // Mojangles/Comfortaa have much taller metrics than Product Sans at
            // the same px — the GUI trims them via ImRenderUtils::g_visualFontScale
            // (set per-frame where the font is pushed), not at load time.
        }

        // Icons font (glyph set used by ClickGUI config buttons / search / etc.)
        {
            std::string iconsPath = assetDir + "Icons.ttf";
            if (std::filesystem::exists(iconsPath)) {
                // ASCII + Latin-1 AND the icon font's own PUA icon range (F000..F0AB) —
    // without the second pair the tab/pin glyphs never reach the atlas.
                static const ImWchar iconRanges[] = { 0x0020, 0x00FF, 0xF000, 0xF0AC, 0 };
                ImFontConfig iconCfg;
                iconCfg.FontDataOwnedByAtlas = true;
                iconFont = io.Fonts->AddFontFromFileTTF(iconsPath.c_str(), 20.0f, &iconCfg, iconRanges);
            }
        }
        
        if (!defaultFont) ImGui::GetIO().Fonts->AddFontDefault();

        // Apply Premium ImGui Style
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 8.0f;
        style.FrameRounding = 6.0f;
        style.ChildRounding = 6.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding = 6.0f;
        style.TabRounding = 6.0f;
        
        style.WindowBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.PopupBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
        
        style.ItemSpacing = ImVec2(8, 8);
        style.FramePadding = ImVec2(6, 4);
        style.WindowPadding = ImVec2(10, 10);
        
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text]                   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
        colors[ImGuiCol_WindowBg]               = ImVec4(0.06f, 0.06f, 0.08f, 0.94f);
        colors[ImGuiCol_ChildBg]                = ImVec4(0.08f, 0.08f, 0.10f, 0.94f);
        colors[ImGuiCol_PopupBg]                = ImVec4(0.08f, 0.08f, 0.10f, 0.94f);
        colors[ImGuiCol_Border]                 = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
        colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg]                = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
        colors[ImGuiCol_FrameBgActive]          = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
        colors[ImGuiCol_TitleBg]                = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);
        colors[ImGuiCol_TitleBgActive]          = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
        colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
        colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
        colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
        colors[ImGuiCol_CheckMark]              = ImVec4(0.60f, 0.30f, 0.90f, 1.00f);
        colors[ImGuiCol_SliderGrab]             = ImVec4(0.60f, 0.30f, 0.90f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.70f, 0.40f, 1.00f, 1.00f);
        colors[ImGuiCol_Button]                 = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
        colors[ImGuiCol_ButtonHovered]          = ImVec4(0.60f, 0.30f, 0.90f, 0.80f);
        colors[ImGuiCol_ButtonActive]           = ImVec4(0.60f, 0.30f, 0.90f, 1.00f);
        colors[ImGuiCol_Header]                 = ImVec4(0.60f, 0.30f, 0.90f, 0.50f);
        colors[ImGuiCol_HeaderHovered]          = ImVec4(0.60f, 0.30f, 0.90f, 0.80f);
        colors[ImGuiCol_HeaderActive]           = ImVec4(0.60f, 0.30f, 0.90f, 1.00f);
        colors[ImGuiCol_Separator]              = colors[ImGuiCol_Border];
        colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
        colors[ImGuiCol_SeparatorActive]        = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
        colors[ImGuiCol_ResizeGrip]             = ImVec4(0.60f, 0.30f, 0.90f, 0.20f);
        colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.60f, 0.30f, 0.90f, 0.67f);
        colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.60f, 0.30f, 0.90f, 0.95f);
    }

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(pDevice, pContext);
    
    if (isDX12) {
        LOG_INFO("RenderManager initialized successfully (D3D11On12)", "RenderManager");
    } else {
        LOG_INFO("RenderManager initialized successfully (DX11)", "RenderManager");
    }

    D2D::init(pSwapChain, pDevice);
    initialized = true;
}

ImFont* RenderManager::pickFont(int idx, bool bold) const {
    ImFont* f = nullptr;
    switch (idx) {
    case 1:  f = bold ? minecraftFontBold : minecraftFont; break;
    case 2:  f = bold ? comfortaaFontBold : comfortaaFont; break;
    case 3:  f = bold ? manropeFontBold   : manropeFont;   break;
    case 4:  f = bold ? geistFontBold     : geistFont;     break;
    case 0:
    default: f = bold ? defaultFontBold   : defaultFont;   break;
    }
    // Missing sheet -> the same family in the other weight, then Product Sans.
    if (!f && bold) f = pickFont(idx, false);
    if (!f && !bold) f = pickFont(idx, true);
    if (!f) f = defaultFont ? defaultFont : defaultFontBold;
    return f;
}

void RenderManager::OnRender(IDXGISwapChain* pSwapChain) {
    if (!initialized) return;

    // Synthetic right-click mailbox (FastUse / PearlTrigger / PlaceDelay).
    // Module ticks run on the GAME thread; the game drains the mouse input
    // queue on the thread that pumps window messages — this one. Feeding the
    // clicks HERE puts them on the producing thread with no race, and the
    // game consumes them at the top of the next frame.
    {
        int pending = SDK::g_pendingRightClicks.exchange(0, std::memory_order_relaxed);
        if (pending > 0) {
            if (pending > 4) pending = 4;   // burst cap: one click per frame is plenty
            for (int i = 0; i < pending; ++i)
                if (!SDK::injectRightClick()) break;
        }
    }

    Module* clickGuiEarly = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
    bool isUiOpenEarly = (clickGuiEarly && clickGuiEarly->IsEnabled());
    // Always acquire the D2D backbuffer surface. Blur/ghost effects must work
    // during normal gameplay too (ArrayList, Watermark, HUD counters, MotionBlur),
    // not only while ClickGUI/HudEditor is open. beginRender() is still invoked
    // lazily by addBlur/addBlurOptimized/addGhostFrame, so frames that request no
    // effect only pay for acquiring + releasing the surface (no bitmap copy or
    // blur pass).
    bool needD2DSurface = true;

    UINT backBufferIdx = 0;
    if (isDX12) {
        IDXGISwapChain3* pSwapChain3;
        if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&pSwapChain3))) {
            backBufferIdx = pSwapChain3->GetCurrentBackBufferIndex();
            pSwapChain3->Release();
        }
    }

    IDXGISurface* dxgiSurface = nullptr;
    if (needD2DSurface && !isDX12) {
        ID3D11Texture2D* pBackBuffer = nullptr;
        if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer))) {
            pBackBuffer->QueryInterface(__uuidof(IDXGISurface), (void**)&dxgiSurface);
            pBackBuffer->Release();
        }
    } else if (needD2DSurface) {
        if (wrappedBackBuffers && wrappedBackBuffers[backBufferIdx]) {
            wrappedBackBuffers[backBufferIdx]->QueryInterface(__uuidof(IDXGISurface), (void**)&dxgiSurface);
        }
    }

    if (dxgiSurface) {
        D2D::currentDxgiSurface = dxgiSurface;
    }

    // Real game icon capture (SDK/Client/GameIcon): read the scratch cells the
    // game's ItemRenderer drew this frame BEFORE ImGui paints over them.
    {
        ID3D11Resource* bb = nullptr;
        if (!isDX12) {
            ID3D11Texture2D* t = nullptr;
            if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&t)) && t) {
                SDK::GameIcon::onPresent(t, false);
                t->Release();
            }
        } else if (wrappedBackBuffers && wrappedBackBuffers[backBufferIdx] && d3d11On12Device) {
            bb = wrappedBackBuffers[backBufferIdx];
            d3d11On12Device->AcquireWrappedResources(&bb, 1);
            SDK::GameIcon::onPresent(bb, true);
            d3d11On12Device->ReleaseWrappedResources(&bb, 1);
            pContext->Flush();
        }
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    int fontIdx = 0;
    bool fontBold = false;
    // The Font setting lives on ClickGUI, which owns the appearance block that
    // used to be the settings-only Interface module. Read by name so setting order
    // in the ctor is free to change.
    Module* iface = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
    if (iface) {
        for (auto* setting : iface->mSettings) {
            if (!setting) continue;
            if (setting->mName == "Font")
                fontIdx = static_cast<EnumSetting*>(setting)->mValue;
            else if (setting->mName == "Bold Font")
                fontBold = static_cast<BoolSetting*>(setting)->mValue;
        }
    }

    // Font enum: 0=Product Sans, 1=Minecraft, 2=Comfortaa, 3=Manrope, 4=Geist,
    // weight picked by the "Bold Font" toggle.
    ImFont* pushedFont = pickFont(fontIdx, fontBold);
    if (pushedFont) ImGui::PushFont(pushedFont);
    // No per-font visual scaling: shrinking only the glyphs while the layout
    // constants (row heights, paddings, panel chrome) kept their 18px metrics
    // made every screen read shrivelled. Fonts render at their own metrics;
    // the UI Scale setting scales the whole GUI uniformly if needed.
    ImRenderUtils::g_visualFontScale = 1.0f;

    Module* clickGui = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
    bool isUiOpen = isUiOpenEarly;
    // While signatures are still scanning, the only thing on screen is the
    // loading card. Module OnRender touches the game (and races ConfigManager
    // ::Load), so it stays off until Initialize publishes Success.
    const bool injectionScanning =
        Sunstead::g_InjectionState == Sunstead::InjectionState::Scanning;
    if (!injectionScanning && isUiOpen) {
        SDK::MouseDevice* mouse = SDK::MouseDevice::get();
        if (mouse) {
            mouse->inputs.clear();
        }
    }

    // Server presence: window title "Sunstead > {server}" + connect debug.
    // Skipped during the scan overlay — ClientInstance and the config are not
    // ready, and this frame exists only to show the loading card.
    if (!injectionScanning)
    Sunstead::GetInstance().UpdateServerPresence();

    // Update checker: post-init toast + world-join chat notice (render thread).
    if (!injectionScanning)
    UpdateChecker::GetInstance().OnFrame();

    // Actor pair snapshots are captured AT PUSH TIME inside the
    // CameraSetup::writeMatrices hook (GameHooks -> SDK::CapturePushPairs) —
    // same instant as the camera matrices, so nothing needs refreshing here.

    if (!injectionScanning)
    ModuleManager::GetInstance().OnRender();

    // Command hints overlay (not a module — a client service). Draws only while
    // its fade is live: closed it costs one screen-name string compare.
    static CommandMenu s_commandMenu;
    if (!injectionScanning)
    s_commandMenu.OnRender();

    // HUD editor. Drawn after the modules so its frames land on top of the elements
    // they outline, and driven from here rather than the module loop because it is
    // not a module — it must keep animating, and keep showing its open button on
    // the pause/chat screens, regardless of what is enabled.
    if (!injectionScanning)
    HudEditor::OnRender();

    // ----- Hold-to-eject (Ctrl+L default, 3s) — same card language as inject UI
    {
        static float holdSec = 0.f;
        static float showAnim = 0.f; // 0..1 fade for the card
        const bool ctrl = (GetAsyncKeyState(Config::UninjectMod) & 0x8000) != 0;
        const bool key  = (GetAsyncKeyState(Config::UninjectKey) & 0x8000) != 0;
        const bool holding = ctrl && key && Config::UninjectKey != 0;
        const float need = Config::UninjectHoldSeconds > 0.1f ? Config::UninjectHoldSeconds : 3.f;
        const float dt = ImGui::GetIO().DeltaTime;
        if (holding) {
            holdSec += dt;
            if (holdSec >= need) {
                holdSec = 0.f;
                showAnim = 0.f;
                Sunstead::GetInstance().Stop();
            }
        } else {
            holdSec = 0.f;
        }
        const float targetAnim = holding ? 1.f : 0.f;
        showAnim += (targetAnim - showAnim) * std::min(1.f, dt * 14.f);
        if (showAnim < 0.001f) showAnim = 0.f;
        if (showAnim > 0.999f && holding) showAnim = 1.f;

        if (!injectionScanning && showAnim > 0.02f) {
            const float t = holding ? std::min(1.f, holdSec / need) : 0.f;
            const float alpha = showAnim;
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImVec2 screen = ImGui::GetIO().DisplaySize;

            // Compact top-center card (inject panel palette).
            const float padX = 22.f;
            const float padY = 16.f;
            const float titleSize = 18.f;
            const float subSize = 13.f;
            const float barH = 6.f;
            const float gapTitleBar = 12.f;
            const float gapBarSub = 10.f;
            const float round = 10.f;

            std::string title = "Hold to eject";
            // Reflect the configured key (Settings > KEYS > Eject) rather than the
            // hardcoded default, or the card lies after a rebind.
            std::string sub = "Ctrl + " + KeyboardUtils::GetKeyName(Config::UninjectKey);
            const float titleW = ImRenderUtils::getTextWidth(&title, titleSize);
            const float subW = ImRenderUtils::getTextWidth(&sub, subSize);
            const float barW = 220.f;
            const float contentW = std::max(barW, std::max(titleW, subW));
            const float panelW = contentW + padX * 2.f;
            const float panelH = padY + titleSize + gapTitleBar + barH + gapBarSub + subSize + padY;
            const float panelX = (screen.x - panelW) * 0.5f;
            const float panelY = 36.f + (1.f - alpha) * -18.f; // slide down on show
            ImVec4 panel(panelX, panelY, panelX + panelW, panelY + panelH);

            // Soft backdrop under the whole card (shadow + dark fill + hairline).
            ImRenderUtils::fillShadowRectangle(panel, ImColor(0, 0, 0), alpha * 0.7f, round, 16.f, 0.55f, dl);
            ImRenderUtils::fillRectangle(panel, ImColor(15, 15, 18, 240), alpha, round, dl);
            dl->AddRect(ImVec2(panel.x, panel.y), ImVec2(panel.z, panel.w),
                        ImColor(255, 255, 255, (int)(28 * alpha)), round, 0, 1.25f);

            // Title
            const float titleX = panel.x + (panelW - titleW) * 0.5f;
            const float titleY = panel.y + padY;
            ImRenderUtils::drawText(ImVec2(titleX, titleY), title,
                                    ImColor(255, 255, 255, 255), titleSize, alpha, true, dl);

            // Progress track + themed fill
            const float pbX = panel.x + (panelW - barW) * 0.5f;
            const float pbY = titleY + titleSize + gapTitleBar;
            dl->AddRectFilled(ImVec2(pbX, pbY), ImVec2(pbX + barW, pbY + barH),
                              ImColor(255, 255, 255, (int)(28 * alpha)), 3.f);
            if (t > 0.001f) {
                ImColor fill = ColorUtils::getThemedColor(0);
                fill.Value.w *= alpha;
                dl->AddRectFilled(ImVec2(pbX, pbY), ImVec2(pbX + barW * t, pbY + barH), fill, 3.f);
            }

            // Hint under the bar
            const float subX = panel.x + (panelW - subW) * 0.5f;
            const float subY = pbY + barH + gapBarSub;
            ImRenderUtils::drawText(ImVec2(subX, subY), sub,
                                    ImColor(160, 160, 160, 255), subSize, alpha, true, dl);
        }
    }

    // ----- Start-screen quick buttons (ImGui overlay) -----------------------
    // Three frosted buttons at the bottom of the main menu. Settings opens the
    // ClickGUI on its Settings tab; Configs and Change Logs open the matching
    // tabs. Only on start_screen / pause_screen, and only while the in-game UI
    // (ClickGUI or the HUD editor) is not already open. "Menu Buttons" on the
    // ClickGUI module (default on) toggles them.
    {
        const std::string& sn = SDK::ScreenView::currentScreenName();
        bool menuButtonsOn = true;
        if (clickGui) {
            for (auto* setting : clickGui->mSettings) {
                if (setting && setting->mName == "Menu Buttons") {
                    menuButtonsOn = static_cast<BoolSetting*>(setting)->mValue;
                    break;
                }
            }
        }
        const bool showMenuBtns = menuButtonsOn && !isUiOpen &&
            (sn == "start_screen" || sn == "pause_screen");

        // 0 = off-screen below, 1 = settled. Animates both appear AND fall-out.
        static float menuBtnAnim = 0.0f;
        static bool  wasMenu = false;
        if (showMenuBtns && !wasMenu) menuBtnAnim = 0.0f; // re-enter: pop from below
        wasMenu = showMenuBtns;
        float target = showMenuBtns ? 1.0f : 0.0f;
        float speed = showMenuBtns ? 0.14f : 0.18f; // fall-out slightly snappier
        menuBtnAnim += (target - menuBtnAnim) * speed;
        if (menuBtnAnim < 0.001f) menuBtnAnim = 0.0f;
        if (menuBtnAnim > 0.999f && showMenuBtns) menuBtnAnim = 1.0f;

        auto easeOutBack = [](float x) {
            const float c1 = 1.70158f, c3 = c1 + 1.0f;
            float xm = (x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x)) - 1.0f;
            return 1.0f + c3 * xm * xm * xm + c1 * xm * xm;
        };
        auto easeInBack = [](float x) {
            const float c1 = 1.70158f, c3 = c1 + 1.0f;
            float t = (x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x));
            return c3 * t * t * t - c1 * t * t;
        };

        // Keep drawing while animating out (fall-out) even if screen already changed.
        if (!injectionScanning && menuBtnAnim > 0.01f) {
            ImGuiIO& io2 = ImGui::GetIO();
            ImVec2 ds = io2.DisplaySize;

            const float bw = 150.0f, bh = 38.0f, gap = 16.0f;
            const float totalW = bw * 3.0f + gap * 2.0f;
            float bx = (ds.x - totalW) * 0.5f;
            float byRest = ds.y - bh - 46.0f;

            ImVec2 mp = io2.MousePos;
            bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && showMenuBtns;

            // Separate Icons.ttf glyph per button. `tab` indexes ClickGUI's rail:
            // 0 = modules, 1 = Settings, 2 = Profiles, 3 = Themes, 4 = Changelog.
            struct B { const char* label; const char* glyph; ImColor accent; int tab; };
            B btns[3] = {
                { "Settings",    "", ImColor(102, 166, 255, 255), 1 },
                { "Configs",     "4", ImColor(140, 217, 140, 255), 2 },
                { "Changelog",   "", ImColor(214, 162, 255, 255), 4 },
            };

            static float hoverAnim[3] = { 0.0f, 0.0f, 0.0f };
            ImFont* icons = iconFont;

            for (int i = 0; i < 3; ++i) {
                B& b = btns[i];
                float tLocal;
                if (showMenuBtns) {
                    tLocal = std::clamp(menuBtnAnim * 1.15f - i * 0.08f, 0.0f, 1.0f);
                    tLocal = easeOutBack(tLocal);
                } else {
                    // Fall-out: reverse stagger, drop down with easeInBack.
                    float raw = std::clamp(menuBtnAnim * 1.15f - (2 - i) * 0.08f, 0.0f, 1.0f);
                    float fall = easeInBack(1.0f - raw);
                    tLocal = 1.0f - fall;
                }
                float yOff = (1.0f - tLocal) * (bh + 110.0f);
                float rx = bx + i * (bw + gap);
                ImVec4 r(rx, byRest + yOff, rx + bw, byRest + yOff + bh);

                float a = std::clamp(menuBtnAnim * tLocal, 0.0f, 1.0f);
                bool hov = showMenuBtns && tLocal > 0.85f &&
                           mp.x >= r.x && mp.x <= r.z && mp.y >= r.y && mp.y <= r.w;
                hoverAnim[i] += ((hov ? 1.0f : 0.0f) - hoverAnim[i]) * 0.18f;
                float ha = hoverAnim[i];

                // Rise-style frosted plate: a backdrop BLUR first (paints
                // blurred world behind the button — the background list
                // replays before any of this geometry), then a much lighter
                // translucent plate over it. The old near-opaque dark fill
                // read as a heavy slab ("слишком жирная"); frosted glass is
                // the Rise menu-button language.
                ImDrawList* bgList = ImGui::GetBackgroundDrawList();
                if (bgList && a > 0.05f) {
                    D2D::addBlurOptimized(bgList, 9.0f,
                        std::optional<ImVec4>(ImVec4(r.x, r.y, r.z, r.w)), 9.0f);
                }
                ImRenderUtils::fillShadowRectangle(r, ImColor(0, 0, 0, 255),
                                                   (0.20f + 0.12f * ha) * a, 9.0f, 12.0f);
                int base = 18 + (int)(10 * ha);
                ImRenderUtils::fillRectangle(r, ImColor(base, base + 2, base + 6,
                                                       (int)(110 * a) + (int)(45 * ha)),
                                             0.78f + 0.12f * ha, 9.0f);
                ImVec4 sheen(r.x, r.y, r.z, r.y + bh * 0.45f);
                ImRenderUtils::fillRectangle(sheen, ImColor(255, 255, 255, (int)(255 * a)),
                                             0.04f + 0.05f * ha, 9.0f);
                float pad = 12.0f - 6.0f * ha;
                ImVec4 underline(r.x + pad, r.w - 4.0f, r.z - pad, r.w - 2.0f);
                ImColor ul = b.accent; ul.Value.w = (0.5f + 0.5f * ha) * a;
                ImRenderUtils::fillRectangle(underline, ul, 1.0f, 2.0f);
                if (ha > 0.01f) {
                    ImGui::GetForegroundDrawList()->AddRect(
                        ImVec2(r.x, r.y), ImVec2(r.z, r.w),
                        ImColor(b.accent.Value.x, b.accent.Value.y, b.accent.Value.z, 0.5f * ha * a),
                        9.0f, 0, 1.5f);
                }

                // Per-button Icons.ttf glyph (k / l / m — swap to real icon codepoints later).
                std::string lbl = b.label;
                float ts = 19.0f;
                float iconSz = 18.0f;
                float iconGap = 6.0f;
                float tw = ImRenderUtils::getTextWidth(&lbl, ts);
                float iconW = icons ? iconSz : 0.f;
                float groupW = (icons ? iconW + iconGap : 0.f) + tw;
                float startX = r.x + (bw - groupW) * 0.5f;
                float textY = r.y + (bh - ts) * 0.5f;
                if (icons && b.glyph && b.glyph[0]) {
                    ImGui::GetForegroundDrawList()->AddText(
                        icons, iconSz,
                        ImVec2(startX, r.y + (bh - iconSz) * 0.5f),
                        ImColor(235, 238, 245, (int)(255 * a)),
                        b.glyph);
                    startX += iconW + iconGap;
                }
                ImRenderUtils::drawText(ImVec2(startX, textY), lbl,
                    ImColor(235, 238, 245, (int)(255 * a)), ts, 0.85f + 0.15f * ha, true);

                if (hov && clicked) {
                    Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
                    if (gui) {
                        ClickGUI::SetTab(b.tab);
                        if (!gui->IsEnabled()) gui->SetEnabled(true);
                    }
                }
            }
        }
    }


    // Draw Injection Loading / Success overlay
    if (Sunstead::g_InjectionState != Sunstead::InjectionState::Finished) {
        float time = ImGui::GetTime();
        if (Sunstead::g_InjectionState == Sunstead::InjectionState::Success && Sunstead::g_InjectionSuccessTime == 0.0f) {
            Sunstead::g_InjectionSuccessTime = time;
        }

        float alpha = 1.0f;
        if (Sunstead::g_InjectionState == Sunstead::InjectionState::Success && Sunstead::g_InjectionSuccessTime > 0.0f) {
            float elapsed = time - Sunstead::g_InjectionSuccessTime;
            if (elapsed > 0.4f) {
                alpha = 1.0f - (elapsed - 0.4f) / 0.25f; // snappy checkmark: hold 0.4s, fade 0.25s
                if (alpha <= 0.0f) {
                    alpha = 0.0f;
                    Sunstead::g_InjectionState = Sunstead::InjectionState::Finished;
                }
            }
        }

        if (Sunstead::g_InjectionState != Sunstead::InjectionState::Finished) {
            ImVec2 screen = ImGui::GetIO().DisplaySize;
            auto drawList = ImGui::GetBackgroundDrawList();

            // Background blur & darkening overlay
            D2D::addBlur(drawList, 8.0f * alpha, std::optional<ImVec4>(ImVec4(0, 0, screen.x, screen.y)), 0.0f);
            drawList->AddRectFilled(ImVec2(0, 0), ImVec2(screen.x, screen.y), IM_COL32(0, 0, 0, (int)(150 * alpha)));

            // Panel in the center
            float panelW = 320.0f;
            float panelH = 180.0f;
            ImVec4 panelRect((screen.x - panelW) / 2.0f, (screen.y - panelH) / 2.0f, (screen.x + panelW) / 2.0f, (screen.y + panelH) / 2.0f);

            // Draw panel card
            ImRenderUtils::fillShadowRectangle(panelRect, ImColor(0, 0, 0), alpha * 0.75f, 12.0f, 20.0f);
            ImRenderUtils::fillRectangle(panelRect, ImColor(15, 15, 18, 240), alpha, 12.0f);
            drawList->AddRect(ImVec2(panelRect.x, panelRect.y), ImVec2(panelRect.z, panelRect.w), ImColor(255, 255, 255, (int)(30 * alpha)), 12.0f, 0, 1.5f);

            // Center of the spinner/checkmark
            ImVec2 center(screen.x / 2.0f, panelRect.y + 55.0f);

            if (Sunstead::g_InjectionState == Sunstead::InjectionState::Scanning) {
                // Fill-up bars animation: a row of vertical bars that fill
                // bottom-to-top in a flowing wave, plus a wide progress bar
                // underneath that tracks an auto-advancing progress value.
                constexpr int numBars = 7;
                float barW = 8.0f;
                float barGap = 7.0f;
                float rowW = numBars * barW + (numBars - 1) * barGap;
                float startX = center.x - rowW * 0.5f;
                float maxH = 34.0f;

                for (int i = 0; i < numBars; ++i) {
                    // Each bar fills with a phase offset so it looks like a wave.
                    float phase = time * 2.6f - i * 0.55f;
                    // Normalized fill amount in [0,1] with smooth easing.
                    float fill = 0.5f + 0.5f * sinf(phase);
                    fill = 1.0f - (1.0f - fill) * (1.0f - fill) * (1.0f - fill); // ease-out cubic

                    float h = maxH * fill;
                    float x = startX + i * (barW + barGap);

                    // Track (dim background)
                    ImColor trackCol(255, 255, 255, (int)(28 * alpha));
                    drawList->AddRectFilled(ImVec2(x, center.y - maxH * 0.5f),
                                            ImVec2(x + barW, center.y + maxH * 0.5f),
                                            trackCol, 3.0f);

                    // Filled portion — themed color, brighter bars lead the wave.
                    ImColor barCol = ColorUtils::getThemedColor(i * 40);
                    barCol.Value.w *= (alpha * (0.45f + 0.55f * fill));
                    drawList->AddRectFilled(ImVec2(x, center.y + maxH * 0.5f - h),
                                            ImVec2(x + barW, center.y + maxH * 0.5f),
                                            barCol, 3.0f);
                }

                // Wide progress bar that steadily advances during scanning.
                float pbW = 220.0f;
                float pbH = 5.0f;
                float pbX = center.x - pbW * 0.5f;
                float pbY = center.y + maxH * 0.5f + 16.0f;

                // Continuous progress across the WHOLE init, including the frames
                // before ImGui existed. Wall-clock from Initialize(), so the bar
                // is already part-way filled on the first drawn frame instead of
                // restarting at zero (and then jumping straight to the checkmark).
                const unsigned long long scanStart = Sunstead::g_InjectionStartMs;
                const float elapsed = scanStart
                    ? (float)(GetTickCount64() - scanStart) / 1000.0f
                    : time;
                float prog = 1.0f - expf(-elapsed * 0.55f); // monotonic 0 -> ~1

                drawList->AddRectFilled(ImVec2(pbX, pbY), ImVec2(pbX + pbW, pbY + pbH),
                                        ImColor(255, 255, 255, (int)(30 * alpha)), 2.5f);
                ImColor pbCol = ColorUtils::getThemedColor(time * 120.0f);
                pbCol.Value.w *= alpha;
                drawList->AddRectFilled(ImVec2(pbX, pbY), ImVec2(pbX + pbW * prog, pbY + pbH),
                                        pbCol, 2.5f);

                // Pulse text below the bars
                std::string loadingText = "Initializing Sunstead...";
                float textW = ImRenderUtils::getTextWidth(&loadingText, 18.0f);
                float pulse = 0.6f + 0.4f * sin(time * 4.0f);
                ImColor textCol(255, 255, 255, (int)(255 * alpha * pulse));
                ImRenderUtils::drawText(ImVec2((screen.x - textW) / 2.0f, panelRect.w - 55.0f), loadingText, textCol, 18.0f, alpha, true);

                // Scanning progress
                std::string subText = "Scanning game signatures...";
                float subW = ImRenderUtils::getTextWidth(&subText, 14.0f);
                ImRenderUtils::drawText(ImVec2((screen.x - subW) / 2.0f, panelRect.w - 30.0f), subText, ImColor(160, 160, 160, (int)(255 * alpha)), 14.0f, alpha, true);
            } else {
                // Success: draw checkmark
                float radius = 22.0f;
                
                // Green/Cyan glowing circle
                ImColor successCol(46, 204, 113, (int)(255 * alpha));
                drawList->AddCircle(center, radius, successCol, 30, 2.5f);
                
                // Draw checkmark lines
                ImVec2 pCheck1(center.x - 8.0f, center.y);
                ImVec2 pCheck2(center.x - 2.0f, center.y + 6.0f);
                ImVec2 pCheck3(center.x + 8.0f, center.y - 4.0f);
                drawList->AddLine(pCheck1, pCheck2, successCol, 3.0f);
                drawList->AddLine(pCheck2, pCheck3, successCol, 3.0f);

                // Success text
                std::string successText = "Sunstead Ready!";
                float textW = ImRenderUtils::getTextWidth(&successText, 20.0f);
                ImRenderUtils::drawText(ImVec2((screen.x - textW) / 2.0f, panelRect.w - 55.0f), successText, successCol, 20.0f, alpha, true);

                std::string subText = "Enjoy playing";
                float subW = ImRenderUtils::getTextWidth(&subText, 14.0f);
                ImRenderUtils::drawText(ImVec2((screen.x - subW) / 2.0f, panelRect.w - 30.0f), subText, ImColor(160, 160, 160, (int)(255 * alpha)), 14.0f, alpha, true);
            }
        }
    }

    if (pushedFont) ImGui::PopFont();
    ImRenderUtils::g_visualFontScale = 1.0f;   // no trim outside the GUI font scope

    ImGui::Render();

    if (isDX12) {
        d3d11On12Device->AcquireWrappedResources(&wrappedBackBuffers[backBufferIdx], 1);
    }

    pContext->OMSetRenderTargets(1, &renderTargetViews[backBufferIdx], NULL);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // Safe to free D3D head textures that modules deferred during this frame
    // (NameTags toggle/prune). Must run AFTER the draw list is submitted.
    NameTags::flushDeferredHeadReleases();

    if (isDX12) {
        d3d11On12Device->ReleaseWrappedResources(&wrappedBackBuffers[backBufferIdx], 1);
        pContext->Flush();
    }

    if (dxgiSurface) {
        if (D2D::isRenderingFrame) {
            D2D::endRender();
        }
        dxgiSurface->Release();
        D2D::currentDxgiSurface = nullptr;
    }
}

void RenderManager::PreResize() {
    if (!initialized) return;
    SDK::GameIcon::clear();
    
    D2D::shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();

    if (pContext) {
        if (isDX12) {
            ID3D11RenderTargetView* nullViews[] = { nullptr, nullptr, nullptr };
            pContext->OMSetRenderTargets(3, nullViews, nullptr);
        } else {
            ID3D11RenderTargetView* nullViews[] = { nullptr };
            pContext->OMSetRenderTargets(1, nullViews, nullptr);
        }
        pContext->Flush();
    }
    
    if (renderTargetViews) {
        for (UINT i = 0; i < buffersCounts; i++) {
            if (renderTargetViews[i]) {
                renderTargetViews[i]->Release();
            }
        }
        delete[] renderTargetViews;
        renderTargetViews = nullptr;
    }
    
    if (isDX12) {
        if (wrappedBackBuffers) {
            for (UINT i = 0; i < buffersCounts; i++) {
                if (wrappedBackBuffers[i]) {
                    wrappedBackBuffers[i]->Release();
                }
            }
            delete[] wrappedBackBuffers;
            wrappedBackBuffers = nullptr;
        }
        if (d3d11On12Device) { d3d11On12Device->Release(); d3d11On12Device = nullptr; }
        if (d3d12Device) { d3d12Device->Release(); d3d12Device = nullptr; }
    }
    
    if (pContext) { pContext->Release(); pContext = nullptr; }
    if (pDevice) { pDevice->Release(); pDevice = nullptr; }
    
    initialized = false;
}

void RenderManager::Shutdown() {
    if (!initialized) return;
    SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    D2D::shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();

    if (pContext) {
        if (isDX12) {
            ID3D11RenderTargetView* nullViews[] = { nullptr, nullptr, nullptr };
            pContext->OMSetRenderTargets(3, nullViews, nullptr);
        } else {
            ID3D11RenderTargetView* nullViews[] = { nullptr };
            pContext->OMSetRenderTargets(1, nullViews, nullptr);
        }
        pContext->Flush();
    }
    
    if (renderTargetViews) {
        for (UINT i = 0; i < buffersCounts; i++) if (renderTargetViews[i]) renderTargetViews[i]->Release();
        delete[] renderTargetViews;
        renderTargetViews = nullptr;
    }
    
    if (isDX12) {
        if (wrappedBackBuffers) {
            for (UINT i = 0; i < buffersCounts; i++) if (wrappedBackBuffers[i]) wrappedBackBuffers[i]->Release();
            delete[] wrappedBackBuffers;
            wrappedBackBuffers = nullptr;
        }
        if (d3d11On12Device) { d3d11On12Device->Release(); d3d11On12Device = nullptr; }
        if (d3d12Device) { d3d12Device->Release(); d3d12Device = nullptr; }
    }
    
    if (pContext) { pContext->Release(); pContext = nullptr; }
    if (pDevice) { pDevice->Release(); pDevice = nullptr; }
    initialized = false;
}