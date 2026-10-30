#include <Windows.h>
#include <shellapi.h>
#include <thread>
#include <string>
#include <vector>
#include "Core/Sunstead.h"

// Handle to our own injected module (for resource extraction).
HMODULE g_hSelfModule = nullptr;

static std::string GetMinecraftVersion() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);

    DWORD handle;
    DWORD size = GetFileVersionInfoSizeA(path, &handle);
    if (size == 0) return "";

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoA(path, handle, size, data.data())) return "";

    VS_FIXEDFILEINFO* fileInfo = nullptr;
    UINT len = 0;
    if (VerQueryValueA(data.data(), "\\", reinterpret_cast<LPVOID*>(&fileInfo), &len)) {
        const auto major = HIWORD(fileInfo->dwFileVersionMS);
        const auto minor = LOWORD(fileInfo->dwFileVersionMS);
        const auto build = HIWORD(fileInfo->dwFileVersionLS);
        const auto revision = LOWORD(fileInfo->dwFileVersionLS);
        // Full four-part version when a revision exists ("1.26.51.01"):
        // the supported-version patterns prefix-match either form.
        std::string v = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build);
        if (revision != 0) v += "." + std::to_string(revision);
        return v;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Unsupported-version error dialog.
//
// A hand-rolled modal window instead of MessageBox: the user gets a real
// "Open GitHub Releases" button that opens the browser (ShellExecute) right
// from the error, plus the version list and a hint to check GitHub/Discord
// for an update — replacing the old "The DLL will now unload." dead end.
// ---------------------------------------------------------------------------
namespace {

constexpr wchar_t kReleasesUrl[] =
    L"https://github.com/sniffz0ne/Sunstead-Releases/releases";

struct UnsupportedDialogCtx {
    const wchar_t* text = nullptr;
};

void OpenInBrowser(const wchar_t* url) {
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK UnsupportedDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        auto* ctx = reinterpret_cast<UnsupportedDialogCtx*>(
            reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);

        HFONT font = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT fontBold = CreateFontW(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HWND title = CreateWindowW(L"STATIC", L"Unsupported Minecraft version",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 18, 500, 28, hwnd, nullptr, nullptr, nullptr);
        HWND body = CreateWindowW(L"STATIC", ctx->text,
            WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 56, 500, 120, hwnd, nullptr, nullptr, nullptr);
        HWND open = CreateWindowW(L"BUTTON", L"Open GitHub Releases",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 196, 230, 38, hwnd, (HMENU)1001, nullptr, nullptr);
        HWND close = CreateWindowW(L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE, 262, 196, 116, 38, hwnd, (HMENU)1002, nullptr, nullptr);

        SendMessageW(title, WM_SETFONT, (WPARAM)fontBold, TRUE);
        SendMessageW(body, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(open, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(close, WM_SETFONT, (WPARAM)font, TRUE);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == 1001) { OpenInBrowser(kReleasesUrl); return 0; }
        if (id == 1002) { DestroyWindow(hwnd); return 0; }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void ShowUnsupportedVersionDialog(const std::string& gameVersion) {
    const std::string supported = Sunstead::GetSupportedVersionsString();

    std::string narrow = "Sunstead does not support your version: " + gameVersion +
        "\n\nSupported versions: " + supported +
        "\n(every build of that line works)\n\n"
        "Check for updates on GitHub or Discord — a newer Sunstead build may "
        "support your version.";

    // The dialog is a Unicode window; widen the body text.
    std::wstring wide(narrow.size(), L'\0');
    int n = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), (int)narrow.size(), wide.data(), (int)wide.size());
    wide.resize(n > 0 ? (size_t)n : 0);

    UnsupportedDialogCtx ctx{ wide.c_str() };

    WNDCLASSW wc = {};
    wc.lpfnWndProc = UnsupportedDlgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SunsteadUnsupportedVersion";
    RegisterClassW(&wc);

    const int W = 560, H = 280;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, wc.lpszClassName,
        L"Sunstead — Unsupported Version",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        (sw - W) / 2, (sh - H) / 2, W, H,
        nullptr, nullptr, wc.hInstance, &ctx);
    if (!hwnd) {
        // Window creation failed (headless/session oddities): fall back to a
        // plain message box so the failure is still reported.
        MessageBoxA(nullptr, narrow.c_str(), "Unsupported Version", MB_OK | MB_ICONERROR);
        return;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Own a tiny message loop so the dialog stays responsive (modal in effect:
    // nothing else in the DLL has started yet).
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

} // namespace

DWORD WINAPI MainThread(LPVOID lpParam) {
    // Check Minecraft version before initializing
    std::string mcVersion = GetMinecraftVersion();
    if (!mcVersion.empty() && !Sunstead::IsVersionSupported(mcVersion)) {
        ShowUnsupportedVersionDialog(mcVersion);

        // Uninject immediately
        FreeLibraryAndExitThread(static_cast<HMODULE>(lpParam), 0);
        return 0;
    }

    // Initialize client
    Sunstead::GetInstance().Initialize();

    // Main loop waiting for uninject key (END)
    while (Sunstead::GetInstance().IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Unload the client
    Sunstead::GetInstance().Shutdown();

    // Free the library and exit the thread
    FreeLibraryAndExitThread(static_cast<HMODULE>(lpParam), 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        g_hSelfModule = hModule;
        if (auto hThread = CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr)) {
            CloseHandle(hThread);
        }
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
