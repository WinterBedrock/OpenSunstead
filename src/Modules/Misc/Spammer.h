#pragma once
#include "../Module.h"

// Spammer - periodically sends a configured chat message from the client.
// Supports a static delay or a randomized delay range, plus an optional
// "bypass" mode that appends 6 random characters to the message to dodge
// duplicate-message filters.
class Spammer : public Module {
public:
    Spammer() : Module("Spammer", "Automatically sends a chat message on a timer", Category::Misc, 0) {
        AddSetting(new StringSetting("Message", "Text that will be sent each interval", "Im a sunstead user! <3"));
        AddSetting(new EnumSetting("Delay Type", "Static = fixed delay, Random = random delay in a range", 0, { "Static", "Random" }));
        AddSetting(new NumberSetting("Delay", "Seconds between messages (Static mode)", 3.0f, 1.0f, 10.0f, 1.0f));
        AddSetting(new NumberSetting("Min Delay", "Minimum seconds between messages (Random mode)", 2.0f, 1.0f, 10.0f, 1.0f));
        AddSetting(new NumberSetting("Max Delay", "Maximum seconds between messages (Random mode)", 6.0f, 1.0f, 10.0f, 1.0f));
        AddSetting(new BoolSetting("Bypass", "Append 6 random characters to bypass spam filters", false));

        UpdateSettingsVisibility();
    }
    void OnEnable() override;
    void OnTick() override;
    void UpdateSettingsVisibility() override;

private:
    long long mLastSendMs = 0;   // timestamp of last send (ms)
    long long mNextDelayMs = 0;  // currently scheduled delay (ms)
};
