// ye

#include "Sunstead.h"
#include "AssetDeployer.h"
#include "../Utils/SoundUtils.h"
#include "../resource.h"
#include "Config.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "CrashHandler.h"
#include "UpdateChecker.h"
#include "../Memory/MemoryScanner.h"
#include "../Hooks/HookManager.h"
#include "../Hooks/GameHooks.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Visual/Notifications.h"
#include "../Render/RenderManager.h"
#include "../SDK/SDK.h"
#include "../SDK/Client/ClientInstance.h"
#include <Windows.h>
#include <stdio.h>
#include <thread>
#include <atomic>
#include <string>
#include <cstring>

static HWND g_GameWindow = nullptr;
static WCHAR g_OriginalTitle[256] = L"";
static std::string g_LastServerKey;

static wchar_t g_TitlePending[384] = L"Sunstead";
static std::atomic<int> g_TitleDirty{ 0 };
static std::atomic<bool> g_TitleWorkerRun{ false };
static std::thread g_TitleWorker;

static HWND findGameWindow() {
    HWND w = g_GameWindow;
    if (w && IsWindow(w)) return w;
    w = FindWindowW(nullptr, L"Minecraft");
    if (!w) w = FindWindowW(nullptr, L"Sunstead");
    if (!w) {
        w = FindWindowW(nullptr, L"Sunstead - ?");
        if (!w) w = FindWindowW(nullptr, L"Sunstead Beta");
    }
    if (!w) w = FindWindowW(L"Windows.UI.Core.CoreWindow", nullptr);
    if (!w) w = FindWindowW(L"ApplicationFrameWindow", L"Minecraft");
    if (!w) w = FindWindowW(L"ApplicationFrameWindow", L"Sunstead");
    if (!w) w = FindWindowW(L"ApplicationFrameWindow", L"Sunstead Beta");
    return w;
}

static void buildTitle(wchar_t* out, size_t outN, const char* serverHost) {
    if (!out || outN < 16) return;
    if (serverHost && serverHost[0]) {
        static const wchar_t kPrefix[] = L"Sunstead > ";
        size_t i = 0;
        for (; kPrefix[i] && i + 1 < outN; ++i) out[i] = kPrefix[i];
        for (const char* p = serverHost; *p && i + 1 < outN; ++p) {
            unsigned char c = (unsigned char)*p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == ':' ||
                c == ' ')
                out[i++] = (wchar_t)c;
        }
        out[i] = L'\0';
    } else {
        wcsncpy_s(out, outN, L"Sunstead", _TRUNCATE);
    }
}

static void queueClientTitle(const char* serverHost /* nullable */) {
    buildTitle(g_TitlePending, 384, serverHost);
    g_TitleDirty.store(1, std::memory_order_release);
    LOG_INFO(serverHost && serverHost[0]
        ? (std::string("Title queued: Sunstead > ") + serverHost)
        : std::string("Title queued: Sunstead"), "Server");
}

static void titleWorkerMain() {
    while (g_TitleWorkerRun.load(std::memory_order_acquire)) {
        if (g_TitleDirty.exchange(0, std::memory_order_acq_rel) == 1) {
            HWND w = g_GameWindow;
            if (!w || !IsWindow(w)) {
                w = findGameWindow();
                if (w) g_GameWindow = w;
            }
            if (w && IsWindow(w)) {
                SetWindowTextW(w, g_TitlePending);
            } else {
                g_TitleDirty.store(1, std::memory_order_release);
            }
        }
        Sleep(50);
    }
}

static void startTitleWorker() {
    if (g_TitleWorkerRun.exchange(true)) return;
    g_TitleWorker = std::thread(titleWorkerMain);
}

static void stopTitleWorker() {
    if (!g_TitleWorkerRun.exchange(false)) return;
    if (g_TitleWorker.joinable()) g_TitleWorker.join();
}

void Sunstead::Initialize() {
    static bool s_inited = false;
    if (s_inited) {
        LOG_INFO("Initialize skipped — Sunstead already attached to this process", "main");
        return;
    }
    s_inited = true;

    AssetDeployer::DeployAll();

    isRunning = true;

    Logger::Initialize();
    CrashHandler::Initialize();
    LOG_INFO("Initializing client", "main");

    g_GameWindow = FindWindowW(nullptr, L"Minecraft");
    if (!g_GameWindow) g_GameWindow = FindWindowW(L"Windows.UI.Core.CoreWindow", nullptr);
    if (!g_GameWindow) g_GameWindow = FindWindowW(L"ApplicationFrameWindow", L"Minecraft");
    if (g_GameWindow) {
        GetWindowTextW(g_GameWindow, g_OriginalTitle, 255);
        SetWindowTextW(g_GameWindow, L"Sunstead");
        wcsncpy_s(g_TitlePending, L"Sunstead", _TRUNCATE);
    }

    startTitleWorker();

    g_InjectionState = InjectionState::Scanning;
    g_InjectionSuccessTime = 0.0f;
    g_InjectionStartMs = GetTickCount64();

    MemoryScanner::GetInstance().Initialize();
    ModuleManager::GetInstance().Initialize();
    HookManager::GetInstance().Initialize();

    ConfigManager::Load("default");

    Notifications::AddNotification("Welcome to sunstead <3!");
    g_InjectionState = InjectionState::Success;

    UpdateChecker::GetInstance().Start();

    SoundUtils::PlayResource(IDR_SOUND_INJECT, L".mp3");
}

void Sunstead::UpdateServerPresence() {
    SDK::RakNetConnector* rnc = SDK::RakNetConnector::get();
    std::string dns, ip, featured, display;
    uint16_t port = 0;
    if (rnc) {
        dns = rnc->getDns();
        ip = rnc->getIpAddress();
        featured = rnc->getFeaturedServer();
        port = rnc->getPort();
        if (!featured.empty()) display = featured;
        else display = !dns.empty() ? dns : ip;
    }

    static const char* kSchemes[] = {"https://", "http://" };
    for (const char* sch : kSchemes) {
        const size_t n = strlen(sch);
        if (display.size() > n && _strnicmp(display.c_str(), sch, n) == 0) {
            display.erase(0, n);
            break;
        }
    }
    if (!dns.empty()) {
        for (const char* sch : {"https://", "http://"}) {
            const size_t n = strlen(sch);
            if (dns.size() > n && _strnicmp(dns.c_str(), sch, n) == 0) {
                dns.erase(0, n);
                break;
            }
        }
    }

    std::string safe;
    safe.reserve(display.size());
    for (unsigned char c : display) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == ':' ||
            c == ' ')
            safe.push_back((char)c);
    }

    const std::string key = safe.empty()
        ? std::string()
        : (safe + ":" + std::to_string((unsigned)port) + "|" + featured);

    if (key == g_LastServerKey) return;
    g_LastServerKey = key;

    if (!safe.empty()) {
        LOG_INFO(
            "Server connected: dns=\"" + dns + "\" ip=\"" + ip +
            "\" port=" + std::to_string((unsigned)port) +
            " featured=\"" + featured + "\"",
            "Server");
        Notifications::AddNotification("Connecting to " + safe + "...");
        queueClientTitle(safe.c_str());
    } else {
        LOG_INFO("Server disconnected / no multiplayer session", "Server");
        queueClientTitle(nullptr);
    }
}

void Sunstead::Shutdown() {
    SoundUtils::PlayResource(IDR_SOUND_UNINJECT, L".wav");
    ConfigManager::Save("default");
    UpdateChecker::GetInstance().Shutdown();
    HookManager::GetInstance().Shutdown();
    RenderManager::GetInstance().Shutdown();

    stopTitleWorker();

    if (g_GameWindow && IsWindow(g_GameWindow) && g_OriginalTitle[0]) {
        SetWindowTextW(g_GameWindow, g_OriginalTitle);
    }

    LOG_INFO("Uninjected successfully", "main");
    Logger::Shutdown();
    FreeConsole();
}
