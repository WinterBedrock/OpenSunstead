#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Forward declarations keep this header light. The real types live in the SDK
// (mce::Image is the game's raw skin bitmap) and in <d3d11.h>.
struct ID3D11ShaderResourceView;
namespace SDK { namespace mce { struct Image; } }

// SkinUtils — shared helpers for the SkinStealer / TabList ports.
//
// The target client renders exclusively through ImGui + D3D11 (no Bedrock
// DrawContext / TexturePtr path like Latite), so skins have to be turned into
// real D3D11 textures and saved with a self-contained PNG encoder. Everything
// here is allocation-bounded and validates the source bitmap before touching
// its bytes, because skin blobs come straight from networked player data.
namespace SkinUtils {
    // True only when the image is an RGBA8 bitmap with a fully-populated byte
    // blob of the expected size (width*height*4) and sane dimensions.
    bool imageValid(const SDK::mce::Image& img);

    // Builds an outSize x outSize RGBA head icon: the 8x8 face region composited
    // with the 8x8 hat overlay, nearest-neighbour scaled. Empty on failure.
    std::vector<unsigned char> extractHeadRGBA(const SDK::mce::Image& img, int outSize = 64);

    // Copies the full skin bitmap as tightly-packed RGBA. Empty on failure.
    std::vector<unsigned char> fullSkinRGBA(const SDK::mce::Image& img, int& outW, int& outH);

    // Creates a D3D11 SRV (DXGI_FORMAT_R8G8B8A8_UNORM) usable as an ImGui
    // texture id. Caller owns the returned SRV and must Release() it. nullptr
    // on failure (e.g. device not ready).
    ID3D11ShaderResourceView* createTextureRGBA(const unsigned char* rgba, int w, int h);

    // Encodes RGBA -> PNG (no external deps) and writes it to fullPath.
    bool writePNG(const std::wstring& fullPath, const unsigned char* rgba, int w, int h);

    // %APPDATA%\Sunstead\Skins\  (created if missing). Trailing backslash.
    std::wstring skinsDir();

    // Strips characters illegal in a Windows filename.
    std::string sanitizeName(const std::string& name);

    // DEFERRED GPU texture release. A module closing mid-frame (TabList on
    // tab spam) used to Release() its head SRVs while the frame's already-
    // recorded ImGui draw data still referenced them — the driver then
    // faulted at Present (the tab-spam crash). Retired SRVs stay alive for
    // a grace period and are actually released once no pending draw can
    // reference them. Call flushRetiredTextures() once per frame.
    void retireTexture(void* srv);
    void flushRetiredTextures();
}
