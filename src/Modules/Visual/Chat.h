#pragma once
#include "../Module.h"
#include <mutex>
#include <Windows.h>

// custom chat like rise cool yees
class Chat : public Module {
public:
    Chat() : Module("Chat", "Java-style chat menu with history (replaces the chat key)", Category::Visual, 0x54 /* 'T' */) {
        sInstance = this;
        mKeybindToggles = false;
        mIsHudModule = true;
        mPosY = 0.f;
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

    static bool IsOpen() { return instance() && instance()->mOpen; }
    static Chat* instance() { return sInstance; }
    static bool RecordMessage(const std::string& line);
    static bool HandlesOpenKey(int vk);
    static bool PrefillsSlash(int vk) { return vk == VK_OEM_2 /* '/' */; }

    static void NoteVanillaCommand();

    static void ArmVanillaPassthrough();
    static bool DisarmVanillaPassthrough();

    static char* InputBuffer();
    static void setInputBuffer(const char* text);
    std::string InputText() const;

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
        mScrollTargetPx = 0.f;
        setInputBuffer("");
    }

    static std::vector<std::string> sentSnapshot();
    int  HistoryIndex() const { return mHistoryIndex; }
    void SetHistoryIndex(int i) { mHistoryIndex = i; }
    void SaveDraft(const std::string& s) { mSavedDraft = s; }
    const std::string& Draft() const { return mSavedDraft; }

    void SendCurrent();
    static void AddLine(const std::string& text);

private:
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

    RiseAnim mHeightAnim;
    RiseAnim mSlideIn;
    RiseAnim mInputPop;
    RiseAnim mInputAlpha;

    float mScrollPx = 0.f;
    float mScrollTargetPx = 0.f;
    float mVisibleRows = 0.f;
    unsigned long long mFadeStampMs = 0;
    size_t mLastRowCount = 0;

    std::string mSavedDraft;
    int mHistoryIndex = -1;

    struct Line {
        std::string text;
        unsigned long long atMs;
    };
    static std::mutex sHistoryMutex;
    static std::vector<Line>& lines() { static std::vector<Line> l; return l; }
    static std::vector<std::string>& sent() { static std::vector<std::string> s; return s; }
    static void pushSent(const std::string& t);

    static Chat* sInstance;
};
