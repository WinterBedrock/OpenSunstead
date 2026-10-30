#pragma once
#include "../Module.h"
#include <mutex>
#include <Windows.h>

// Chat — Rise-style (Java-like) chat menu for Bedrock
class Chat : public Module {
public:
    Chat() : Module("Chat", "Java-style chat menu with history (replaces the chat key)", Category::Visual, 0x54 /* 'T' */) {
        sInstance = this;
        mKeybindToggles = false;
        mIsHudModule = true;   // HUD-editor: drag on pause screen
        mPosY = 0.f;            // bottom-anchor sentinel (see OnRender)
        AddSetting(new NumberSetting("Width", "Panel width in px", 440.0f, 240.0f, 720.0f, 10.0f));
        AddSetting(new NumberSetting("Open Height", "Panel height in px while open", 190.0f, 100.0f, 500.0f, 10.0f));
        AddSetting(new NumberSetting("Closed Lines", "Lines kept visible while closed", 7.0f, 1.0f, 20.0f, 1.0f));
        AddSetting(new NumberSetting("Fade Time", "Seconds a closed-panel line stays visible", 5.0f, 1.0f, 15.0f, 0.5f));
        AddSetting(new NumberSetting("Dim", "Background dim (plate) opacity", 0.55f, 0.0f, 1.0f, 0.05f));
        AddSetting(new EnumSetting("Font", "Chat text font", 0, { "Theme", "Minecraft" }));                          // [5]
        AddSetting(new NumberSetting("Blur", "Backdrop blur strength", 7.0f, 0.0f, 25.0f, 0.5f));                  // [6]
        AddSetting(new NumberSetting("Rounding", "Panel corner rounding", 6.0f, 0.0f, 24.0f, 1.0f));               // [7]
        AddSetting(new BoolSetting("Shadow", "Soft glow around the panel", false));                                // [8]
        AddSetting(new NumberSetting("Shadow Opacity", "Glow strength when Shadow is on", 0.5f, 0.0f, 1.0f, 0.05f)); // [9]
        AddSetting(new BoolSetting("Enable Links", "Make links in messages clickable (opens the browser)", true)); // [10]
        AddSetting(new NumberSetting("Open Lines", "Max history lines shown in the upper chat while open", 12.0f, 3.0f, 50.0f, 1.0f)); // [11]
        UpdateSettingsVisibility();
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;
    void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) override;

    void UpdateSettingsVisibility();

    // --- state shared with the WndProc / displayClientMessage hook ---
    static bool IsOpen() { return instance() && instance()->mOpen; }
    static Chat* instance() { return sInstance; }
    static bool RecordMessage(const std::string& line);
    static bool HandlesOpenKey(int vk);
    static bool PrefillsSlash(int vk) { return vk == VK_OEM_2 /* '/' */; }

    // Stamp that a command was typed in the VANILLA chat. It is executed one
    // render frame later (GameHooks queues it out of the send hook), by which
    // time the chat screen has closed; without this marker RecordMessage would
    // treat the command's reply as "the module owns the chat" and swallow it,
    // so a vanilla-chat command would look like it ran nothing.
    static void NoteVanillaCommand();

    // The next RecordMessage keeps the line in this panel and also lets it
    // through to the real HUD chat. Translate Delay cancels the packet, so
    // this is how that line still reaches vanilla chat.
    static void ArmVanillaPassthrough();
    // Clears the one-shot and reports whether it was still armed.
    static bool DisarmVanillaPassthrough();

    // The chat input buffer is OWNED by the chat (persistent).
    static char* InputBuffer();
    static void setInputBuffer(const char* text);
    std::string InputText() const;

    // WndProc routes keys here while our menu is open (the input is hand-drawn,
    // the same ToUnicode/reveal/sine-caret machinery as the ClickGUI search).
    static void FeedKeyDown(unsigned int vk);

    void Open(const std::string& prefill = std::string()) {
        mOpen = true;
        setInputBuffer(prefill.c_str());
        mHistoryIndex = -1;
        mSavedDraft.clear();
    }
    void Close() {
        if (!mOpen) return;
        mOpen = false;
        mHistoryIndex = -1;
        mSavedDraft.clear();
        mScrollTargetPx = 0.f;   // glide back to the bottom (Rise)
        setInputBuffer("");      // Rise clears the typed text on close
    }

    static std::vector<std::string> sentSnapshot();
    int  HistoryIndex() const { return mHistoryIndex; }
    void SetHistoryIndex(int i) { mHistoryIndex = i; }
    void SaveDraft(const std::string& s) { mSavedDraft = s; }
    const std::string& Draft() const { return mSavedDraft; }

    void SendCurrent();
    static void AddLine(const std::string& text);

private:
    // Rise's retargetable eased value (Animation + Easing, verbatim):
    //   EASE_OUT_EXPO    1 - 2^(-10t)            (height, slide-in; 500ms)
    //   EASE_OUT_ELASTIC 2^(-10t)sin((10t-0.75)2π/3)/2 + 1   (input open; 850ms)
    //   EASE_IN_EXPO     2^(10t-10)             (input close; 300ms)
    //   LINEAR                                 (input alpha; 400ms)
    enum RiseEasing { kEaseOutExpo = 0, kEaseInExpo, kEaseLinear, kEaseOutElastic };
    struct RiseAnim {
        float value = 0.f, target = 0.f, from = 0.f;
        double t0 = 0.0;
        float durMs = 500.f;
        int ease = kEaseOutExpo;
        void set(float t, float d, int e) {
            if (target == t && durMs == d && ease == e) return;
            target = t; durMs = d; ease = e; from = value; t0 = nowMs();
        }
        void bump(float dv, float d, int e) {
            value += dv; from = value; durMs = d; ease = e; t0 = nowMs();
        }
        float tick();
        static double nowMs();
    };

    bool mOpen = false;

    RiseAnim mHeightAnim;        // panel height, px (bottom-pinned)
    RiseAnim mSlideIn;           // new-message slide-in, px
    RiseAnim mInputPop;          // input row scale (elastic in / expo out)
    RiseAnim mInputAlpha;        // input row alpha

    float mScrollPx = 0.f;
    float mScrollTargetPx = 0.f;
    float mVisibleRows = 0.f;     // Rise visibleLines counter
    unsigned long long mFadeStampMs = 0;
    size_t mLastRowCount = 0;

    std::string mSavedDraft;
    int mHistoryIndex = -1;

    struct Line {
        std::string text;                  // RAW, § codes intact
        unsigned long long atMs;
    };
    static std::mutex sHistoryMutex;
    static std::vector<Line>& lines() { static std::vector<Line> l; return l; }
    static std::vector<std::string>& sent() { static std::vector<std::string> s; return s; }
    static void pushSent(const std::string& t);

    static Chat* sInstance;
};
