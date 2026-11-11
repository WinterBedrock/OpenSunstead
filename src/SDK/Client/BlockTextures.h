#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ImDrawList;


namespace SDK {

bool blockTexturesAvailable();

std::vector<std::string> blockTextureList();

void drawBlockTexture3D(ImDrawList* dl, float x, float y, float size,
                        const char* blockName, float alpha = 1.0f);

bool drawBlockTextureFlat(ImDrawList* dl, float x, float y, float w, float h,
                          const char* blockName, float alpha = 1.0f);

} // namespace SDK
