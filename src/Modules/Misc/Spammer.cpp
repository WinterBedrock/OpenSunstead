#include "Spammer.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Network/ClientPackets.h"
#include <chrono>
#include <random>
#include <string>

static long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int RandInt(int lo, int hi) {
    static std::mt19937 rng{ std::random_device{}() };
    if (hi < lo) std::swap(lo, hi);
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng);
}

static std::string RandomSuffix(int len) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string out;
    out.reserve(len);
    for (int i = 0; i < len; ++i)
        out += charset[RandInt(0, (int)sizeof(charset) - 2)];
    return out;
}

// Min/Max delay sliders only make sense in Random mode; the single Delay slider
// only makes sense in Static mode. The ClickGUI calls this every frame for the
// module being displayed, so visibility stays correct with no side effects.
void Spammer::UpdateSettingsVisibility() {
    auto* typeSet = static_cast<EnumSetting*>(mSettings[1]);
    bool random = (typeSet->mValue == 1);
    mSettings[2]->mVisible = !random; // Delay
    mSettings[3]->mVisible = random;  // Min Delay
    mSettings[4]->mVisible = random;  // Max Delay
}

void Spammer::OnEnable() {
    // Reset timing so the first message fires after one full delay window.
    mLastSendMs = NowMs();
    mNextDelayMs = 0;
}

void Spammer::OnTick() {
    // OnTick is also invoked by the ClickGUI when settings change, so guard the
    // send logic with the enabled state to avoid sending while disabled.
    if (!IsEnabled()) return;

    // NOTE: this SDK build has no reliable screen-name detection, so we simply
    // rely on the chat controller having been captured (chat opened once).

    auto* msgSet  = static_cast<StringSetting*>(mSettings[0]);
    auto* typeSet = static_cast<EnumSetting*>(mSettings[1]);
    auto* delSet  = static_cast<NumberSetting*>(mSettings[2]);
    auto* minSet  = static_cast<NumberSetting*>(mSettings[3]);
    auto* maxSet  = static_cast<NumberSetting*>(mSettings[4]);
    auto* bypass  = static_cast<BoolSetting*>(mSettings[5]);

    long long now = NowMs();

    // (Re)compute the scheduled delay if it hasn't been set this cycle.
    if (mNextDelayMs <= 0) {
        if (typeSet->mValue == 1) {
            mNextDelayMs = (long long)RandInt((int)minSet->mValue, (int)maxSet->mValue) * 1000LL;
        } else {
            mNextDelayMs = (long long)delSet->mValue * 1000LL;
        }
    }

    if (now - mLastSendMs < mNextDelayMs) return;

    std::string text = msgSet->mValue;
    if (text.empty()) text = "Sunstead - best Minecraft Bedrock utility mod!";
    if (bypass->mValue) {
        text += " ";
        text += RandomSuffix(6);
    }

    SDK::ClientPackets::sendText(text);

    mLastSendMs = now;
    mNextDelayMs = 0; // re-roll the random delay next cycle
}