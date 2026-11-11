#pragma once
#include <string>

struct ImDrawList;
struct ID3D11Resource;

namespace SDK {
namespace GameIcon {

void noteBlock(const std::string& name, void* block);

bool draw(ImDrawList* dl, float x, float y, float size, const std::string& name, float alpha);

void onUiRender(void* mcuirc, const std::string& screenName);

void onPresent(ID3D11Resource* backBuffer, bool isDX12);

void clear();

} // namespace GameIcon
} // namespace SDK
