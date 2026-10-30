#include "SkinUtils.hpp"
#include "../../SDK/Network/Packet.h"
#include "../../Render/RenderManager.h"

#include <d3d11.h>
#include <Windows.h>
#include <ShlObj.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {

// ---- PNG / zlib primitives ------------------------------------------------
// Self-contained encoder. We avoid pulling in stb_image_write (absent here)
// and keep it tiny: zlib "stored" blocks (no compression) are perfectly valid
// and a 64x64 skin is only ~16 KB, so file size is a non-issue.

uint32_t crc32_of(const unsigned char* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        built = true;
    }
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint32_t adler32_of(const unsigned char* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void putBE32(std::vector<unsigned char>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);  v.push_back(x & 0xFF);
}

void writeChunk(std::vector<unsigned char>& out, const char* type, const std::vector<unsigned char>& data) {
    putBE32(out, (uint32_t)data.size());
    size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = crc32_of(out.data() + crcStart, out.size() - crcStart) ^ 0xFFFFFFFFu;
    putBE32(out, crc);
}

} // namespace

namespace {

struct RetiredTex { void* srv; unsigned long long t; };
std::vector<RetiredTex>& retiredTextures() {
    static std::vector<RetiredTex> v;
    return v;
}
constexpr unsigned long long kTexRetireGraceMs = 500;

} // namespace

namespace SkinUtils {

void retireTexture(void* srv) {
    if (!srv) return;
    retiredTextures().push_back({ srv, GetTickCount64() });
}

void flushRetiredTextures() {
    auto& r = retiredTextures();
    if (r.empty()) return;
    const unsigned long long now = GetTickCount64();
    for (size_t i = 0; i < r.size(); ) {
        if (now - r[i].t >= kTexRetireGraceMs) {
            ((ID3D11ShaderResourceView*)r[i].srv)->Release();
            r.erase(r.begin() + i);
        } else {
            ++i;
        }
    }
}

bool imageValid(const SDK::mce::Image& img) {
    // Validate by BYTE SIZE, not by the format enum. Bedrock skin images carry
    // 4-byte RGBA pixels but the `format` field is not reliably RGBA8Unorm on
    // player-list skins (Latite's own head cache ignores it and only checks
    // width*height*4 <= byteCount). Requiring RGBA8Unorm here rejected every
    // valid remote-player skin, leaving the TabList head column blank.
    if (img.mWidth == 0 || img.mHeight == 0) return false;
    if (img.mWidth > 4096 || img.mHeight > 4096) return false;
    const unsigned char* bytes = img.mImageBytes.data();
    size_t need = (size_t)img.mWidth * img.mHeight * 4u;
    if (bytes == nullptr || img.mImageBytes.size() < need) return false;
    // The vector HEADER lives in probed struct memory, but the heap buffer it
    // points at can still be stale/freed (player left, skin streamed out
    // mid-frame) — probe the actual bytes before the decode reads them. That
    // stale-buffer read was the intermittent TabList crash on tab press.
    return sdk_safe_ptr((void*)bytes, need);
}

std::vector<unsigned char> fullSkinRGBA(const SDK::mce::Image& img, int& outW, int& outH) {
    outW = outH = 0;
    if (!imageValid(img)) return {};
    outW = (int)img.mWidth;
    outH = (int)img.mHeight;
    size_t n = (size_t)outW * outH * 4u;
    std::vector<unsigned char> out(n);
    std::memcpy(out.data(), img.mImageBytes.data(), n);
    return out;
}

std::vector<unsigned char> extractHeadRGBA(const SDK::mce::Image& img, int outSize) {
    if (!imageValid(img) || outSize <= 0) return {};
    const unsigned w = img.mWidth, h = img.mHeight;
    // Standard skins are 64x64 (or legacy 64x32). Both place the face at (8,8)
    // and the hat overlay at (40,8), each 8x8, relative to a 64-wide sheet.
    const unsigned scale = (w >= 64u) ? (w / 64u) : 1u;
    const unsigned faceX = 8u * scale, faceY = 8u * scale;
    const unsigned hatX = 40u * scale, hatY = 8u * scale;
    const unsigned region = 8u * scale; // source square edge in px
    if (faceX + region > w || faceY + region > h) return {};
    const unsigned char* src = img.mImageBytes.data();

    auto sample = [&](unsigned px, unsigned py, unsigned char rgba[4]) {
        size_t idx = ((size_t)py * w + px) * 4u;
        rgba[0] = src[idx]; rgba[1] = src[idx + 1];
        rgba[2] = src[idx + 2]; rgba[3] = src[idx + 3];
    };

    std::vector<unsigned char> out((size_t)outSize * outSize * 4u);
    bool hasHat = (hatX + region <= w) && (hatY + region <= h);
    for (int oy = 0; oy < outSize; ++oy) {
        for (int ox = 0; ox < outSize; ++ox) {
            // Nearest-neighbour map output pixel -> source 8x8 region pixel.
            unsigned sx = (unsigned)((int64_t)ox * region / outSize);
            unsigned sy = (unsigned)((int64_t)oy * region / outSize);
            if (sx >= region) sx = region - 1;
            if (sy >= region) sy = region - 1;

            unsigned char base[4]; sample(faceX + sx, faceY + sy, base);
            if (hasHat) {
                unsigned char hat[4]; sample(hatX + sx, hatY + sy, hat);
                if (hat[3] > 0) {
                    // Alpha-composite hat over face.
                    float a = hat[3] / 255.0f, ia = 1.0f - a;
                    base[0] = (unsigned char)(hat[0] * a + base[0] * ia);
                    base[1] = (unsigned char)(hat[1] * a + base[1] * ia);
                    base[2] = (unsigned char)(hat[2] * a + base[2] * ia);
                    base[3] = (unsigned char)std::max<int>(base[3], hat[3]);
                }
            }
            size_t o = ((size_t)oy * outSize + ox) * 4u;
            out[o] = base[0]; out[o + 1] = base[1]; out[o + 2] = base[2]; out[o + 3] = base[3];
        }
    }
    return out;
}

ID3D11ShaderResourceView* createTextureRGBA(const unsigned char* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return nullptr;
    ID3D11Device* dev = RenderManager::GetInstance().GetDevice();
    if (!dev) return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sr{};
    sr.pSysMem = rgba;
    sr.SysMemPitch = (UINT)w * 4u;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&desc, &sr, &tex)) || !tex) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = desc.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = dev->CreateShaderResourceView(tex, &sd, &srv);
    tex->Release();
    return SUCCEEDED(hr) ? srv : nullptr;
}

bool writePNG(const std::wstring& fullPath, const unsigned char* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return false;

    // 1) Build the raw filtered scanline buffer (filter byte 0 per row).
    std::vector<unsigned char> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0); // filter: None
        const unsigned char* row = rgba + (size_t)y * w * 4u;
        raw.insert(raw.end(), row, row + (size_t)w * 4u);
    }

    // 2) Wrap in a zlib stream using stored (uncompressed) deflate blocks.
    std::vector<unsigned char> zlib;
    zlib.push_back(0x78); zlib.push_back(0x01); // CMF/FLG (no preset dict, check OK)
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t block = std::min<size_t>(65535, raw.size() - pos);
        bool last = (pos + block >= raw.size());
        zlib.push_back(last ? 1 : 0);                 // BFINAL + BTYPE=00 (stored)
        zlib.push_back(block & 0xFF);                 // LEN  (little-endian)
        zlib.push_back((block >> 8) & 0xFF);
        zlib.push_back(~block & 0xFF);                // NLEN
        zlib.push_back((~block >> 8) & 0xFF);
        zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + block);
        pos += block;
    }
    putBE32(zlib, adler32_of(raw.data(), raw.size()));

    // 3) Assemble the PNG.
    std::vector<unsigned char> png = { 137, 80, 78, 71, 13, 10, 26, 10 };
    std::vector<unsigned char> ihdr;
    putBE32(ihdr, (uint32_t)w);
    putBE32(ihdr, (uint32_t)h);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // color type RGBA
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0); // compression/filter/interlace
    writeChunk(png, "IHDR", ihdr);
    writeChunk(png, "IDAT", zlib);
    writeChunk(png, "IEND", {});

    // 4) Write to disk (wide path, binary).
    FILE* f = _wfopen(fullPath.c_str(), L"wb");
    if (!f) return false;
    size_t written = fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return written == png.size();
}

std::wstring skinsDir() {
    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
        return L"";
    std::wstring dir = appdata;
    dir += L"\\Sunstead";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\Skins";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\";
    return dir;
}

std::string sanitizeName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 0x20)
            out.push_back('_');
        else
            out.push_back(c);
    }
    if (out.empty()) out = "Player";
    if (out.size() > 48) out.resize(48);
    return out;
}

} // namespace SkinUtils
