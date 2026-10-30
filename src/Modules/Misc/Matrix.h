#pragma once
#include "../Module.h"
#include <vector>
#include <string>
#include <cstdint>

// Matrix rain: falling green Mojangles glyphs. Draws only over ClickGUI,
// chat_screen and pause_screen — nowhere else.
class Matrix : public Module {
public:
    Matrix();
    void OnRender() override;
    void OnEnable() override;
    void OnDisable() override;

private:
    struct Drop {
        float x = 0.f;
        float y = 0.f;
        float speed = 80.f;
        int   len = 12;
        float phase = 0.f;
        std::vector<char> glyphs;
    };

    void ensureDrops(float screenW, float screenH, float fontSize);
    void reseatDrop(Drop& d, float screenW, float screenH, float fontSize, bool randomY);

    std::vector<Drop> mDrops;
    uint32_t mRng = 0xC0FFEEu;
    float mLastW = 0.f;
    float mLastH = 0.f;

    uint32_t nextU() {
        mRng ^= mRng << 13;
        mRng ^= mRng >> 17;
        mRng ^= mRng << 5;
        return mRng;
    }
    float nextF() { return (nextU() & 0xFFFFu) / 65535.f; }
};
