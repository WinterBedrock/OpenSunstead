#pragma once
#include "../Module.h"
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Translate — translates incoming chat text through Google Translate's public
// endpoint. The source language is left unspecified, so the service detects it.
//
//   Mode "Delay"  — the message is held back and only shown once the
//                   translation is ready.
//   Mode "Resend" — the original reaches the client at once; the translation
//                   is shown again afterwards as its own line.
//
// "Suffix" (on by default) appends " [T]" to every translated line.
class Translate : public Module {
public:
    Translate() : Module("Translate", "Translates incoming chat messages", Category::Misc, 0) {
        AddSetting(new EnumSetting("Mode", "Delay holds a message until it is translated; Resend shows the original first",
                                   0, { "Delay", "Resend" }));
        AddSetting(new EnumSetting("Language", "Language incoming messages are translated into",
                                   0, { "English", "Spanish", "Chinese", "Russian" }));
        AddSetting(new BoolSetting("Suffix", "Append [T] to translated messages", true));
        // All no-key. Mozhi is default: it proxies Google server-side, so a
        // per-IP "automated queries" 429 from translate.googleapis.com does not
        // touch it. MyMemory has a small anonymous daily quota.
        AddSetting(new EnumSetting("Provider", "Translation service to use",
                                   1, { "Google", "Mozhi", "MyMemory" }));   // [3]
        // Off by default: stripping keeps colour/format codes out of the
        // request so weak engines don't mangle them, but the translation then
        // loses every colour. [4]
        AddSetting(new BoolSetting("Remove color codes", "Strip § colour/format codes before translating", false));
        // APPENDED [5]: translate scoreboard line entries (SetScore packets).
        // The scoreboard TITLES (SetDisplayObjective packets) are never
        // touched — only the per-line entries the sidebar/next-to-name lists
        // show. Translation results are cached per original text so a stable
        // scoreboard does not spam the translation worker every re-send.
        AddSetting(new BoolSetting("Scoreboard", "Translate scoreboard line entries (not the titles)", false));   // [5]
    }

    void OnDisable() override;
    // Applies finished translations on the game thread.
    void OnTick() override;
    void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) override;
    // SetScore packet hook (called for packet id 108 from the receive hook).
    // May set cancel to defer delivery until the rows are translated.
    void OnSetScore(class SDK::Packet* packet, bool& cancel);
    // SetScoreboardIdentity packet hook (id 112 — the fake-name registration
    // table servers like Hive use for sidebar rows). Same hold/translate/
    // re-deliver pipeline as OnSetScore; rows are string_views.
    void OnSetScoreboardIdentity(class SDK::Packet* packet, bool& cancel);

private:
    struct Job {
        unsigned token = 0;
        std::string body;     // the part that gets translated
        std::string prefix;   // kept as-is (the "author > " of a chat line)
        std::string lang;
        int provider = 0;     // 0 Google, 1 Mozhi, 2 MyMemory
        bool delay = false;
        bool stripColors = false;
        unsigned long long bornMs = 0;
        bool scoreboard = false;   // SetScore entry: result goes to the score cache
    };

    struct Result {
        unsigned token = 0;
        bool ok = false;
        std::string prefix;   // "author > " — Resend has no waiting job to read it from
        std::string text;
    };

    void workerLoop();
    void startWorker();
    void stopWorker();
    void enqueue(Job job);
    bool suffixOn() const;

    // Thread proc. unsigned long __stdcall matches CreateThread without pulling
    // Windows.h into this header (DWORD/WINAPI are not visible here).
    static unsigned long __stdcall workerThunk(void* self);

    std::mutex mMutex;
    std::deque<Job> mQueue;
    std::deque<Result> mResults;
    // Tokens of Delay jobs still waiting. OnTick prints them translated, or
    // verbatim once they outlive the give-up window.
    std::deque<Job> mWaiting;

    void* mWorker = nullptr;   // HANDLE; void* keeps <Windows.h> out of the header
    volatile bool mStop = false;
    unsigned mNextToken = 1;

    // Scoreboard translation cache: original line -> translated line. Written
    // on the game thread from finished scoreboard jobs, read from the packet
    // hook; guarded by mMutex. Capped — a rotating scoreboard would otherwise
    // grow it without bound.
    std::map<std::string, std::string> mScoreCache;

    // SetScore packets HELD (cancelled at the hook) until every FakePlayer
    // row text has a translation (or the give-up fires), then re-delivered
    // through GameHooks::ResendIncomingPacket on the network thread. This is
    // why the scoreboard never stalls: nothing blocks the receive thread.
    //
    // Two row shapes:
    //  - SetScore (108): an owning std::string at namePtr (assign = rewrite).
    //  - SetScoreboardIdentity (112): a std::string_view at namePtr — writing
    //    means pointing (ptr at namePtr, length at lenAddr) into
    //    mScoreStringPool, which outlives the packet being processed.
    struct PendingScoreRow {
        void* namePtr = nullptr;
        uintptr_t lenAddr = 0;     // non-zero => string_view row (identity packet)
        std::string original;      // text before translation
        bool queued = false;       // a worker job for `original` was enqueued
    };
    struct PendingScorePacket {
        class SDK::Packet* raw = nullptr;   // lifetime owned by GameHooks' hold map
        int realId = 108;                    // 108 (SetScore) or 112 (Identity)
        std::vector<PendingScoreRow> rows;
        unsigned long long bornMs = 0;
    };
    std::deque<PendingScorePacket> mPendingScores;   // game thread only
    // Owns translated texts that held string_view rows point at. Deque, so
    // element addresses stay stable on push/pop at the ends.
    std::deque<std::string> mScoreStringPool;

    // Rewrites a held packet's rows with whatever translations the cache has
    // (string assign for SetScore rows, pooled view redirect for identity
    // rows) and queues its re-delivery. Game thread; mMutex must be held.
    void deliverPending(PendingScorePacket& p, const char* how);
    // Texts with a translation job currently in flight, and texts whose last
    // translation failed (delivered verbatim, retried on the next NEW text).
    std::unordered_set<std::string> mScoreInFlight;
    std::unordered_set<std::string> mScoreFailed;
};
