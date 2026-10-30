#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ImDrawList;

// Real block textures from the game's brarchive, drawn through ImGui.
// Archive (data\resource_packs\vanilla\__brarchive\textures\blocks.brarchive;
// same container all version packs use): magic 7D 27 25 B1 A0 52 70 26,
// +0x08 u32 recordCount, +0x0C u32 version, then recordCount 0x100-byte
// records ({u8 nameLen; name; zero pad; u32 blobSize @ +0xFC}) followed by the
// raw PNG blobs back to back. Block name -> texture name from blocks.json
// (commented JSON), texture -> "textures/blocks/xxx" from terrain_texture.json;
// base + ".png" is the record name. Decoded with stb_image (PNG) and uploaded
// via SkinUtils::createTextureRGBA so it draws ON TOP of the HUD; the 3D form
// is a per-face-shaded iso cube from the block's real top/side textures.

namespace SDK {

// True when the archive and both JSON mappings were found and parsed.
bool blockTexturesAvailable();

// Every block id the merged vanilla blocks.json knows. Empty when unavailable.
std::vector<std::string> blockTextureList();

// Draws the block's real texture as a 3D isometric cube on `dl` at (x, y),
// `size` px, honouring the HUD alpha. Draws nothing if unresolvable.
void drawBlockTexture3D(ImDrawList* dl, float x, float y, float size,
                        const char* blockName, float alpha = 1.0f);

// Flat single-texture form. Returns true when something was drawn.
bool drawBlockTextureFlat(ImDrawList* dl, float x, float y, float w, float h,
                          const char* blockName, float alpha = 1.0f);

} // namespace SDK
