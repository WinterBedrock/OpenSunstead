#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Forward declarations keep this header light. The real types live in the SDK
// (mce::Image is the game's raw skin bitmap) and in <d3d11.h>.
struct ID3D11ShaderResourceView;
namespace SDK { namespace mce { struct Image; } }

namespace SkinUtils {
    // True only when the image is an RGBA8 bitmap with a fully-populated byte
    // blob of the expected size (width*height*4) and sane dimensions.
    bool imageValid(const SDK::mce::Image& img);

    // Builds an outSize x outSize RGBA head icon: the 8x8 face region composited
    // with the 8x8 hat overlay, nearest-neighbour scaled. Empty on failure.
    std::vector<unsigned char> extractHeadRGBA(const SDK::mce::Image& img, int outSize = 64);

    // Copies the full skin bitmap as tightly-packed RGBA. Empty on failure.
    std::vector<unsigned char> fullSkinRGBA(const SDK::mce::Image& img, int& outW, int& outH);

    ID3D11ShaderResourceView* createTextureRGBA(const unsigned char* rgba, int w, int h);

    // Encodes RGBA -> PNG (no external deps) and writes it to fullPath.
    bool writePNG(const std::wstring& fullPath, const unsigned char* rgba, int w, int h);

    // %APPDATA%\Sunstead\Skins\  (created if missing). Trailing backslash.
    std::wstring skinsDir();

    // Strips characters illegal in a Windows filename.
    std::string sanitizeName(const std::string& name);

    void retireTexture(void* srv);
    void flushRetiredTextures();
}
