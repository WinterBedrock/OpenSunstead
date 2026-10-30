#include "BlockTextures.h"
#include "GameIcon.h"

#include "../../Core/Logger.h"
#include "../../Utils/MiscUtils/SkinUtils.hpp"
#include "../../Render/RenderManager.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#include "stb/stb_image.h"
#include <nlohmann/json.hpp>

namespace SDK {
namespace {

// The DLL lives inside the game process, so the exe's own directory is the
// game install root.
std::string gameResourceDir() {
    char exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::filesystem::path p(exe);
    if (!p.has_parent_path()) return {};
    return (p.parent_path() / "data" / "resource_packs").string();
}

// brarchive reader: name -> (blob index, offset, size), file in memory.
struct Brarchive {
    // Every pack's blocks.brarchive merged: name -> (blob index, off, len).
    std::vector<std::vector<unsigned char>> blobs;
    std::unordered_map<std::string, std::array<size_t, 3>> files;
    bool ok = false;
};

// keyPrefix is the folder the archive represents: flat "blocks.brarchive" keys
// records bare ("stone.png"); a subfolder archive ("deepslate.brarchive") keys
// them relative ("deepslate/deepslate_diamond_ore.png").
bool loadBrarchive(const std::string& path, Brarchive& out, const std::string& keyPrefix) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0x10) { fclose(f); return false; }
    std::vector<unsigned char> data((size_t)sz);
    const size_t rd = fread(data.data(), 1, data.size(), f);
    fclose(f);
    if (rd != data.size()) return false;

    static const unsigned char kMagic[8] = { 0x7D, 0x27, 0x25, 0xB1, 0xA0, 0x52, 0x70, 0x26 };
    if (std::memcmp(data.data(), kMagic, 8) != 0) return false;

    const uint32_t count = *reinterpret_cast<const uint32_t*>(data.data() + 8);
    if (count == 0 || count > 65536) return false;

    const size_t blobIdx = out.blobs.size();
    out.blobs.push_back(std::move(data));
    const std::vector<unsigned char>& blob = out.blobs[blobIdx];

    // The 0x100-byte record table comes FIRST (0x10 + count*0x100); the blobs
    // follow it back to back. Verified against the vanilla blocks.brarchive:
    // the blob walk lands exactly on EOF and every blob starts with a real
    // image header.
    size_t pos = 0x10 + (size_t)count * 0x100;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t rec = 0x10 + (size_t)i * 0x100;
        if (rec + 0x100 > blob.size()) return false;
        const unsigned len = blob[rec];
        if (len == 0 || len > 250) return false;
        std::string name(reinterpret_cast<const char*>(&blob[rec + 1]), len);
        const uint32_t size = *reinterpret_cast<const uint32_t*>(&blob[rec + 0xFC]);
        if (size == 0 || pos + (size_t)size > blob.size()) return false;
        out.files[keyPrefix + name] = { blobIdx, pos, size };   // later packs override
        pos += size;
    }
    out.ok = true;
    return true;
}

// blocks.json is NOT strict JSON: it carries `// note` comments and trailing
// commas. Strip both before handing it to nlohmann.
std::string readTextFile(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return {};
    std::string out;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        out.resize((size_t)sz);
        const size_t rd = fread(out.data(), 1, out.size(), f);
        out.resize(rd);
    }
    fclose(f);
    return out;
}

std::string sanitizeJson(std::string s) {
    // Strip a UTF-8 BOM if present (nlohmann rejects it in strict mode).
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s = s.substr(3);
    // Remove // comments (this file family has no URLs, so a plain scan is fine).
    std::string cleaned;
    cleaned.reserve(s.size());
    bool inStr = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inStr) {
            cleaned.push_back(c);
            if (c == '\\' && i + 1 < s.size()) { cleaned.push_back(s[++i]); continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; cleaned.push_back(c); continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        cleaned.push_back(c);
    }
    // Trailing commas: ",}" / ",]" after whitespace.
    std::string out;
    out.reserve(cleaned.size());
    for (size_t i = 0; i < cleaned.size(); ++i) {
        out.push_back(cleaned[i]);
        if (cleaned[i] != ',') continue;
        size_t j = i + 1;
        while (j < cleaned.size() && std::isspace((unsigned char)cleaned[j])) ++j;
        if (j < cleaned.size() && (cleaned[j] == '}' || cleaned[j] == ']'))
            out.pop_back(); // drop the comma, the loop will re-emit the brace
    }
    return out;
}

// The texture database.
struct TextureDb {
    Brarchive archive;
    // block id ("stone") -> { top texName, side texName }
    std::unordered_map<std::string, std::pair<std::string, std::string>> blocks;
    // texture name -> "textures/blocks/xxx" path base ("xxx")
    std::unordered_map<std::string, std::string> terrainPaths;
    bool ok = false;
    bool tried = false;
};

TextureDb& db() {
    static TextureDb instance;
    return instance;
}

// blocks.json "textures" value: either a plain string or a per-face object.
void readFaceTextures(const nlohmann::json& tex, std::string& top, std::string& side) {
    if (tex.is_string()) {
        const std::string s = tex.get<std::string>();
        if (top.empty()) top = s;
        if (side.empty()) side = s;
        return;
    }
    if (!tex.is_object()) return;
    auto grab = [&](const char* key) {
        auto it = tex.find(key);
        if (it != tex.end() && it->is_string()) return it->get<std::string>();
        return std::string();
    };
    const std::string up    = grab("up");
    const std::string down  = grab("down");
    const std::string all   = grab("all");
    const std::string sd    = grab("side");
    const std::string north = grab("north");
    top = !up.empty() ? up : (!all.empty() ? all : (!down.empty() ? down : std::string()));
    side = !sd.empty() ? sd : (!north.empty() ? north : all);
}

// terrain_texture.json stores the path one level down. An entry is
//   { "textures": <string | array of variants | {"path": "..."}> }
// — arrays mix plain strings with {"path","overlay_color"} objects. Take the
// first thing that names a file.
std::string terrainPathOf(const nlohmann::json& entry) {
    const nlohmann::json* node = &entry;
    if (node->is_object()) {
        const auto wrapped = node->find("textures");
        if (wrapped != node->end()) node = &(*wrapped);
    }
    if (node->is_string()) return node->get<std::string>();
    if (node->is_array()) {
        for (const nlohmann::json& variant : *node) {
            if (variant.is_string()) return variant.get<std::string>();
            if (variant.is_object()) {
                const auto path = variant.find("path");
                if (path != variant.end() && path->is_string()) return path->get<std::string>();
            }
        }
        return {};
    }
    if (node->is_object()) {
        const auto path = node->find("path");
        if (path != node->end() && path->is_string()) return path->get<std::string>();
    }
    return {};
}

void loadDb(TextureDb& t) {
    t.tried = true;
    const std::string resDir = gameResourceDir();
    if (resDir.empty()) {
        LOG_WARN("BlockTextures: game resource dir not found", "BlockTextures");
        return;
    }

    // vanilla first, then vanilla_* overlays in version order and later packs
    // win (a block introduced by a version pack exists only there).
    std::vector<std::string> packs{ resDir + "\\vanilla" };
    std::vector<std::string> overlays;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(resDir, ec)) {
        const std::string n = entry.path().filename().string();
        if (n.rfind("vanilla_", 0) == 0)
            overlays.push_back(entry.path().string());
    }
    std::sort(overlays.begin(), overlays.end());
    packs.insert(packs.end(), overlays.begin(), overlays.end());

    // Every pack ships block textures under __brarchive\textures: the flat
    // blocks.brarchive plus one blocks/<folder>.brarchive per atlas subfolder.
    // Load all with the matching key prefix (see loadBrarchive).
    for (const std::string& pk : packs) {
        const std::string texDir = pk + "\\__brarchive\\textures";
        std::vector<std::pair<std::string, std::string>> archives;   // (path, key prefix)
        std::error_code ec2;
        if (!std::filesystem::exists(texDir, ec2)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(texDir, ec2);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (ec2) break;
            if (!it->is_regular_file(ec2)) continue;
            std::string rel = it->path().generic_string();
            const size_t anchor = rel.rfind("/textures/");
            if (anchor != std::string::npos) rel = rel.substr(anchor + 10);
            static const char kExt[] = ".brarchive";
            if (rel.size() <= sizeof(kExt) - 1 ||
                rel.compare(rel.size() - (sizeof(kExt) - 1), sizeof(kExt) - 1, kExt) != 0) continue;
            rel.resize(rel.size() - (sizeof(kExt) - 1));
            std::string prefix;
            if (rel == "blocks") prefix.clear();
            else if (rel.rfind("blocks/", 0) == 0) prefix = rel.substr(7) + "/";
            else continue;   // gui / items / entity / ... — not the block atlas
            archives.emplace_back(it->path().string(), prefix);
        }
        // Deterministic order; the flat archive ("" prefix) first.
        std::sort(archives.begin(), archives.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        for (const auto& a : archives)
            loadBrarchive(a.first, t.archive, a.second);
    }
    if (!t.archive.ok) {
        LOG_WARN("BlockTextures: no blocks.brarchive loaded (packs tried: " +
                 std::to_string(packs.size()) + ")", "BlockTextures");
        return;
    }

    for (const std::string& pk : packs) {
        // terrain_texture.json - texture name -> atlas path.
        {
            const std::string raw = readTextFile(pk + "\\textures\\terrain_texture.json");
            if (raw.empty()) continue;
            try {
                const auto j = nlohmann::json::parse(sanitizeJson(raw));
                const auto it = j.find("texture_data");
                if (it == j.end() || !it->is_object()) continue;
                for (auto el = it->begin(); el != it->end(); ++el) {
                    const std::string path = terrainPathOf(el.value());
                    if (path.empty()) continue;
                    // "textures/blocks/stone" -> "stone"; only the blocks atlas
                    // is in these archives, other folders are skipped.
                    static const char kPrefix[] = "textures/blocks";
                    const size_t prefixLen = sizeof(kPrefix) - 1;
                    if (path.size() <= prefixLen + 1 ||
                        path.compare(0, prefixLen, kPrefix) != 0) continue;
                    t.terrainPaths[el.key()] = path.substr(prefixLen + 1);
                }
            } catch (...) { continue; }
        }
        // blocks.json - block id -> face texture names.
        {
            const std::string raw = readTextFile(pk + "\\blocks.json");
            if (raw.empty()) continue;
            try {
                const auto j = nlohmann::json::parse(sanitizeJson(raw));
                if (!j.is_object()) continue;
                for (auto el = j.begin(); el != j.end(); ++el) {
                    const auto texIt = el.value().find("textures");
                    if (texIt == el.value().end()) continue;
                    std::string top, side;
                    readFaceTextures(*texIt, top, side);
                    if (top.empty() && side.empty()) continue;
                    t.blocks[el.key()] = { top, side };
                }
            } catch (...) { continue; }
        }
    }

    // ALIASES: the game reports many blocks under modern ids while blocks.json
    // is keyed by legacy ones. Registered after the merge so real entries win.
    auto alias = [&](const char* from, const char* to) {
        if (t.blocks.count(from)) return;
        auto it = t.blocks.find(to);
        if (it != t.blocks.end()) t.blocks[from] = it->second;
    };
    alias("grass_block", "grass");
    alias("grass_path", "grass_path");
    static const char* const kWoods[] = { "oak", "spruce", "birch", "jungle", "acacia",
                                          "dark_oak", "big_oak", "mangrove", "cherry",
                                          "bamboo", "crimson", "warped" };
    for (const char* w : kWoods) {
        const std::string wood(w);
        alias((wood + "_planks").c_str(), "planks");
        alias((wood + "_log").c_str(), "log");
        alias((wood + "_wood").c_str(), "log");
        alias(("stripped_" + wood + "_log").c_str(), "log");
        alias(("stripped_" + wood + "_wood").c_str(), "log");
    }

    t.ok = !t.blocks.empty() && !t.terrainPaths.empty();
    if (t.ok)
        LOG_INFO("block texture db: " + std::to_string(t.blocks.size()) + " blocks, " +
                 std::to_string(t.terrainPaths.size()) + " atlas entries, " +
                 std::to_string(t.archive.files.size()) + " archive files", "BlockTextures");
    else
        LOG_WARN("BlockTextures: db incomplete - blocks=" + std::to_string(t.blocks.size()) +
                 " atlas=" + std::to_string(t.terrainPaths.size()), "BlockTextures");
}

// GPU cache: (archive record) -> SRV. Only the render thread (OnRender/Present)
// calls the draw helpers that use it.
struct FaceTex {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
};

struct GpuCache {
    std::unordered_map<std::string, FaceTex> map;
    void* device = nullptr;   // drop everything if the D3D device is recreated
};

GpuCache& gpu() {
    static GpuCache c;
    return c;
}

FaceTex* loadFace(const std::string& texName) {
    TextureDb& t = db();
    auto pathIt = t.terrainPaths.find(texName);
    if (pathIt == t.terrainPaths.end()) return nullptr;

    ID3D11Device* dev = RenderManager::GetInstance().GetDevice();
    if (!dev) return nullptr;
    GpuCache& g = gpu();
    if (g.device != (void*)dev) {          // device recreated under us
        for (auto& kv : g.map) if (kv.second.srv) kv.second.srv->Release();
        g.map.clear();
        g.device = (void*)dev;
    }

    const std::string base = pathIt->second;
    auto it = g.map.find(base);
    if (it != g.map.end()) return it->second.srv ? &it->second : nullptr;

    FaceTex ft;
    // Candidate records: the exact base first ("deepslate/deepslate_diamond_ore"),
    // then the bare file name; suffixes "", "_side", "_top"; extensions .png and
    // .tga — the archive mixes both (grass_side is a .tga, stone a .png).
    std::array<std::string, 2> bases{ base, std::string() };
    const size_t sl = base.find_last_of('/');
    if (sl != std::string::npos && sl + 1 < base.size()) bases[1] = base.substr(sl + 1);
    for (const std::string& b : bases) {
        if (b.empty()) continue;
        for (const char* suffix : { "", "_side", "_top" }) {
            for (const char* ext : { ".png", ".tga" }) {
            const std::string rec = b + suffix + ext;
            auto f = t.archive.files.find(rec);
            if (f == t.archive.files.end()) continue;
            const auto& idx = f->second;                      // (blobIdx, off, len)
            if (idx[0] >= t.archive.blobs.size()) continue;
            const std::vector<unsigned char>& blob = t.archive.blobs[idx[0]];
            if (idx[1] + idx[2] > blob.size()) continue;

            int w = 0, h = 0, comp = 0;
            unsigned char* px = stbi_load_from_memory(blob.data() + idx[1],
                                                      (int)idx[2], &w, &h, &comp, 4);
            if (!px || w <= 0 || h <= 0) continue;
            ft.srv = SkinUtils::createTextureRGBA(px, w, h);
            stbi_image_free(px);
            if (ft.srv) { ft.w = w; ft.h = h; break; }
            }
            if (ft.srv) break;
        }
        if (ft.srv) break;
    }
    const std::string key = base;
    g.map[key] = ft;
    return g.map[key].srv ? &g.map[key] : nullptr;
}

std::string blockKey(const char* blockName) {
    if (!blockName || !*blockName) return {};
    std::string k(blockName);
    const size_t colon = k.find(':');
    if (colon != std::string::npos) k = k.substr(colon + 1);
    if (k.empty()) return {};
    // Namespace is stripped; blocks.json keys are plain ids ("stone").
    return k;
}

} // namespace

bool blockTexturesAvailable() {
    TextureDb& t = db();
    if (!t.tried) loadDb(t);
    return t.ok;
}

std::vector<std::string> blockTextureList() {
    TextureDb& t = db();
    if (!t.tried) loadDb(t);
    std::vector<std::string> out;
    if (!t.ok) return out;
    out.reserve(t.blocks.size());
    for (const auto& kv : t.blocks) out.push_back(kv.first);
    return out;
}

namespace {
// The authentic missing-texture indicator (magenta/black checker), drawn when a
// block resolves to nothing.
void drawMissingTexture(ImDrawList* dl, float x, float y, float s, float a) {
    const ImU32 magenta = ImGui::GetColorU32(ImVec4(0.87f, 0.10f, 0.42f, a));
    const ImU32 black   = ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.05f, a));
    const float half = s * 0.5f;
    for (int cy = 0; cy < 2; ++cy)
        for (int cx = 0; cx < 2; ++cx) {
            const ImVec2 p0(x + cx * half, y + cy * half);
            dl->AddRectFilled(p0, ImVec2(p0.x + half, p0.y + half),
                              ((cx + cy) & 1) ? black : magenta);
        }
}

void drawCube3D(ImDrawList* dl, float x, float y, float s,
                FaceTex* top, FaceTex* side, float r, float g, float b, float a) {
    const float cx = x + s * 0.50f;
    const float cy = y + s * 0.54f;
    const float hx = s * 0.42f;
    const float hy = s * 0.24f;
    const float hh = s * 0.42f;

    const ImVec2 topN(cx,          cy - hh);
    const ImVec2 topE(cx + hx,     cy - hh + hy);
    const ImVec2 topS(cx,          cy - hh + hy * 2.f);
    const ImVec2 topW(cx - hx,     cy - hh + hy);
    const ImVec2 botE(cx + hx,     cy + hy);
    const ImVec2 botS(cx,          cy + hy * 2.f);
    const ImVec2 botW(cx - hx,     cy + hy);

    const ImU32 cTop  = ImGui::GetColorU32(ImVec4(r, g, b, a));
    const ImU32 cR    = ImGui::GetColorU32(ImVec4(r * 0.80f, g * 0.80f, b * 0.80f, a));
    const ImU32 cL    = ImGui::GetColorU32(ImVec4(r * 0.58f, g * 0.58f, b * 0.58f, a));
    const ImVec2 uv0(0.f, 0.f), uv1(1.f, 0.f), uv2(1.f, 1.f), uv3(0.f, 1.f);

    // Top face: N -> E -> S -> W.
    if (top) dl->AddImageQuad((ImTextureID)top->srv, topN, topE, topS, topW, uv0, uv1, uv2, uv3, cTop);
    // Left face: W -> S -> botS -> botW.
    if (side) dl->AddImageQuad((ImTextureID)side->srv, topW, topS, botS, botW, uv0, uv1, uv2, uv3, cL);
    // Right face: S -> E -> botE -> botS.
    if (side) dl->AddImageQuad((ImTextureID)side->srv, topS, topE, botE, botS, uv0, uv1, uv2, uv3, cR);

    // Hairline silhouette so the cube reads on any background.
    const ImU32 edge = ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, a * 0.35f));
    dl->AddLine(topN, topE, edge, 1.0f);
    dl->AddLine(topE, topS, edge, 1.0f);
    dl->AddLine(topS, topW, edge, 1.0f);
    dl->AddLine(topW, topN, edge, 1.0f);
    dl->AddLine(topW, botW, edge, 1.0f);
    dl->AddLine(topS, botS, edge, 1.0f);
    dl->AddLine(topE, botE, edge, 1.0f);
    dl->AddLine(botW, botS, edge, 1.0f);
    dl->AddLine(botS, botE, edge, 1.0f);
}
} // namespace

void drawBlockTexture3D(ImDrawList* dl, float x, float y, float size, const char* blockName, float alpha) {
    if (!dl || size < 2.f || alpha <= 0.01f || !blockName || !*blockName) return;

    // Real captured icon (GameIcon) first; the file-built cube is the fallback.
    if (GameIcon::draw(dl, x, y, size, blockName, alpha)) return;

    TextureDb& t = db();
    if (!t.tried) loadDb(t);
    if (!t.ok) {
        drawMissingTexture(dl, x, y, size, alpha);
        return;
    }

    std::string key = blockKey(blockName);
    if (key.empty()) return;

    auto it = t.blocks.find(key);
    if (it == t.blocks.end()) {
        static int missLogged = 0;
        if (missLogged < 8) {
            ++missLogged;
            LOG_INFO("BlockTextures: no blocks.json entry for '" + key + "'", "BlockTextures");
        }
        drawMissingTexture(dl, x, y, size, alpha);
        return;
    }
    const std::string& topName = it->second.first;
    const std::string& sideName = it->second.second.empty() ? it->second.first : it->second.second;

    FaceTex* top = loadFace(topName);
    FaceTex* side = loadFace(sideName);
    if (!top && !side) {
        static int missLogged = 0;
        if (missLogged < 8) {
            ++missLogged;
            LOG_INFO("BlockTextures: texture miss for '" + key + "' (tex '" +
                     topName + "')", "BlockTextures");
        }
        drawMissingTexture(dl, x, y, size, alpha);
        return;
    }

    // White tint: the atlas textures already carry the colour (grass variants
    // are pre-tinted).
    drawCube3D(dl, x, y, size, top, side, 1.f, 1.f, 1.f, alpha);
}

bool drawBlockTextureFlat(ImDrawList* dl, float x, float y, float w, float h,
                          const char* blockName, float alpha) {
    if (!dl || w < 1.f || h < 1.f || alpha <= 0.01f || !blockName || !*blockName) return false;
    TextureDb& t = db();
    if (!t.tried) loadDb(t);
    if (!t.ok) return false;

    std::string key = blockKey(blockName);
    auto it = t.blocks.find(key);
    if (it == t.blocks.end()) return false;
    FaceTex* ft = loadFace(it->second.second.empty() ? it->second.first : it->second.second);
    if (!ft) return false;
    dl->AddImage((ImTextureID)ft->srv, ImVec2(x, y), ImVec2(x + w, y + h),
                 ImVec2(0, 0), ImVec2(1, 1),
                 ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, alpha)));
    return true;
}

} // namespace SDK
