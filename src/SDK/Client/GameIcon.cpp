#include "GameIcon.h"
#include "ItemIcon.h"
#include "ClientInstance.h"
#include "../Types.h"
#include "../../Render/RenderManager.h"
#include "../../Utils/MiscUtils/SkinUtils.hpp"
#include "../../Core/Logger.h"
#include <Windows.h>
#include <d3d11.h>
#include <imgui.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SDK {
namespace GameIcon {

namespace {

// MinecraftUIRenderContext vtable (0x14E9948B0 on 1.26.50.04):
//   [9]  flushImages   [14] drawRectangle   [15] fillRectangle
// fillRectangle(this, RectangleArea const& {x0,x1,y0,y1}, mce::Color const&, float alpha)
constexpr int kFillRectSlot = 15;
// First bytes of fillRectangle (sub_146635CD0) — checked before every use.
const unsigned char kFillProlog[] = { 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x68, 0x0F, 0x29, 0x74, 0x24, 0x50, 0x0F, 0x28 };

constexpr int kIcon = 64;         // icon edge in screen pixels (16 * 4)
constexpr int kMargin = 4;        // fill margin around the icon
constexpr int kCell = kIcon + 2 * kMargin;
constexpr int kMaxFrames = 40;    // give up on a capture after this many presents

struct RectArea { float x0, x1, y0, y1; };
struct McColor  { float r, g, b, a; };

struct Job {
    std::string name;
    void* block = nullptr;
    int frames = 0;
    // Pixel origin (top-left) of the black cell; white cell is right next to it.
    int px = 0, py = 0;
    bool drawn = false;
};

std::mutex g_mx;
std::unordered_map<std::string, void*> g_blocks;            // name -> Block*
std::unordered_map<std::string, ID3D11ShaderResourceView*> g_tex;
std::unordered_set<std::string> g_failed;
std::vector<std::string> g_queue;
Job g_job;
bool g_jobActive = false;
bool g_loggedFill = false;

std::string norm(const std::string& n) {
    std::string s = n;
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    const size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    return s;
}

bool sehFill(void* mcuirc, const RectArea* r, const McColor* c) {
    __try {
        void** vt = *(void***)mcuirc;
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * (kFillRectSlot + 1))) return false;
        void* fn = vt[kFillRectSlot];
        if (!fn || !sdk_safe_ptr(fn, sizeof(kFillProlog))) return false;
        if (memcmp(fn, kFillProlog, sizeof(kFillProlog)) != 0) return false;
        using fill_t = void(__fastcall*)(void*, const RectArea*, const McColor*, float);
        ((fill_t)fn)(mcuirc, r, c, 1.0f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

float uiFrac() {
    ClientInstance* ci = ClientInstance::get();
    GuiData* gd = ci ? ci->getGuiData() : nullptr;
    float frac = gd ? gd->getGuiScaleFrac() : 0.f;
    if (frac <= 0.f) {
        const float s = gd ? gd->getGuiScale() : 0.f;
        frac = (s > 0.f) ? 1.f / s : 1.f;
    }
    return frac;
}

void startNextJobLocked() {
    while (!g_queue.empty()) {
        std::string n = g_queue.front();
        g_queue.erase(g_queue.begin());
        if (g_tex.count(n) || g_failed.count(n)) continue;
        auto it = g_blocks.find(n);
        if (it == g_blocks.end() || !it->second) continue;
        g_job = Job{};
        g_job.name = n;
        g_job.block = it->second;
        g_jobActive = true;
        return;
    }
}

} // namespace

void noteBlock(const std::string& name, void* block) {
    if (name.empty() || !block) return;
    const std::string n = norm(name);
    std::lock_guard<std::mutex> lk(g_mx);
    g_blocks[n] = block;
}

bool draw(ImDrawList* dl, float x, float y, float size, const std::string& name, float alpha) {
    if (!dl || name.empty() || size <= 1.f) return false;
    const std::string n = norm(name);
    ID3D11ShaderResourceView* srv = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_tex.find(n);
        if (it != g_tex.end()) srv = it->second;
        else if (!g_failed.count(n) && g_blocks.count(n)) {
            bool queued = (g_jobActive && g_job.name == n);
            for (auto& q : g_queue) if (q == n) { queued = true; break; }
            if (!queued) g_queue.push_back(n);
            if (!g_jobActive) startNextJobLocked();
        }
    }
    if (!srv) return false;
    int a = (int)(alpha * 255.f + 0.5f);
    if (a < 0) a = 0; if (a > 255) a = 255;
    dl->AddImage((ImTextureID)srv, ImVec2(x, y), ImVec2(x + size, y + size),
                 ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
    return true;
}

void onUiRender(void* mcuirc, const std::string& screenName) {
    if (!mcuirc) return;
    if (screenName != "hud_screen") return;
    if (!itemIconAvailable()) return;

    Job job;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_jobActive) startNextJobLocked();
        if (!g_jobActive) return;
        job = g_job;
    }

    ClientInstance* ci = ClientInstance::get();
    GuiData* gd = ci ? ci->getGuiData() : nullptr;
    if (!gd) return;
    const float frac = uiFrac();
    if (gd->getHeight() < kCell + 8 || gd->getWidth() < 2 * kCell + 8) return;

    // Top-left scratch cells, pixel aligned (independent of window height, so
    // GuiData and back buffer agree even if their heights ever differ).
    const int px = 2;
    const int py = 2;

    const McColor black{ 0.f, 0.f, 0.f, 1.f };
    const McColor white{ 1.f, 1.f, 1.f, 1.f };
    for (int i = 0; i < 2; ++i) {
        const float x0 = (float)(px + i * kCell);
        const RectArea r{ x0 * frac, (x0 + kCell) * frac, py * frac, (py + kCell) * frac };
        if (!sehFill(mcuirc, &r, i == 0 ? &black : &white)) {
            if (!g_loggedFill) {
                g_loggedFill = true;
                LOG_WARN("fillRectangle unavailable - real icons disabled (file textures used)", "GameIcon");
            }
            std::lock_guard<std::mutex> lk(g_mx);
            g_failed.insert(job.name);
            g_jobActive = false;
            return;
        }
        drawBlockIcon(mcuirc, job.block, x0 + kMargin, (float)(py + kMargin), (float)kIcon);
    }

    std::lock_guard<std::mutex> lk(g_mx);
    if (g_jobActive && g_job.name == job.name) {
        g_job.px = px;
        g_job.py = py;
        g_job.drawn = true;
    }
}

// Read a [x,y,w,h] pixel rect of the back buffer into tight RGBA8.
static bool readBack(ID3D11Resource* bb, int x, int y, int w, int h, std::vector<unsigned char>& out) {
    RenderManager& rm = RenderManager::GetInstance();
    ID3D11Device* dev = rm.GetDevice();
    ID3D11DeviceContext* ctx = rm.GetContext();
    if (!dev || !ctx || !bb) return false;

    ID3D11Texture2D* src = nullptr;
    if (FAILED(bb->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&src)) || !src) return false;
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    bool bgra = false;
    switch (sd.Format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: bgra = true; break;
    default: src->Release(); return false;   // HDR / 10-bit: not supported
    }
    if (x < 0 || y < 0 || x + w > (int)sd.Width || y + h > (int)sd.Height || sd.SampleDesc.Count != 1) {
        src->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = sd.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stage = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &stage)) || !stage) { src->Release(); return false; }

    D3D11_BOX box{ (UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1 };
    ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, src, 0, &box);
    src->Release();

    D3D11_MAPPED_SUBRESOURCE m{};
    bool ok = false;
    if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
        out.resize((size_t)w * h * 4);
        for (int row = 0; row < h; ++row) {
            const unsigned char* s = (const unsigned char*)m.pData + (size_t)row * m.RowPitch;
            unsigned char* d = out.data() + (size_t)row * w * 4;
            for (int i = 0; i < w; ++i) {
                d[i * 4 + 0] = bgra ? s[i * 4 + 2] : s[i * 4 + 0];
                d[i * 4 + 1] = s[i * 4 + 1];
                d[i * 4 + 2] = bgra ? s[i * 4 + 0] : s[i * 4 + 2];
                d[i * 4 + 3] = 255;
            }
        }
        ctx->Unmap(stage, 0);
        ok = true;
    }
    stage->Release();
    return ok;
}

void onPresent(ID3D11Resource* backBuffer, bool isDX12) {
    Job job;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_jobActive || !g_job.drawn) return;
        job = g_job;
        // Each capture attempt needs a fresh UI draw: when the HUD stops
        // rendering (menus) nothing is counted and nothing times out.
        g_job.drawn = false;
        if (++g_job.frames > kMaxFrames) {
            LOG_WARN("icon capture timed out for " + job.name, "GameIcon");
            g_failed.insert(job.name);
            g_jobActive = false;
            startNextJobLocked();
            return;
        }
    }
    if (!backBuffer) return;

    RenderManager& rm = RenderManager::GetInstance();
    std::vector<unsigned char> px;
    // Grab both cells in one copy.
    const int w = kCell * 2, h = kCell;
    (void)isDX12;   // caller already acquired the wrapped resource on DX12
    if (!readBack(backBuffer, job.px, job.py, w, h, px)) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_jobActive && g_job.name == job.name && g_job.frames > 3) {
            g_failed.insert(job.name);
            g_jobActive = false;
            startNextJobLocked();
        }
        return;
    }
    (void)rm;

    auto at = [&](int x, int y) -> const unsigned char* { return &px[((size_t)y * w + x) * 4]; };
    // Validate that this frame really shows our cells: the margins must be
    // pure black / pure white (DX12 may present an older frame first).
    auto nearC = [](const unsigned char* p, int v) {
        return abs(p[0] - v) < 10 && abs(p[1] - v) < 10 && abs(p[2] - v) < 10;
    };
    if (!nearC(at(1, 1), 0) || !nearC(at(kCell - 2, kCell - 2), 0) ||
        !nearC(at(kCell + 1, 1), 255) || !nearC(at(2 * kCell - 2, kCell - 2), 255))
        return;   // try again next present

    // Difference matte over the icon area.
    std::vector<unsigned char> rgba((size_t)kIcon * kIcon * 4);
    int opaque = 0;
    for (int y = 0; y < kIcon; ++y) {
        for (int x = 0; x < kIcon; ++x) {
            const unsigned char* b = at(kMargin + x, kMargin + y);
            const unsigned char* wv = at(kCell + kMargin + x, kMargin + y);
            const float d = ((wv[0] - b[0]) + (wv[1] - b[1]) + (wv[2] - b[2])) / (3.f * 255.f);
            float a = 1.f - d;
            if (a < 0.f) a = 0.f; if (a > 1.f) a = 1.f;
            unsigned char* o = &rgba[((size_t)y * kIcon + x) * 4];
            if (a < 1.f / 255.f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            for (int c = 0; c < 3; ++c) {
                float v = b[c] / a;
                o[c] = (unsigned char)(v > 255.f ? 255 : v);
            }
            o[3] = (unsigned char)(a * 255.f + 0.5f);
            if (o[3] > 200) ++opaque;
        }
    }

    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_jobActive || g_job.name != job.name) return;
    if (opaque < 16) {
        // Nothing drawn (block without an item form, e.g. some technical
        // blocks) — keep the file texture fallback for it.
        g_failed.insert(job.name);
    } else if (ID3D11ShaderResourceView* srv = SkinUtils::createTextureRGBA(rgba.data(), kIcon, kIcon)) {
        g_tex[job.name] = srv;
        LOG_INFO("captured real icon: " + job.name, "GameIcon");
    } else {
        g_failed.insert(job.name);
    }
    g_jobActive = false;
    startNextJobLocked();
}

void clear() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& kv : g_tex) if (kv.second) kv.second->Release();
    g_tex.clear();
    g_failed.clear();
    g_queue.clear();
    g_jobActive = false;
}

} // namespace GameIcon
} // namespace SDK
