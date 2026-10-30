#include "UpdateChecker.h"
#include "Sunstead.h"
#include "Logger.h"
#include "../Modules/Visual/Notifications.h"
#include "../Modules/ModuleManager.h"
#include "../SDK/SDK.h"
#include <Windows.h>
#include <winhttp.h>
#include <cctype>
#include <cstring>

static std::string NormalizeVersion(std::string v) {
    size_t a = 0, b = v.size();
    auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (a < b && isWs(v[a])) ++a;
    while (b > a && isWs(v[b - 1])) --b;
    v = v.substr(a, b - a);
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    return v;
}

static bool FetchCurrentTxt(std::string& out) {
    const wchar_t* kHost = L"raw.githubusercontent.com";
    const wchar_t* kPath = L"/sniffz0ne/Sunstead-Releases/refs/heads/main/current.txt";

    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool secure = (attempt == 0);
        const INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

            HINTERNET session = WinHttpOpen(L"Sunstead/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) continue;
        WinHttpSetTimeouts(session, 2000, 2000, 2000, 4000);

        HINTERNET connect = WinHttpConnect(session, kHost, port, 0);
        if (!connect) { WinHttpCloseHandle(session); continue; }

        HINTERNET request = WinHttpOpenRequest(connect, L"GET", kPath, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); continue; }

        BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr);

        bool got = false;
        if (ok) {
            // Reject non-200
            DWORD status = 0, size = sizeof(status);
            if (WinHttpQueryHeaders(request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 200) {
                out.clear();
                for (;;) {
                    char buf[512];
                    DWORD read = 0;
                    if (!WinHttpReadData(request, buf, sizeof(buf), &read) || read == 0) break;
                    out.append(buf, read);
                    if (out.size() > 256) break; // a version line, not a file
                }
                got = !out.empty();
            }
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (got) return true;
    }
    return false;
}

void UpdateChecker::Start() {
    if (mStopping) return;
    if (mWorkerRun.exchange(true)) return; // already started
    if (GetState() != State::Idle) return;

    mState.store(State::Checking, std::memory_order_release);
    mWorker = std::thread(&UpdateChecker::RunCheck, this);
}

void UpdateChecker::Shutdown() {
    mStopping = true;
    mWorkerRun.store(false, std::memory_order_release);
    if (mWorker.joinable()) mWorker.join();
}

void UpdateChecker::RunCheck() {
    std::string body;
    if (!FetchCurrentTxt(body)) {
        mState.store(State::Failed, std::memory_order_release);
        LOG_INFO("Update check failed (could not fetch current.txt)", "Update");
        return;
    }

    const std::string remote = NormalizeVersion(body);
    const std::string local = NormalizeVersion(Sunstead::ClientVersion);
    mLatest = remote.empty() ? body : remote;

    if (remote.empty()) {
        mState.store(State::Failed, std::memory_order_release);
        LOG_INFO("Update check failed (empty current.txt)", "Update");
        return;
    }

    if (remote == local) {
        mState.store(State::UpToDate, std::memory_order_release);
        LOG_INFO("Update check: up to date (" + local + ")", "Update");
        return;
    }

    mState.store(State::UpdateAvailable, std::memory_order_release);
    LOG_INFO("Update available: local=" + local + " latest=" + remote, "Update");
}

void UpdateChecker::DeliverToast() {
    if (mToastDone || !IsUpdateAvailable()) return;
    mToastDone = true;
    Notifications::AddNotification(
        "Sunstead " + mLatest + " is out! You have " +
        std::string(Sunstead::ClientVersion) +
        " — update on GitHub Releases or Discord");
}

void UpdateChecker::PumpJoinChat() {
    bool inWorld = false;
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (ci && sdk_safe_ptr(ci, 0x200)) {
        SDK::Actor* lp = ci->getLocalPlayer();
        if (lp && sdk_safe_ptr(lp, 0x230)) inWorld = true;
    }

    const unsigned long long now = GetTickCount64();
    if (inWorld && !mWasInWorld) {
        mJoinMs = now;
        mChatSent = false;
        mChatTryMs = 0;
    } else if (!inWorld) {
        mChatSent = false;
    }
    mWasInWorld = inWorld;

    if (!inWorld || mChatSent || !IsUpdateAvailable()) return;

    if (now - mJoinMs < 3000) return;
    if (mChatTryMs && now - mChatTryMs < 1000) return;
    if (now - mJoinMs > 60000) return;
    mChatTryMs = now;

    const std::string msg =
        "§l§0[§bSunstead§0]§r §eUpdate available!§r "
        "latest §6v" + mLatest + "§r, you have §7" +
        std::string(Sunstead::ClientVersion) + "§r"
        "\n§7Download it from §fGitHub Releases§7 or the §fDiscord server§7.§r";

    if (SDK::Chat::displayClientMessage(msg)) {
        mChatSent = true;
        LOG_INFO("Update notice printed to chat (latest=" + mLatest + ")", "Update");
    }
}

void UpdateChecker::OnFrame() {
    DeliverToast();
    PumpJoinChat();
}
