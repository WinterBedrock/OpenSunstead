#pragma once
#include "../Module.h"
#include "../../Utils/SoundCloud.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <mutex>

class Player : public Module {
public:
    struct State {
        std::string title = "Nothing playing";
        std::string artist;
        std::string artworkUrl;
        long long durationMs = 0;
        bool hasTrack = false;
        bool loading = false;
        bool playing = false;
        bool paused = false;
        bool isPreview = false;
        std::string note;                  // error / hint line
    };

    Player();

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;
    void OnFirstLayout(float screenW, float screenH) override {
        if (mPosX == 24.0f && mPosY == 120.0f) {
            mPosX = screenW * 0.5f - 170.0f;
            mPosY = screenH - 120.0f;
        }
    }

    // Command entry points (thread-safe; network runs on a worker thread and
    // every MCI call is deferred to the render thread via pump()).
    static void SetKey(const std::string& id);
    static std::string GetKey();
    static void Play(const std::string& query);
    static void PlayRandom();
    static void Next() { PlayRandom(); }
    static void Prev();
    static void Stop();
    static void PauseToggle();
    static void SetVolume(int v);          // 0..100
    static std::string StatusLine();
    static void SetProfile(const std::string& urlOrName);
    static std::string GetProfile();
    static bool ProfileResolved();         // numeric user id is cached

    // Appear/disappear driver (0 = hidden, 1 = shown) — drives the scale-up
    // + fade of the whole plate so enable/disable and editor drags animate.
    float mAppear = 0.0f;

private:
    static State snapshot();
    static void setStatus(const char* title, const char* artist, const char* note,
                          bool playing, bool paused, bool hasTrack, long long durationMs,
                          const std::string& artworkUrl, bool isPreview);
    static void workerPlay(std::string key, std::string query, int gen);
    static void workerRandom(std::string key, int gen);
    static void workerTrack(SoundCloud::Track t, int gen, const char* pickLabel = "");
    static void failTrack(const std::string& err, int gen);
    static void workerArtwork(std::string url);
    static void workerResolveProfile(std::string urlOrName);
    static std::string tempAudioPath(int gen);
    static std::string mciErrorText(unsigned long err);
    static bool mci(const std::wstring& cmd, std::wstring* out = nullptr);
    static long mciStatus(const wchar_t* item);   // position/length in ms, -1 on failure
    static std::string mciMode();
    static bool isCurrent(int gen);
    static void pump(bool enabled);      // deferred requests + position poll
    static void openPending();           // consume a finished download
    static void closeStream();
    // --- any thread ---------------------------------------------------------
    static void rememberCurrent();       // push the outgoing track for Prev
    static void ensureArtwork(const std::vector<unsigned char>& jpg);
    static void dropArtwork();
};
