#include "Player.h"
#include "../../Utils/SoundCloud.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/SkinUtils.hpp"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Render/RenderManager.h"
#include "../../Render/HudEditor.h"
#include "../Setting.h"
#include "../ModuleManager.h"
#include "../../Commands/CommandManager.h"
#include "../../Core/Logger.h"
#include <Windows.h>
#include <mmsystem.h>
#include <atomic>
#include <thread>
#include <cmath>
#include <cfloat>
#include <ctime>
#include <algorithm>

#pragma comment(lib, "winmm.lib")

// JPEG-only private stb build (the game TUs build PNG/TGA-only stb_image;
// STB_IMAGE_STATIC keeps this TU's symbols internal so nothing clashes).
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace {

std::mutex g_mu;
Player::State g_st;
void* g_artSrv = nullptr;    // ID3D11ShaderResourceView*
int g_artW = 0, g_artH = 0;
void* g_artDevice = nullptr;
// Request generation: every Play/PlayRandom/Stop bumps it. Workers carry
// the generation they started with and abort as soon as a newer request
// exists — so rapid "next" presses always resolve to the LATEST track and a
// stale worker can never kill it or overwrite its titles. (The old bool busy
// flag silently dropped presses made during a download and could stick at
// true forever if a worker died, bricking playback until restart.)
std::atomic<int> g_gen{ 0 };
std::atomic<long> g_posMs{ 0 };
std::atomic<long> g_lenMs{ 0 };
std::atomic<int> g_volume{ 70 };

const wchar_t* kAlias = L"scplay";

// ---- Render-thread-owned playback ------------------------------------------
// Every MCI call happens on the render thread inside Player::pump(). The old
// code opened the device on a worker thread while the render thread polled
// `status ... position` in parallel — those answers never agreed, g_posMs
// stayed 0 and the progress bar never filled regardless of playback. Workers
// therefore only do network work and hand audio over through g_pending;
// cross-thread intents (stop / pause / volume / restart) arrive as one-shot
// flags that pump() executes.
struct Pending {
    bool valid = false;
    int gen = 0;
    std::string path;
    SoundCloud::Track track;
    std::string pickLabel;   // "wave: ..." note shown once playback starts
};
std::mutex g_pendMu;
Pending g_pending;

// Artwork JPEG lands in a slot too: ID3D11 resources were being created and
// released from the download thread while the render thread was drawing the
// very same pointer.
std::mutex g_artMu;
std::vector<unsigned char> g_artJpg;
bool g_artPending = false;

std::atomic<bool> g_closeReq{ false };    // stop + close the device
std::atomic<bool> g_restartReq{ false };  // seek 0 + play (Prev with no history)
std::atomic<int>  g_pauseReq{ 0 };        // non-zero = toggle pause
std::atomic<int>  g_volReq{ -1 };         // -1 = nothing to apply
std::atomic<bool> g_opened{ false };      // alias is open (written on render thread)

// Render-thread-owned, never touched from a worker:
bool g_wasPlaying = false;                // for end-of-track detection
long g_anchorMs = 0;                      // position at the last good poll
unsigned long long g_anchorTick = 0;      // ...and its wall clock
unsigned long long g_openTick = 0;        // when the device started playing
std::string g_lastAudio;                  // previous temp file, retired on open

// Play history for the Prev button (guarded by g_mu).
std::vector<SoundCloud::Track> g_hist;
SoundCloud::Track g_cur;
bool g_curValid = false;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Preset queries used when the charts feed AND every personal source are
// unreachable. Genre-diverse on purpose (the old list was literally "wave"
// repeated — that was the "тупо ищет wave" complaint).
const char* kFallbackQueries[] = {
    "phonk", "cyberpunk", "future bass", "lofi beats", "deep house", "synthwave",
    "russian rap", "drum and bass", "progressive house", "trap",
};

bool autoplayEnabled() {
    auto* mod = ModuleManager::GetInstance().GetModuleByName("Player");
    if (mod && mod->mSettings.size() > 10)
        return static_cast<BoolSetting*>(mod->mSettings[10])->mValue;
    return true;
}

// "Retry" setting ([12], appended): download auto-retry on failure.
bool retriesEnabled() {
    auto* mod = ModuleManager::GetInstance().GetModuleByName("Player");
    if (mod && mod->mSettings.size() > 12)
        return static_cast<BoolSetting*>(mod->mSettings[12])->mValue;
    return true;
}

// Recent picks (cap 24, track ids). chartsRandom re-shuffles the SAME top-50
// feed every call, so the same handful of chart hits kept coming back
// ("повторяются треки"). Anything heard in this session is excluded from the
// random picks; manual plays remember here too.
std::mutex g_recentMu;
std::vector<long long> g_recentIds;
void rememberRecentId(long long id) {
    if (!id) return;
    std::lock_guard<std::mutex> lock(g_recentMu);
    for (long long v : g_recentIds) if (v == id) return;
    g_recentIds.push_back(id);
    if (g_recentIds.size() > 24) g_recentIds.erase(g_recentIds.begin());
}
bool isRecentId(long long id) {
    if (!id) return false;
    std::lock_guard<std::mutex> lock(g_recentMu);
    for (long long v : g_recentIds) if (v == id) return true;
    return false;
}

// The user's own search queries (every `.player play <query>` lands here) —
// a wave fallback seed. With OAuth the /me likes feed takes over; without it
// the related-track chain (seeded by the last played track) is the wave.
std::mutex g_qMu;
std::vector<std::string> g_userQueries;
void rememberUserQuery(const std::string& q) {
    if (q.empty()) return;
    std::lock_guard<std::mutex> lock(g_qMu);
    for (const auto& v : g_userQueries) if (v == q) return;
    g_userQueries.push_back(q);
    if (g_userQueries.size() > 16) g_userQueries.erase(g_userQueries.begin());
}

// [13] "Personal Wave".
bool personalWaveOn() {
    auto* mod = ModuleManager::GetInstance().GetModuleByName("Player");
    if (mod && mod->mSettings.size() > 13)
        return static_cast<BoolSetting*>(mod->mSettings[13])->mValue;
    return true;
}

// String settings ([0] api key, [14] profile). Same cross-thread access
// pattern as SetKey (command thread <-> render thread), all plain assignments.
std::string getStrSetting(size_t idx) {
    auto* mod = ModuleManager::GetInstance().GetModuleByName("Player");
    if (mod && mod->mSettings.size() > idx)
        return static_cast<StringSetting*>(mod->mSettings[idx])->mValue;
    return {};
}
void setStrSetting(size_t idx, const std::string& v) {
    auto* mod = ModuleManager::GetInstance().GetModuleByName("Player");
    if (mod && mod->mSettings.size() > idx)
        static_cast<StringSetting*>(mod->mSettings[idx])->mValue = v;
}

// Chat lines queued by worker threads; pump() (render thread) prints them —
// GuiData::displayClientMessage is not safe to call from a network worker.
std::mutex g_chatMu;
std::vector<std::string> g_chatQueue;
void queueChat(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_chatMu);
    g_chatQueue.push_back(msg);
}

// Your profile's numeric user id for the likes feed (0 = unresolved/None).
// The profile string persists as setting [14]; the id resolves once per
// session on a worker thread (workerResolveProfile).
std::atomic<long long> g_profileId{ 0 };
std::string            g_profileName;             // display name after resolve
std::string            g_profileResolvedFor;      // the profile string g_profileId belongs to
std::atomic<bool>      g_profileResolving{ false };
// "Set your profile" hint, once per enable.
bool                   g_waveHintShown = false;

} // namespace

// --- module state access (Player::State is private; helpers live here) -----
// NOTE: Player::State/private statics are touched only through the static
// member functions below.

Player::Player() : Module("Player", "SoundCloud now-playing widget", Category::Visual, 0) {
    AddSetting(new StringSetting("API Key", "SoundCloud api-v2 client_id (.player key <id>)", "")); // [0]
    AddSetting(new NumberSetting("Volume", "Playback volume 0..100", 70.0f, 0.0f, 100.0f, 1.0f));   // [1]
    // --- Styling (appended so existing config indices stay valid) ---
    AddSetting(new NumberSetting("Rounding", "Roundness of the panel corners", 12.0f, 0.0f, 20.0f, 1.0f)); // [2]
    AddSetting(new NumberSetting("Background Dim", "Opacity of the plate", 0.45f, 0.0f, 1.0f, 0.05f));     // [3]
    AddSetting(new NumberSetting("Background Blur", "Blur intensity behind the plate", 10.0f, 0.0f, 20.0f, 0.5f)); // [4]
    AddSetting(new BoolSetting("Shadow", "Draw a drop shadow behind the plate", true));                   // [5]
    AddSetting(new NumberSetting("Shadow Strength", "Opacity of the drop shadow", 0.45f, 0.0f, 1.0f, 0.05f)); // [6]
    AddSetting(new NumberSetting("Shadow Size", "Spread of the drop shadow in pixels", 14.0f, 4.0f, 30.0f, 1.0f)); // [7]
    AddSetting(new BoolSetting("Border", "Draw the plate border outline", true));                          // [8]
    AddSetting(new NumberSetting("Bar Height", "Thickness of the progress bar", 8.0f, 6.0f, 16.0f, 1.0f)); // [9]
    AddSetting(new BoolSetting("Autoplay", "Auto-play a random wave track + pick the next one", true));   // [10]
    AddSetting(new NumberSetting("Panel Width", "Minimum plate width", 340.0f, 200.0f, 520.0f, 5.0f));    // [11]
    // APPENDED (indices above never move): download auto-retry on failure.
    AddSetting(new BoolSetting("Retry", "Auto-retry the audio download when it fails (up to 3 times)", true)); // [12]
    // The personal wave: recommendations from YOUR likes (public profile) +
    // the related-track chain, with global charts only as the fallback.
    AddSetting(new BoolSetting("Personal Wave", "Feed the wave from your likes (profile) and related tracks instead of random charts", true)); // [13]
    // Your SoundCloud profile (link or username) — the wave reads your PUBLIC
    // likes through it. No OAuth/login needed: SoundCloud closed app
    // registration, and public likes are open to the plain api-v2 key.
    AddSetting(new StringSetting("SoundCloud Profile", "Your soundcloud.com profile link or username (.player user <link>)", "")); // [14]
    mIsHudModule = true;
    mPosX = 24.0f;
    mPosY = 120.0f;
}

Player::State Player::snapshot() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_st;
}

void Player::setStatus(const char* title, const char* artist, const char* note,
                       bool playing, bool paused, bool hasTrack, long long durationMs,
                       const std::string& artworkUrl, bool isPreview) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (title) g_st.title = title;
    if (artist) g_st.artist = artist;
    if (note) g_st.note = note;
    g_st.playing = playing;
    g_st.paused = paused;
    g_st.hasTrack = hasTrack;
    g_st.durationMs = durationMs;
    g_st.artworkUrl = artworkUrl;
    g_st.isPreview = isPreview;
}

bool Player::mci(const std::wstring& cmd, std::wstring* out) {
    wchar_t ret[128] = {};
    MCIERROR err = mciSendStringW(cmd.c_str(), out ? ret : nullptr,
                                  out ? (UINT)128 : 0, nullptr);
    if (out && err == 0) *out = ret;
    return err == 0;
}

std::string Player::mciErrorText(unsigned long err) {
    wchar_t buf[256] = {};
    if (mciGetErrorStringW((MCIERROR)err, buf, (UINT)256) && buf[0])
        return narrow(buf);
    char b[64];
    std::snprintf(b, sizeof(b), "mci error %lu", err);
    return b;
}

std::string Player::tempAudioPath(int gen) {
    char tmp[MAX_PATH] = {};
    DWORD n = GetTempPathA((DWORD)sizeof(tmp), tmp);
    std::string dir = (n > 0 && n < (DWORD)sizeof(tmp)) ? tmp : ".\\";
    // One file per request. Every track used to share a single path, so two
    // overlapping downloads wrote the SAME file and MCI opened a half-written
    // MP3 — which is exactly why pressing next often appeared to do nothing.
    char name[64];
    std::snprintf(name, sizeof(name), "sunstead_sc_%d.mp3", gen);
    return dir + name;
}

long Player::mciStatus(const wchar_t* item) {
    std::wstring cmd = L"status ";
    cmd += kAlias;
    cmd += L" ";
    cmd += item;
    std::wstring out;
    if (!mci(cmd, &out) || out.empty()) return -1;
    try { return std::stol(out); } catch (...) { return -1; }
}

std::string Player::mciMode() {
    std::wstring out;
    if (!mci(std::wstring(L"status ") + kAlias + L" mode", &out) || out.empty())
        return {};
    std::string s(out.begin(), out.end());
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool Player::isCurrent(int gen) {
    return gen == g_gen.load();
}

// Render thread only. Consume a finished download and start it.
void Player::openPending() {
    Pending p;
    {
        std::lock_guard<std::mutex> lock(g_pendMu);
        if (!g_pending.valid) return;
        if (g_pending.gen != g_gen.load()) { g_pending.valid = false; return; }
        p = g_pending;
        g_pending.valid = false;
    }
    closeStream();
    const std::wstring quoted = L"\"" + widen(p.path) + L"\"";
    MCIERROR err = mciSendStringW((L"open " + quoted + L" type mpegvideo alias " + kAlias).c_str(),
                                  nullptr, 0, nullptr);
    if (err != 0) {
        // Fallback: typeless open lets MCI pick the driver by extension.
        err = mciSendStringW((L"open " + quoted + L" alias " + kAlias).c_str(),
                             nullptr, 0, nullptr);
    }
    if (err != 0) {
        const std::string what = mciErrorText((unsigned long)err);
        std::lock_guard<std::mutex> lock(g_mu);
        g_st.loading = false;
        g_st.playing = false;
        g_st.paused = false;
        g_st.note = "MCI open: " + what;
        return;
    }
    wchar_t vol[64];
    swprintf_s(vol, L"setaudio %s volume to %d", kAlias, g_volume.load() * 10);
    mci(vol);
    MCIERROR perr = mciSendStringW((std::wstring(L"play ") + kAlias).c_str(), nullptr, 0, nullptr);
    if (perr != 0) {
        const std::string what = mciErrorText((unsigned long)perr);
        closeStream();
        std::lock_guard<std::mutex> lock(g_mu);
        g_st.loading = false;
        g_st.playing = false;
        g_st.paused = false;
        g_st.note = "MCI play: " + what;
        return;
    }
    const long len = mciStatus(L"length");
    if (len > 0) g_lenMs.store(len);
    g_posMs.store(0);
    g_anchorMs = 0;
    g_anchorTick = GetTickCount64();
    g_openTick = g_anchorTick;
    g_opened = true;
    g_wasPlaying = true;
    // Remember a track only once its audio is actually running — a failed
    // download must never poison the Prev history — and exclude it from the
    // random picks (the charts feed would otherwise replay it).
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_cur = p.track;
        g_curValid = true;
        g_st.loading = false;
        g_st.playing = true;
        g_st.paused = false;
        if (g_st.note.rfind("MCI", 0) != 0 && g_st.note.rfind("audio download", 0) != 0)
            g_st.note = p.track.isPreview ? "30s preview" : p.pickLabel;
    }
    rememberRecentId(p.track.id);
    // Retire the previous temp file only now that this one is in use.
    if (!g_lastAudio.empty() && g_lastAudio != p.path)
        DeleteFileA(g_lastAudio.c_str());
    g_lastAudio = p.path;
}

// Render thread only — the single place MCI is driven from.
void Player::pump(bool enabled) {
    // Chat lines queued by the OAuth worker (displayClientMessage must not
    // be called from a network thread).
    {
        std::vector<std::string> chat;
        {
            std::lock_guard<std::mutex> lock(g_chatMu);
            chat.swap(g_chatQueue);
        }
        for (const auto& m : chat)
            CommandManager::GetInstance().PrintToChat(m);
    }
    if (g_closeReq.exchange(false)) {
        closeStream();
        g_opened = false;
        g_wasPlaying = false;
        g_posMs.store(0);
        g_lenMs.store(0);
        g_anchorMs = 0;
        dropArtwork();
    }
    openPending();   // locks internally and returns at once when nothing is due

    {
        std::vector<unsigned char> jpg;
        {
            std::lock_guard<std::mutex> lock(g_artMu);
            if (g_artPending) { jpg = std::move(g_artJpg); g_artPending = false; }
        }
        if (!jpg.empty()) ensureArtwork(jpg);
    }

    const int vol = g_volReq.exchange(-1);
    if (vol >= 0 && g_opened) {
        wchar_t cmd[64];
        swprintf_s(cmd, L"setaudio %s volume to %d", kAlias, vol * 10);
        mci(cmd);
    }
    if (g_restartReq.exchange(false) && g_opened) {
        mci(std::wstring(L"seek ") + kAlias + L" to 0");
        mci(std::wstring(L"play ") + kAlias);
        g_posMs.store(0);
        g_anchorMs = 0;
        g_anchorTick = GetTickCount64();
        g_wasPlaying = true;
        std::lock_guard<std::mutex> lock(g_mu);
        g_st.playing = true;
        g_st.paused = false;
    }
    if (g_pauseReq.exchange(false) && g_opened) {
        const std::string mode = mciMode();
        if (mode == "playing") {
            mci(std::wstring(L"pause ") + kAlias);
            std::lock_guard<std::mutex> lock(g_mu);
            g_st.paused = true;
            g_st.playing = false;
        } else if (mode == "paused") {
            mci(std::wstring(L"play ") + kAlias);
            std::lock_guard<std::mutex> lock(g_mu);
            g_st.paused = false;
            g_st.playing = true;
        }
    }
    if (!g_opened) return;

    const long pos = mciStatus(L"position");
    const long len = mciStatus(L"length");
    if (len > 0) g_lenMs.store(len);
    if (pos >= 0) {
        g_posMs.store(pos);
        g_anchorMs = pos;
        g_anchorTick = GetTickCount64();
    } else if (g_wasPlaying) {
        // MCI status went quiet — keep the bar moving off the wall clock
        // instead of freezing at the last value.
        long est = g_anchorMs + (long)(GetTickCount64() - g_anchorTick);
        const long cap = g_lenMs.load();
        if (cap > 0 && est > cap) est = cap;
        g_posMs.store(est);
    }

    const std::string mode = mciMode();
    if (mode == "playing") {
        g_wasPlaying = true;
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_st.paused) g_st.playing = true;
    } else if (mode == "paused") {
        std::lock_guard<std::mutex> lock(g_mu);
        g_st.paused = true;
        g_st.playing = false;
    } else if (mode == "stopped" && g_wasPlaying) {
        // End of track. Only roll the next one after it has actually played
        // for a moment — a bad file that dies instantly must not spin the
        // autoplay loop at full speed.
        const bool playedForReal = (GetTickCount64() - g_openTick) > 1500;
        g_wasPlaying = false;
        g_opened = false;
        closeStream();
        g_posMs.store(0);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_st.playing = false;
            g_st.paused = false;
        }
        if (enabled && playedForReal && autoplayEnabled())
            PlayRandom();
    }
}

void Player::rememberCurrent() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_curValid) return;
    g_hist.push_back(g_cur);
    if (g_hist.size() > 32) g_hist.erase(g_hist.begin());
    g_curValid = false;
}

void Player::closeStream() {
    mci(std::wstring(L"stop ") + kAlias);
    mci(std::wstring(L"close ") + kAlias);
}

void Player::ensureArtwork(const std::vector<unsigned char>& jpg) {
    dropArtwork();
    if (jpg.empty()) return;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(jpg.data(), (int)jpg.size(), &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0) return;
    ID3D11ShaderResourceView* srv =
        (ID3D11ShaderResourceView*)SkinUtils::createTextureRGBA(px, w, h);
    stbi_image_free(px);
    if (!srv) return;
    g_artSrv = srv;
    g_artW = w; g_artH = h;
    g_artDevice = RenderManager::GetInstance().GetDevice();
}

void Player::dropArtwork() {
    if (g_artSrv) {
        ((ID3D11ShaderResourceView*)g_artSrv)->Release();
        g_artSrv = nullptr;
    }
    g_artW = g_artH = 0;
    g_artDevice = nullptr;
}

void Player::workerArtwork(std::string url) {
    if (url.empty()) return;
    std::vector<unsigned char> bytes = SoundCloud::downloadBytes(url);
    if (bytes.empty()) return;
    if (!isCurrent(g_gen.load())) return;
    std::lock_guard<std::mutex> lock(g_artMu);
    g_artJpg = std::move(bytes);
    g_artPending = true;
}

// Worker tail shared by search/wave/history: publish the meta, stream the
// MP3 to its per-request temp file, then hand it to pump(). Re-checks the
// generation at every step so a superseded worker touches nothing.
// `pickLabel` is the wave note ("wave: similar to X") shown by openPending.
void Player::workerTrack(SoundCloud::Track t, int gen, const char* pickLabel) {
    if (!isCurrent(gen)) return;
    setStatus(t.title.c_str(), t.artist.c_str(),
              "downloading audio...", false, false, true,
              t.durationMs, t.artworkUrl, t.isPreview);
    const std::string path = tempAudioPath(gen);
    // Auto-retry (Retry setting): a dropped Wi-Fi socket used to end the
    // download in one shot and playback just stopped. Up to 3 retries with a
    // growing pause; every attempt re-checks the generation so a superseded
    // worker aborts instead of fighting a newer request.
    bool downloaded = false;
    const int maxAttempts = retriesEnabled() ? 4 : 1;   // 1 try + 3 retries
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (!isCurrent(gen)) { DeleteFileA(path.c_str()); return; }
        if (attempt > 0) {
            const std::string rn = "download retry " + std::to_string(attempt) + "/3...";
            setStatus(t.title.c_str(), t.artist.c_str(), rn.c_str(), false, false, true,
                      t.durationMs, t.artworkUrl, t.isPreview);
            Sleep(600 * attempt);
            if (!isCurrent(gen)) { DeleteFileA(path.c_str()); return; }
        }
        if (SoundCloud::downloadToFile(t.streamUrl, path)) { downloaded = true; break; }
    }
    if (!downloaded) {
        if (!isCurrent(gen)) { DeleteFileA(path.c_str()); return; }
        std::lock_guard<std::mutex> lock(g_mu);
        g_st.loading = false;
        g_st.playing = false;
        g_st.paused = false;
        g_st.note = "audio download failed (network blocked?)";
        return;
    }
    if (!isCurrent(gen)) { DeleteFileA(path.c_str()); return; }
    {
        // Publishing is all pump() needs — it opens the device itself.
        std::lock_guard<std::mutex> lock(g_pendMu);
        g_pending.valid = true;
        g_pending.gen = gen;
        g_pending.path = path;
        g_pending.track = t;
        g_pending.pickLabel = pickLabel ? pickLabel : "";
    }
    if (!t.artworkUrl.empty() && isCurrent(gen))
        workerArtwork(t.artworkUrl);
}

void Player::failTrack(const std::string& err, int gen) {
    if (!isCurrent(gen)) return;
    setStatus("Nothing playing", "", err.c_str(),
              false, false, false, 0, {}, false);
    std::lock_guard<std::mutex> lock(g_mu);
    g_st.loading = false;
}

void Player::workerPlay(std::string key, std::string query, int gen) {
    if (!isCurrent(gen)) return;
    setStatus("Searching...", query.c_str(), "", false, false, false, 0, {}, false);
    SoundCloud::Result r = SoundCloud::searchFirst(key, query);
    if (!isCurrent(gen)) return;
    if (!r.ok) { failTrack(r.error, gen); return; }
    workerTrack(std::move(r.track), gen);
}

// THE WAVE: real recommendations instead of raw search. Source order (first
// playable non-recent pick wins, each source gets up to 3 draws):
//   1) /users/{id}/likes/tracks — tracks YOU liked, off your public profile
//      (`.player user <link>`; the id resolves once per session, no OAuth);
//   2) tracks/{id}/related — SoundCloud's own recommendation graph, seeded
//      by the last track you actually played, so the station follows your
//      taste as the session goes;
//   3) a random re-roll over your own past search queries;
//   4) global charts / preset wave queries (the last resort).
void Player::workerRandom(std::string key, int gen) {
    if (!isCurrent(gen)) return;
    setStatus("Searching...", "the wave...", "", false, false, false, 0, {}, false);

    SoundCloud::Result r;
    std::string label;
    const bool personal = personalWaveOn();

    // 1) Your liked tracks (public profile). A configured-but-unresolved
    //    profile kicks off the resolve and is retried on the next pick.
    if (personal) {
        const long long uid = g_profileId.load();
        if (uid > 0) {
            for (int d = 0; d < 3 && !r.ok; ++d) {
                SoundCloud::Result cand = SoundCloud::userLikesRandom(key, uid);
                if (!isCurrent(gen)) return;
                if (cand.ok && !isRecentId(cand.track.id)) {
                    r = std::move(cand);
                    label = "wave: your likes";
                }
            }
        } else if (!GetProfile().empty() && !g_profileResolving.load()) {
            g_profileResolving.store(true);
            const std::string prof = GetProfile();
            std::thread([prof]() { workerResolveProfile(prof); }).detach();
        }
        // Tell the user the wave CAN be personal — once per enable.
        if (GetProfile().empty() && !g_waveHintShown) {
            g_waveHintShown = true;
            queueChat("§ePlayer: want YOUR tracks? §b.player user <soundcloud.com/you> §eand the wave plays your likes (no login needed).");
        }
    }

    // 2) The related-track chain, seeded by the last played track.
    if (personal && !r.ok) {
        long long seedId = 0;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_curValid) seedId = g_cur.id;
        }
        if (seedId == 0) {
            std::lock_guard<std::mutex> lock(g_recentMu);
            if (!g_recentIds.empty()) seedId = g_recentIds.back();
        }
        if (seedId != 0) {
            for (int d = 0; d < 3 && !r.ok; ++d) {
                SoundCloud::Result cand = SoundCloud::relatedRandom(key, seedId);
                if (!isCurrent(gen)) return;
                if (cand.ok && !isRecentId(cand.track.id)) {
                    r = std::move(cand);
                    label = "wave: similar to your last track";
                }
            }
        }
    }

    // 3) Your own past search queries.
    if (!r.ok) {
        std::string q;
        {
            std::lock_guard<std::mutex> lock(g_qMu);
            if (!g_userQueries.empty())
                q = g_userQueries[(size_t)(rand() % (int)g_userQueries.size())];
        }
        if (!q.empty()) {
            for (int d = 0; d < 3 && !r.ok; ++d) {
                SoundCloud::Result cand = SoundCloud::searchRandom(key, q);
                if (!isCurrent(gen)) return;
                if (cand.ok && !isRecentId(cand.track.id)) {
                    r = std::move(cand);
                    label = "wave: from your searches";
                }
            }
        }
    }

    // 4) Charts + preset wave queries.
    for (int attempt = 0; attempt < 8 && !r.ok; ++attempt) {
        r = (attempt == 0) ? SoundCloud::chartsRandom(key)
                           : SoundCloud::searchRandom(key,
                  kFallbackQueries[(attempt - 1) %
                                   (sizeof(kFallbackQueries) / sizeof(kFallbackQueries[0]))]);
        if (!isCurrent(gen)) return;
        if (r.ok) {
            label = (attempt == 0) ? "wave: trending" : "wave: fresh picks";
            if (isRecentId(r.track.id)) r.ok = false;      // reroll, repeats were the norm
        }
    }

    if (!r.ok) { failTrack(r.error.empty() ? "no wave pick (network?)" : r.error, gen); return; }
    rememberRecentId(r.track.id);
    workerTrack(std::move(r.track), gen, label.c_str());
}

void Player::SetKey(const std::string& id) {
    auto* mod = static_cast<Player*>(ModuleManager::GetInstance().GetModuleByName("Player"));
    if (mod && !mod->mSettings.empty())
        static_cast<StringSetting*>(mod->mSettings[0])->mValue = id;
}

std::string Player::GetKey() {
    auto* mod = static_cast<Player*>(ModuleManager::GetInstance().GetModuleByName("Player"));
    if (mod && !mod->mSettings.empty())
        return static_cast<StringSetting*>(mod->mSettings[0])->mValue;
    return {};
}

// Newest request wins: bump the generation (any in-flight worker becomes
// stale), ask pump() to stop the current audio this frame, then resolve on a
// worker thread. Nothing here touches MCI, so it is safe from a command or a
// render thread alike.
static void beginRequest() {
    g_gen.fetch_add(1);
    g_closeReq.store(true);
    g_posMs.store(0);
    g_lenMs.store(0);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_st.loading = true;
        g_st.playing = false;
        g_st.paused = false;
        g_st.note.clear();
    }
}

void Player::Play(const std::string& query) {
    const std::string key = GetKey();
    if (key.empty()) {
        setStatus("Nothing playing", "", "set .player key <client_id> first",
                  false, false, false, 0, {}, false);
        CommandManager::GetInstance().PrintToChat(
            "§cPlayer: no API key — use §b.player key <client_id>");
        return;
    }
    // The user's own query is remembered: PlayRandom re-rolls THESE instead
    // of the global charts (their taste, not the wave-heavy top-50).
    rememberUserQuery(query);
    rememberCurrent();
    beginRequest();
    const int gen = g_gen.load();
    std::thread([key, query, gen]() { workerPlay(key, query, gen); }).detach();
}

void Player::PlayRandom() {
    const std::string key = GetKey();
    if (key.empty()) {
        setStatus("Nothing playing", "", "set .player key <client_id> first",
                  false, false, false, 0, {}, false);
        CommandManager::GetInstance().PrintToChat(
            "§cPlayer: no API key — use §b.player key <client_id>");
        return;
    }
    rememberCurrent();
    beginRequest();
    const int gen = g_gen.load();
    std::thread([key, gen]() { workerRandom(key, gen); }).detach();
}

// Back through the play history. With nothing left to go back to, restart
// whatever is loaded from the top instead of silently doing nothing.
void Player::Prev() {
    SoundCloud::Track t;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_hist.empty()) {
            t = g_hist.back();
            g_hist.pop_back();
            have = true;
        }
    }
    if (!have) {
        if (g_opened) g_restartReq.store(true);
        return;
    }
    beginRequest();
    const int gen = g_gen.load();
    std::thread([t, gen]() { workerTrack(t, gen); }).detach();
}

void Player::Stop() {
    g_gen.fetch_add(1);        // invalidate any in-flight worker
    g_closeReq.store(true);    // pump() closes the device + drops artwork
    g_posMs.store(0);
    g_lenMs.store(0);
    setStatus("Nothing playing", "", "", false, false, false, 0, {}, false);
    std::lock_guard<std::mutex> lock(g_mu);
    g_st.loading = false;
}

void Player::PauseToggle() {
    g_pauseReq.store(1);       // executed by pump() on the render thread
}

void Player::SetVolume(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_volume.store(v);
    auto* mod = static_cast<Player*>(ModuleManager::GetInstance().GetModuleByName("Player"));
    if (mod && mod->mSettings.size() > 1)
        static_cast<NumberSetting*>(mod->mSettings[1])->mValue = (float)v;
    g_volReq.store(v);         // applied by pump() on the render thread
}

std::string Player::StatusLine() {
    State s = snapshot();
    if (s.loading) return "Player: searching...";
    if (!s.hasTrack) {
        std::string base = "Player: idle";
        if (!g_profileName.empty()) base += " (wave: " + g_profileName + "'s likes)";
        return base + (s.note.empty() ? "" : " (" + s.note + ")");
    }
    char b[256];
    std::snprintf(b, sizeof(b), "Player: %s - %s [%s]%s",
                  s.artist.c_str(), s.title.c_str(),
                  s.paused ? "paused" : (s.playing ? "playing" : "stopped"),
                  s.isPreview ? " (preview)" : "");
    return b;
}

// ---------------------------------------------------------------------------
// The personal profile (likes feed for the wave). One link, zero login:
// api-v2's /resolve maps the profile URL to a numeric user id once per
// session, /users/{id}/likes/tracks serves the user's public likes.
// ---------------------------------------------------------------------------

bool Player::ProfileResolved() {
    return g_profileId.load() > 0;
}

std::string Player::GetProfile() {
    return getStrSetting(14);
}

void Player::SetProfile(const std::string& urlOrName) {
    setStrSetting(14, urlOrName);
    // Any previous id belonged to the old profile.
    g_profileId.store(0);
    g_profileResolvedFor.clear();
    if (urlOrName.empty()) {
        CommandManager::GetInstance().PrintToChat(
            "§7Player: profile cleared - the wave runs on related tracks & charts.");
        return;
    }
    bool expected = false;
    if (!g_profileResolving.compare_exchange_strong(expected, true)) return;
    std::thread([urlOrName]() { workerResolveProfile(urlOrName); }).detach();
}

// Resolve a stored/copied profile string to its numeric user id once per
// session. Chat output is queued for the render thread (displayClientMessage
// is not safe to call from a network worker).
void Player::workerResolveProfile(std::string urlOrName) {
    struct Guard { ~Guard() { g_profileResolving.store(false); } } guard;
    SoundCloud::UserInfo u = SoundCloud::resolveUser(GetKey(), urlOrName);
    if (!u.ok) {
        queueChat("§cPlayer: could not resolve profile - " + u.error);
        return;
    }
    g_profileId.store(u.id);
    g_profileName = u.username;
    g_profileResolvedFor = urlOrName;
    queueChat("§aPlayer: profile found - §b" + u.username +
              "§a. The wave now plays your likes (make them public if it ever says 'private').");
    // Start the personal wave right away when the widget is up and idle.
    auto* mod = ModuleManager::GetInstance().GetModuleByName("Player");
    if (mod && mod->IsEnabled() && autoplayEnabled()) {
        State s = snapshot();
        if (!s.hasTrack && !s.loading) PlayRandom();
    }
}

void Player::OnEnable() {
    if (GetKey().empty()) {
        CommandManager::GetInstance().PrintToChat(
            "§ePlayer: set your SoundCloud client_id first — §b.player key <client_id>");
    }
    auto* mod = static_cast<Player*>(ModuleManager::GetInstance().GetModuleByName("Player"));
    if (mod && mod->mSettings.size() > 1)
        g_volume = (int)static_cast<NumberSetting*>(mod->mSettings[1])->mValue;
    // Re-resolve the persisted profile (ids are cached per session) and reset
    // the "set your profile" hint for this enable.
    g_waveHintShown = false;
    if (!GetProfile().empty() && g_profileId.load() == 0 &&
        g_profileResolvedFor != GetProfile() && !g_profileResolving.load()) {
        g_profileResolving.store(true);
        const std::string prof = GetProfile();
        std::thread([prof]() { workerResolveProfile(prof); }).detach();
    }
    // Wave autoplay: a recommendation pick starts by itself so the widget is
    // never just a dead picture frame.
    if (!GetKey().empty() && autoplayEnabled()) {
        State s = snapshot();
        if (!s.hasTrack && !s.loading)
            PlayRandom();
    } else if (GetKey().empty() || GetProfile().empty()) {
        queueChat("§7Player: §b.player user <soundcloud.com/you> §7= the wave plays YOUR likes. Key for playback: §b.player key <client_id>");
    }
}

void Player::OnDisable() {
    Stop();
}

static std::string fmtMs(long ms) {
    if (ms < 0) ms = 0;
    char b[16];
    std::snprintf(b, sizeof(b), "%ld:%02ld", ms / 60000, (ms / 1000) % 60);
    return b;
}

void Player::OnRender() {
    // MCI is driven from here — BEFORE any visibility gate, so the device
    // still opens, advances and finishes while no HUD screen is showing.
    // (This used to be a `status ... position` poll scattered around the
    // worker thread, which is why the bar never moved.)
    pump(IsEnabled());

    // Appear/disappear driver (TargetHUD discipline): the exponential approach
    // keeps producing frames while the value sits inside (0,1) — that renders
    // BOTH the fade-in on enable AND the fade-out after disable. The old code
    // early-returned on !IsEnabled() before drawing anything, so toggling and
    // dragging the plate had no animation at all.
    float dtA = ImGui::GetIO().DeltaTime;
    if (dtA <= 0.0f || dtA > 0.25f) dtA = 1.0f / 60.0f;
    mAppear += ((IsEnabled() ? 1.0f : 0.0f) - mAppear) * std::min(1.0f, dtA * 9.0f);
    if (mAppear < 0.004f) { mAppear = 0.0f; return; }
    if (!IsEnabled() && mAppear < 0.01f) return;

    OnFirstLayout(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);

    State s = snapshot();

    const long posMs = g_posMs.load();
    long lenMs = g_lenMs.load();
    if (lenMs <= 0) lenMs = (long)s.durationMs;

    // Device change drops GPU textures (same discipline as block icons).
    if (g_artSrv && g_artDevice != RenderManager::GetInstance().GetDevice())
        dropArtwork();

    // ---- Style settings (TargetHUD-style plate) ----
    auto num = [&](size_t i, float fallback) -> float {
        auto* mod = ModuleManager::GetInstance().GetModuleByName("Player");
        if (mod && mod->mSettings.size() > i)
            if (auto* n = dynamic_cast<NumberSetting*>(mod->mSettings[i]))
                return n->mValue;
        return fallback;
    };
    auto boolean = [&](size_t i, bool fallback) -> bool {
        auto* mod = ModuleManager::GetInstance().GetModuleByName("Player");
        if (mod && mod->mSettings.size() > i)
            if (auto* b = dynamic_cast<BoolSetting*>(mod->mSettings[i]))
                return b->mValue;
        return fallback;
    };
    const float rounding  = num(2, 12.0f);
    const float dim       = num(3, 0.45f);
    const float blurStr   = num(4, 10.0f);
    const bool  shadowOn  = boolean(5, true);
    const float shadowStr = num(6, 0.45f);
    const float shadowSz  = num(7, 14.0f);
    const bool  borderOn  = boolean(8, true);
    // Bar Height ([9]) with a hard 6px floor — a hairline-thin track (the old
    // 5px default) is what made the slider unreadable.
    const float barHBase  = std::max(num(9, 8.0f), 6.0f);
    const float minPlateW = std::max(num(11, 340.0f), 300.0f);

    // Volume slider in the ClickGUI drives the live device.
    {
        const int v = (int)num(1, 70.0f);
        if (v != g_volume.load()) SetVolume(v);
    }

    const float scale = mScale > 0.01f ? mScale : 1.0f;
    const float pad = 10.0f * scale;
    const float gap = 8.0f * scale;
    const float roundingPx = rounding * scale;

    // FOREGROUND list for the plate content (TargetHUD discipline: blur goes
    // on the BACKGROUND list first so it can never paint over the plate).
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (!dl || !bg) return;
    ImFont* font = ImGui::GetFont();
    const float fsTitle = 17.0f * scale;
    const float fsSmall = 13.0f * scale;

    // The note is the only place transport feedback surfaces (MCI open/play
    // failure, download failure, "downloading audio..."). Put it in the artist
    // slot while it is meaningful, so a next-press that does nothing is
    // diagnosable on screen instead of looking like a dead button.
    const bool noteIsError = s.note.rfind("MCI", 0) == 0 ||
                             s.note.rfind("audio download", 0) == 0 ||
                             s.note.rfind("set .player", 0) == 0;
    const bool noteIsProgress = s.note == "downloading audio...";
    std::string title = s.hasTrack ? s.title : "Nothing playing";
    std::string artist;
    if (noteIsError || noteIsProgress) artist = s.note;
    else if (!s.artist.empty())        artist = s.artist;
    else if (!s.note.empty())          artist = s.note;
    else                                artist = "SoundCloud wave player";
    if (title.size() > 32) title = title.substr(0, 31) + "...";
    if (artist.size() > 36) artist = artist.substr(0, 35) + "...";

    const float titleW = ImRenderUtils::getTextWidth(&title, fsTitle);
    const float artistW = ImRenderUtils::getTextWidth(&artist, fsSmall);
    const float textW = std::max(titleW, artistW);
    // Wide enough that the time readout and the three transport buttons on
    // the control row never collide (3x30 + 2x6 buttons + ~75px of times).
    float colW = std::max(textW, 250.0f * scale);

    // Row layout is driven by MEASURED line heights, not ad-hoc gaps, so no
    // element can overlap the next at any scale:
    //   row0 title (fsTitle)  -> 6px gap
    //   row1 artist (fsSmall) -> 8px gap
    //   row2 controls (28px)  -> 10px gap
    //   row3 progress bar (Bar Height, min 6px)
    const float ctrlH = 28.0f * scale;
    const float barH = barHBase * scale;
    const float contentH = fsTitle + 6.0f * scale + fsSmall + 8.0f * scale +
                           ctrlH + 10.0f * scale + barH;
    const float artSide = contentH; // artwork fills the full plate height
    float width = pad * 2.0f + artSide + gap + colW;
    if (width < minPlateW * scale) {
        colW += minPlateW * scale - width;
        width = minPlateW * scale;
    }
    const float height = pad * 2.0f + contentH;

    // Footprint = the un-zoomed plate (HUD editor drags against the resting
    // size, exactly like every other HUD module).
    mWidth = width;
    mHeight = height;

    // Keep the plate on screen: the bottom-center default + a wide plate used
    // to overflow the right/left screen edges ("вылезает за края").
    {
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        if (mPosX < 6.0f) mPosX = 6.0f;
        if (mPosY < 6.0f) mPosY = 6.0f;
        if (mPosX + width > disp.x - 6.0f) mPosX = std::max(6.0f, disp.x - width - 6.0f);
        if (mPosY + height > disp.y - 6.0f) mPosY = std::max(6.0f, disp.y - height - 6.0f);
    }

    // ---- Fade/scale driver (TargetHUD plate spring) ----
    const float a = mAppear;
    // Appear: 0.88 -> 1 with an ease-out-back overshoot; disappear: smoothstep
    // down to 0.94 while the alpha follows. Center-pivot zoom so nothing
    // crawls out of the plate while it animates.
    float zoom;
    if (IsEnabled()) {
        const float t = std::min(1.0f, a);
        const float c1 = 1.70158f, c3 = c1 + 1.0f;
        const float u = t - 1.0f;
        const float back = 1.0f + c3 * u * u * u + c1 * u * u;   // easeOutBack
        zoom = 0.88f + 0.12f * back;
    } else {
        const float t = std::min(1.0f, a);
        zoom = 0.94f + 0.06f * (t * t * (3.0f - 2.0f * t));      // smoothstep up
    }
    const float pivotX = mPosX + width * 0.5f;
    const float pivotY = mPosY + height * 0.5f;
    auto zx = [&](float x) { return pivotX + (x - pivotX) * zoom; };
    auto zy = [&](float y) { return pivotY + (y - pivotY) * zoom; };

    const ImVec2 p0(zx(mPosX), zy(mPosY));
    const ImVec2 p1(zx(mPosX + width), zy(mPosY + height));
    const ImVec4 rect(p0.x, p0.y, p1.x, p1.y);
    const uint64_t nowMs = ColorUtils::GetTimeMs();
    const ImColor accent = ColorUtils::getThemedColor(0.0f, nowMs);
    const ImColor accent2 = ColorUtils::getThemedColor(60.0f, nowMs);

    // ---- Backdrop blur FIRST, on the background list ----
    {
        bool guiOpen = false;
        if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
            guiOpen = gui->IsEnabled();
        const float blurLive = blurStr * std::min(1.0f, a * 3.0f);
        if (blurLive > 0.05f && !guiOpen)
            D2D::addBlurOptimized(bg, blurLive, std::optional<ImVec4>(rect), roundingPx);
    }
    // ---- Shadow + dim plate (background list, under everything) ----
    if (shadowOn)
        ImRenderUtils::fillShadowRectangle(rect, ImColor(0, 0, 0), shadowStr * a, roundingPx, shadowSz);
    ImRenderUtils::fillRectangle(rect, ImColor(12, 12, 15, (int)(255.0f * dim * a)), 1.0f, roundingPx);
    // ---- TargetHUD-style TOP SHEEN: a white gradient fading down from the
    //      top edge — the "3D lit from above" read. Rounded so it stays
    //      inside the plate corners.
    ImRenderUtils::fillGradientRounded(dl, rect,
        ImVec4(1, 1, 1, 0.10f * a), ImVec4(1, 1, 1, 0.0f),
        roundingPx, 0, /*vertical=*/true);
    // ---- Border (foreground, above the plate) ----
    if (borderOn) {
        ImColor borderCol = s.playing && !s.paused
            ? ImColor(accent.Value.x, accent.Value.y, accent.Value.z, 0.35f * a)
            : ImColor(1.0f, 1.0f, 1.0f, 0.10f * a);
        dl->AddRect(p0, p1, borderCol, roundingPx, 0, 1.0f);
    }

    // ---- Artwork: left edge, full plate height ----
    const ImVec2 ap0(p0.x + pad, p0.y + pad);
    const ImVec2 ap1(ap0.x + artSide, ap0.y + artSide);
    const float artRound = std::min(8.0f * scale, roundingPx * 0.7f);
    if (g_artSrv) {
        dl->AddImageRounded((ImTextureID)g_artSrv, ap0, ap1,
                            ImVec2(0, 0), ImVec2(1, 1),
                            ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, a)), artRound);
    } else {
        dl->AddRectFilled(ap0, ap1,
                          ImGui::ColorConvertFloat4ToU32(
                              ImVec4(accent.Value.x, accent.Value.y, accent.Value.z, 0.85f * a)),
                          artRound);
        dl->AddCircle(ImVec2(ap0.x + artSide * 0.5f, ap0.y + artSide * 0.5f),
                      artSide * 0.22f, IM_COL32(10, 10, 13, (int)(255 * a)), 24, 2.0f);
    }
    dl->AddRect(ap0, ap1, ImColor(255, 255, 255, (int)(45 * a)), artRound, 0, 1.0f);
    // Same lit-from-above sheen on the artwork so it reads as one 3D surface
    // with the plate.
    ImRenderUtils::fillGradientRounded(dl, ImVec4(ap0.x, ap0.y, ap1.x, ap1.y),
        ImVec4(1, 1, 1, 0.14f * a), ImVec4(1, 1, 1, 0.0f),
        artRound, 0, /*vertical=*/true);

    // ---- Text column (right of the artwork) ----
    const float tx = ap1.x + gap;
    float ty = ap0.y + 1.0f * scale;
    ImRenderUtils::drawText(ImVec2(tx, ty), title, ImColor(255, 255, 255, (int)(255 * a)),
                            fsTitle, 1.0f, false, dl);
    ty += fsTitle + 6.0f * scale;
    ImRenderUtils::drawText(ImVec2(tx, ty), artist, ImColor(170, 170, 180, (int)(255 * a)),
                            fsSmall, 1.0f, false, dl);
    ty += fsSmall + 8.0f * scale;

    // ---- Control row: elapsed/total on the left, prev / pause / next ----
    const float cy = ty + ctrlH * 0.5f;

    const float bw = 30.0f * scale;   // 30px hit target (was 28) - thumb-sized
    const float bh = ctrlH;
    const float bRad = std::min(bw, bh) * 0.5f;
    const float bgap = 6.0f * scale;
    const float rowRight = tx + colW;
    const ImVec4 rNext(rowRight - bw, cy - bh * 0.5f, rowRight, cy + bh * 0.5f);
    const ImVec4 rPause(rNext.x - bgap - bw, rNext.y, rNext.x - bgap, rNext.w);
    const ImVec4 rPrev(rPause.x - bgap - bw, rPause.y, rPause.x - bgap, rPause.w);

    const std::string times = fmtMs(posMs) + " / " + fmtMs(lenMs);
    ImRenderUtils::drawText(ImVec2(tx, cy - fsSmall * 0.5f), times,
                            ImColor(150, 150, 160, (int)(255 * a)), fsSmall, 1.0f, false, dl);

    const bool live = s.hasTrack || s.loading;
    const ImU32 glyph = live ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 110);

    // Rounded-rect capsules (TargetHUD pill language), NOT circles: the
    // circular pads read as floating dots and the icons drown inside them.
    auto pill = [&](const ImVec4& r, bool hover) {
        if (r.z - r.x < 2.0f) return;
        ImColor baseCol = hover
            ? ImColor(accent.Value.x, accent.Value.y, accent.Value.z, 190.0f * a)
            : ImColor(1.0f, 1.0f, 1.0f, 0.10f * a);
        ImRenderUtils::fillRectangle(r, baseCol, 1.0f, bRad, dl);
        ImRenderUtils::fillGradientRounded(dl, r,
            ImVec4(1, 1, 1, (hover ? 0.22f : 0.10f) * a), ImVec4(1, 1, 1, 0.0f),
            bRad, 0, /*vertical=*/true);
        ImColor edgeCol = hover ? ImColor(255, 255, 255, (int)(140 * a))
                                : ImColor(255, 255, 255, (int)(40 * a));
        dl->AddRect(ImVec2(r.x, r.y), ImVec2(r.z, r.w), edgeCol, bRad, 0, 1.0f);
    };
    // Icons redrawn on a shared 9px-high grid centered in the capsule: wider
    // glyph spread, thicker strokes with round caps — the old cramped 5px
    // triangles read as smudges inside the pads.
    auto gSkipFwd = [&](const ImVec4& r, ImU32 col) {
        const float gx = (r.x + r.z) * 0.5f, gy = (r.y + r.w) * 0.5f;
        const float h = 5.0f * scale, u = scale;
        dl->AddTriangleFilled(ImVec2(gx - 8.0f * u, gy - h), ImVec2(gx - 8.0f * u, gy + h),
                              ImVec2(gx - 2.5f * u, gy), col);
        dl->AddTriangleFilled(ImVec2(gx - 2.0f * u, gy - h), ImVec2(gx - 2.0f * u, gy + h),
                              ImVec2(gx + 3.5f * u, gy), col);
        dl->AddRectFilled(ImVec2(gx + 5.0f * u, gy - h), ImVec2(gx + 8.0f * u, gy + h),
                          col, 1.2f * u);
    };
    auto gSkipBack = [&](const ImVec4& r, ImU32 col) {
        const float gx = (r.x + r.z) * 0.5f, gy = (r.y + r.w) * 0.5f;
        const float h = 5.0f * scale, u = scale;
        dl->AddRectFilled(ImVec2(gx - 8.0f * u, gy - h), ImVec2(gx - 5.0f * u, gy + h),
                          col, 1.2f * u);
        dl->AddTriangleFilled(ImVec2(gx + 8.0f * u, gy - h), ImVec2(gx + 8.0f * u, gy + h),
                              ImVec2(gx + 2.5f * u, gy), col);
        dl->AddTriangleFilled(ImVec2(gx + 2.0f * u, gy - h), ImVec2(gx + 2.0f * u, gy + h),
                              ImVec2(gx - 3.5f * u, gy), col);
    };
    auto gPause = [&](const ImVec4& r, ImU32 col) {
        const float gx = (r.x + r.z) * 0.5f, gy = (r.y + r.w) * 0.5f;
        const float h = 5.5f * scale, u = scale;
        dl->AddRectFilled(ImVec2(gx - 5.5f * u, gy - h), ImVec2(gx - 1.6f * u, gy + h),
                          col, 1.2f * u);
        dl->AddRectFilled(ImVec2(gx + 1.6f * u, gy - h), ImVec2(gx + 5.5f * u, gy + h),
                          col, 1.2f * u);
    };
    auto gPlay = [&](const ImVec4& r, ImU32 col) {
        const float gx = (r.x + r.z) * 0.5f, gy = (r.y + r.w) * 0.5f;
        const float h = 5.5f * scale, u = scale;
        dl->AddTriangleFilled(ImVec2(gx - 4.5f * u, gy - h), ImVec2(gx - 4.5f * u, gy + h),
                              ImVec2(gx + 5.5f * u, gy), col);
    };

    const bool hPrev  = live && ImRenderUtils::isMouseOver(rPrev);
    const bool hPause = live && ImRenderUtils::isMouseOver(rPause);
    const bool hNext  = live && ImRenderUtils::isMouseOver(rNext);
    const bool clicked = ImGui::IsMouseClicked(0);
    // Own this frame's click so HudEditor doesn't start dragging the panel
    // out from under the button press.
    if (clicked && (hPrev || hPause || hNext)) mClickConsumed = true;

    pill(rPrev, hPrev);
    gSkipBack(rPrev, glyph);
    pill(rPause, hPause);
    if (s.paused) gPlay(rPause, glyph); else gPause(rPause, glyph);
    pill(rNext, hNext);
    gSkipFwd(rNext, glyph);

    ty += ctrlH + 10.0f * scale;

    // ---- Progress bar: full-width track with a round thumb knob ----
    const ImVec2 b0(tx, ty), b1(tx + colW, ty + barH);
    const float barR = barH * 0.5f;
    const float ratio = (lenMs > 0 && posMs > 0)
        ? std::min(1.0f, (float)posMs / (float)lenMs) : 0.0f;
    // Track: quiet dark pill with a single accent fill.
    ImRenderUtils::fillRectangle(ImVec4(b0.x, b0.y, b1.x, b1.y),
                                 ImColor(0, 0, 0, (int)(160 * a)), 1.0f, barR, dl);
    dl->AddRect(b0, b1, ImColor(255, 255, 255, (int)(18 * a)), barR, 0, 1.0f);
    if (ratio > 0.0f) {
        const float fillW = std::max((b1.x - b0.x) * ratio, barR);
        const ImVec4 fillPill(b0.x, b0.y, b0.x + fillW, b1.y);
        ImRenderUtils::fillRectangle(fillPill, accent, 1.0f, barR, dl);
        // TargetHUD-style 3D volume pass: a lit top fading out and a crisp
        // glossy top line — the fill reads as a shaded cylinder, not a stripe.
        ImRenderUtils::fillGradientRounded(dl, fillPill,
            ImVec4(1, 1, 1, 0.30f * a), ImVec4(1, 1, 1, 0.0f),
            barR, 0, /*vertical=*/true);
        dl->AddLine(ImVec2(fillPill.x + barR * 0.5f, fillPill.y + 1.0f),
                    ImVec2(fillPill.z - barR * 0.5f, fillPill.y + 1.0f),
                    IM_COL32(255, 255, 255, (int)(70 * a)), 1.0f);
    }

    // ---- Interactions ----
    ImGuiIO& io = ImGui::GetIO();
    if (clicked && hNext)  Next();
    if (clicked && hPrev)  Prev();
    if (clicked && hPause) PauseToggle();

    // Click on the bar = seek (drag keeps scrubbing while held).
    const bool overBar =
        io.MousePos.x >= b0.x - 4 && io.MousePos.x <= b1.x + 4 &&
        io.MousePos.y >= b0.y - 6 && io.MousePos.y <= b1.y + 6;
    const bool seekWanted = s.hasTrack && (s.playing || s.paused) && lenMs > 0 &&
                            overBar && (clicked || ImGui::IsMouseDown(0));
    if (clicked && overBar && s.hasTrack) mClickConsumed = true;
    if (seekWanted) {
        const float r = (io.MousePos.x - b0.x) / (b1.x - b0.x);
        const long to = (long)(std::max(0.0f, std::min(1.0f, r)) * (float)lenMs);
        wchar_t cmd[96];
        swprintf_s(cmd, L"seek %s to %ld", kAlias, to);
        if (mci(cmd)) {
            // MCI seek leaves playback halted; resume if we were playing.
            if (s.playing && !s.paused)
                mci(std::wstring(L"play ") + kAlias);
            g_posMs.store(to);
            g_anchorMs = to;
            g_anchorTick = GetTickCount64();
        }
    }
}
