#pragma once
#include "../Module.h"

class SelfNametag : public Module {
public:
    SelfNametag() : Module("SelfNametag", "Renders your nametag in third-person view", Category::Visual, 0) {}

    void OnEnable() override;
    void OnDisable() override;

private:
    unsigned char originalBytes[2] = {0};
    bool bytesSaved = false;
};
