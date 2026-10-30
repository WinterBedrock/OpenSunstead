#include "Chams.h"
#include "../../Hooks/GameHooks.h"
#include "../../Memory/SigManager.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Client/GameContext.h"
#include "../../Render/RenderManager.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../Setting.h"
#include "../Player/FreeCam.h"
#include "../../Core/Logger.h"
#include "../ModuleManager.h"
#include <MinHook.h>
#include <Windows.h>
#include <d3d12.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

typedef int (__fastcall* ActorRender_fn)(
    void* self_ard,
    void* baseActorRenderContext,
    SDK::Actor* actor,
    __int64 cameraTargetPos,
    __int64 pos,
    __int64 rotPtr,
    char    uiRendering);
static ActorRender_fn oActorRender = nullptr;

// dispatcher abi 
typedef unsigned long long* (__fastcall* ActorRenderB_fn)(void*, __int64, void*, float*);
typedef void                (__fastcall* ActorRenderC_fn)(void*, __int64, void*);
static ActorRenderB_fn oActorRenderB = nullptr;
static ActorRenderC_fn oActorRenderC = nullptr;
static uintptr_t s_dispatcherFnB = 0, s_dispatcherFnC = 0;

// test freecam part
static void*    s_ghostArd = nullptr;
static void*    s_ghostBarc = nullptr;
static __int64  s_ghostCam = 0;

typedef HRESULT (__stdcall* CreateGPS_fn)(
    ID3D12Device* self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    REFIID riid, void** ppPipelineState);
static CreateGPS_fn oCreateGPS = nullptr;
typedef void (__stdcall* SetPipelineState_fn)(
    ID3D12GraphicsCommandList* self, ID3D12PipelineState* pPipelineState);
static SetPipelineState_fn oSetPipelineState = nullptr;
typedef void (__stdcall* IASetVB_fn)(
    ID3D12GraphicsCommandList* self, UINT StartSlot, UINT NumBuffers,
    const D3D12_VERTEX_BUFFER_VIEW* pViews);
static IASetVB_fn oIASetVB = nullptr;
typedef void (__stdcall* DrawIndexed_fn)(
    ID3D12GraphicsCommandList* self, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);
static DrawIndexed_fn oDrawIndexed = nullptr;
typedef void (__stdcall* Draw_fn)(
    ID3D12GraphicsCommandList* self, UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation);
static Draw_fn oDraw = nullptr;
// OMSetBlendFactor is called
typedef void (__stdcall* OMSetBlendFactor_fn)(
    ID3D12GraphicsCommandList* self, const FLOAT BlendFactor[4]);

namespace {
constexpr UINT kEntityStride = 24;   // corroboration only, never a gate
constexpr unsigned long long kWantWindowMs = 250;
constexpr size_t kMaxLastPSO = 2048;
//records older than this are dead lists, evicted in bulk so the map can never starve a live lists in-flight draw
constexpr unsigned long long kLastPSOTtlMs = 2000;
enum class TwinKind { Solid = 0, Wire = 1, Tint = 2, Shadow = 3 };

struct PSOInfo {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::vector<uint8_t> vs, ps, ds, hs, gs;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    std::vector<std::string> inputSemantics;
    std::vector<std::string> semantics;
    bool hasSkin = false;
    bool valid = false;
};

std::recursive_mutex s_psoMutex;   // rtwin creation via hkCGPS
std::unordered_map<ID3D12PipelineState*, PSOInfo> s_psos;
struct TwinKey {
    ID3D12PipelineState* orig;
    TwinKind kind;
    bool operator==(const TwinKey& o) const { return orig == o.orig && kind == o.kind; }
};
struct TwinKeyHash {
    size_t operator()(const TwinKey& k) const {
        return std::hash<uintptr_t>()((uintptr_t)k.orig) * 31u + (size_t)k.kind;
    }
};
std::unordered_map<TwinKey, ID3D12PipelineState*, TwinKeyHash> s_twins;
std::unordered_set<ID3D12PipelineState*> s_twinSet;
std::unordered_map<ID3D12GraphicsCommandList*,
                   std::pair<ID3D12PipelineState*, unsigned long long>> s_lastPSO;
unsigned long long s_lastPSOStamp = 0;   // guarded by s_psoMutex
thread_local ID3D12GraphicsCommandList* t_lastSetList = nullptr;
std::unordered_map<ID3D12GraphicsCommandList*, UINT> s_stride;
std::unordered_map<UINT, unsigned> s_strideHist;
std::vector<std::string> s_semanticSets;   //debug
int s_twinsStyle = -1;
ID3D12Device* s_d3d12Device = nullptr;
bool s_creatingTwin = false;
bool s_cgpsHooked = false;
bool s_listHooksHooked = false;
std::atomic<bool> s_armListHooks{ false };
std::atomic<bool> s_chamsOn{ false };
std::atomic<unsigned long long> s_wantUntilMs{ 0 };
std::atomic<unsigned> s_scopes{ 0 }, s_draws{ 0 }, s_stride24{ 0 }, s_subs{ 0 },
    s_twinsMade{ 0 }, s_unknownPSO{ 0 }, s_scopeDraws{ 0 }, s_windowDraws{ 0 };
std::atomic<unsigned> s_clsPlayer{ 0 }, s_clsMob{ 0 }, s_clsItem{ 0 }, s_clsFail{ 0 };
std::atomic<unsigned> s_gateNoPSO{ 0 }, s_gateNotWant{ 0 },
    s_gateWindow{ 0 }, s_gateNoSkin{ 0 }, s_gateTwin{ 0 };
thread_local int  t_scopeDepth = 0;
thread_local bool t_scopeWant  = false;
unsigned long long s_lastDbgMs = 0;
bool s_firstSubLogged = false;
bool s_mhInit = false;
uintptr_t s_dispatcherFn = 0;

void chamsMHInit() {
    static std::once_flag once;
    std::call_once(once, []() { MH_Initialize(); });
    s_mhInit = true;
}

Chams* chamsModule() {
    return static_cast<Chams*>(ModuleManager::GetInstance().GetModuleByName("Chams"));
}

bool semanticIsSkin(const char* s) {
    if (!s) return false;
    // BLENDINDICES, BLENDWEIGHT, BONE* bone data only skinned i think
    return std::strstr(s, "BLEND") != nullptr || std::strstr(s, "BONE") != nullptr;
}

bool sehReadSemantic(const D3D12_INPUT_ELEMENT_DESC* elems, UINT n, UINT i, char out[64]) {
    __try {
        if (!elems || i >= n || !out) return false;
        const char* sem = elems[i].SemanticName;
        if (!sem) return false;
        std::strncpy(out, sem, 63);
        out[63] = 0;
        return out[0] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehCopyBytes(void* dst, const void* src, size_t size) {
    __try {
        if (!dst || !src || size == 0) return false;
        std::memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

typedef const void* (__fastcall* ActorGetIdent_fn)(void* actor);

enum class ActorClass { Unknown = 0, Player = 1, Item = 2, Mob = 3 };

const char* actorClassName(ActorClass c) {
    switch (c) {
        case ActorClass::Player: return "player";
        case ActorClass::Item:   return "item";
        case ActorClass::Mob:    return "mob";
        default:                 return "unknown";
    }
}

ActorClass classifyActorIdent(const char* id) {
    if (!id || !id[0]) return ActorClass::Unknown;
    if (std::strncmp(id, "minecraft:player", 16) == 0) return ActorClass::Player;
    if (std::strncmp(id, "minecraft:item", 14) == 0)   return ActorClass::Item;
    return ActorClass::Mob;
}

int sehReadActorIdent(SDK::Actor* actor, char* buf, unsigned bufCap) {
    __try {
        if (!actor || !buf || bufCap < 20) return -1;
        void** vtbl = *(void***)actor;
        if (!vtbl) return -1;
        void* fn = vtbl[69];
        if (!fn) return -1;
        const void* hs = ((ActorGetIdent_fn)fn)(actor);
        if (!hs) return -1;
        // HashedString = { hash @0, std::string @8 } (str data ptr/SSO buf @8, size @0x18, capacity @0x20 - same fields the dispatcher itself reads
        const char*  sso  = (const char*)hs + 8;
        const size_t size = *(const size_t*)((const char*)hs + 0x18);
        const size_t cap  = *(const size_t*)((const char*)hs + 0x20);
        if (size == 0 || size >= bufCap) return -1;
        const char* p = (cap >= 16) ? *(const char* const*)sso : sso;
        if (!p) return -1;
        unsigned i = 0;
        for (; i < size; ++i) {
            const unsigned char ch = (unsigned char)p[i];
            if (ch < 0x20 || ch == 0x7F) break;
            buf[i] = (char)ch;
        }
        if (i != size || i == 0) return -1;   // non-printable garbage
        buf[i] = 0;
        return 69;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

void logNewActorIdent(const char* id, int slot, ActorClass cls) {
    static std::mutex m;
    static std::unordered_set<std::string> seen;
    constexpr size_t kMaxSeen = 16;
    if (!id || !id[0]) return;
    std::lock_guard<std::mutex> lock(m);
    if (seen.size() >= kMaxSeen || !seen.insert(std::string(id)).second) return;
    LOG_INFO(std::string("Chams actor id '") + id + "' (vtable slot " +
             std::to_string(slot) + ") -> " + actorClassName(cls), "Chams");
}

bool copyShaderBytecode(D3D12_SHADER_BYTECODE& bytecode, std::vector<uint8_t>& copy) {
    if (bytecode.BytecodeLength == 0) {
        bytecode.pShaderBytecode = nullptr;
        return true;
    }
    if (!bytecode.pShaderBytecode || bytecode.BytecodeLength > 32u * 1024u * 1024u)
        return false;
    copy.resize(bytecode.BytecodeLength);
    if (!sehCopyBytes(copy.data(), bytecode.pShaderBytecode, copy.size())) return false;
    bytecode.pShaderBytecode = copy.data();
    return true;
}

bool snapshotPSODesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& source, PSOInfo& info) {
    info.desc = source;
    if (source.StreamOutput.NumEntries || source.StreamOutput.NumStrides) return false;
    if (!copyShaderBytecode(info.desc.VS, info.vs) ||
        !copyShaderBytecode(info.desc.PS, info.ps) ||
        !copyShaderBytecode(info.desc.DS, info.ds) ||
        !copyShaderBytecode(info.desc.HS, info.hs) ||
        !copyShaderBytecode(info.desc.GS, info.gs)) return false;

    const UINT count = source.InputLayout.NumElements;
    if (count > 64 || (count && !source.InputLayout.pInputElementDescs)) return false;
    if (count == 0) {
        info.desc.InputLayout.pInputElementDescs = nullptr;
        return true;
    }

    info.inputElements.resize(count);
    if (!sehCopyBytes(info.inputElements.data(), source.InputLayout.pInputElementDescs,
                      sizeof(D3D12_INPUT_ELEMENT_DESC) * count)) return false;
    info.inputSemantics.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        char semantic[64] = {};
        if (!sehReadSemantic(source.InputLayout.pInputElementDescs, count, i, semantic)) return false;
        info.inputSemantics.emplace_back(semantic);
    }
    for (UINT i = 0; i < count; ++i)
        info.inputElements[i].SemanticName = info.inputSemantics[i].c_str();
    info.desc.InputLayout.pInputElementDescs = info.inputElements.data();
    return true;
}

void recordSemantics(PSOInfo& info) {
    info.hasSkin = false;
    const UINT count = info.desc.InputLayout.NumElements;
    const D3D12_INPUT_ELEMENT_DESC* elems = info.desc.InputLayout.pInputElementDescs;
    if (!elems || count == 0 || count > 64) return;
    std::string joined;
    for (UINT i = 0; i < count; ++i) {
        char buf[64] = {};
        if (!sehReadSemantic(elems, count, i, buf)) continue;
        info.semantics.emplace_back(buf);
        if (semanticIsSkin(buf)) info.hasSkin = true;
        if (!joined.empty()) joined += ",";
        joined += buf;
    }
    if (!joined.empty() && s_semanticSets.size() < 12) {
        bool known = false;
        for (const auto& e : s_semanticSets)
            if (e == joined) { known = true; break; }
        if (!known) s_semanticSets.push_back(joined);
    }
}

bool sehValidatePSODesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    __try {
        if (!d.pRootSignature) return false;
        if (d.VS.BytecodeLength && !d.VS.pShaderBytecode) return false;
        if (d.PS.BytecodeLength && !d.PS.pShaderBytecode) return false;
        if (d.InputLayout.NumElements > 64) return false;
        if (d.InputLayout.NumElements && !d.InputLayout.pInputElementDescs) return false;
        if (d.NumRenderTargets > 8) return false;
        if (d.SampleDesc.Count < 1 || d.SampleDesc.Count > 64) return false;
        volatile unsigned char touch = 0;
        if (d.VS.BytecodeLength)
            touch ^= ((const unsigned char*)d.VS.pShaderBytecode)[0];
        if (d.PS.BytecodeLength)
            touch ^= ((const unsigned char*)d.PS.pShaderBytecode)[0];
        for (UINT i = 0; i < d.InputLayout.NumElements; ++i) {
            const char* sem = d.InputLayout.pInputElementDescs[i].SemanticName;
            if (!sem) return false;
            touch ^= (unsigned char)sem[0];
        }
        (void)touch;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehCreateTwin(ID3D12Device* dev,
                   const D3D12_GRAPHICS_PIPELINE_STATE_DESC* twin,
                   ID3D12PipelineState** out) {
    __try {
        if (!dev || !twin || !out) return false;
        *out = nullptr;
        const HRESULT hr = dev->CreateGraphicsPipelineState(
            twin, __uuidof(ID3D12PipelineState), (void**)out);
        return SUCCEEDED(hr) && *out != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (out) *out = nullptr;
        return false;
    }
}

void logTwinFailure(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& src, const char* why) {
    static std::atomic<unsigned> n{ 0 };
    if (n.fetch_add(1, std::memory_order_relaxed) >= 3) return;
    LOG_WARN(std::string("Chams twin rejected (") + why +
             "): rt=" + std::to_string(src.NumRenderTargets) +
             " rtv0=" + std::to_string((int)src.RTVFormats[0]) +
             " dsv=" + std::to_string((int)src.DSVFormat) +
             " sample=" + std::to_string(src.SampleDesc.Count) +
             " depth=" + std::to_string((int)src.DepthStencilState.DepthEnable) +
             " elems=" + std::to_string(src.InputLayout.NumElements), "Chams");
}

ID3D12PipelineState* findOrCreateTwin(ID3D12PipelineState* orig, TwinKind kind, int style) {
    if (!orig || !s_d3d12Device) return nullptr;
    if (s_twinSet.count(orig)) return orig;
    if (style != s_twinsStyle) {
        for (auto& kv : s_twins)
            if (kv.second) kv.second->Release();
        s_twins.clear();
        s_twinSet.clear();
        s_twinsStyle = style;
    }
    auto dit = s_psos.find(orig);
    if (dit == s_psos.end() || !dit->second.valid) {
        ++s_unknownPSO;
        static std::atomic<bool> logged{ false };
        bool expected = false;
        if (logged.compare_exchange_strong(expected, true))
            LOG_WARN("Chams: unknown PSO drawn by a wanted actor (created before the "
                     "CGPS hook?) - chams unavailable for that pipeline variant", "Chams");
        return nullptr;
    }
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& src = dit->second.desc;
    const TwinKey key{ orig, kind };
    auto it = s_twins.find(key);
    if (it != s_twins.end()) return it->second;
    if (src.NumRenderTargets == 0) { s_twins[key] = orig; return orig; }
    if (kind == TwinKind::Solid && !src.DepthStencilState.DepthEnable) {
        s_twins[key] = orig;
        return orig;
    }
    if (!sehValidatePSODesc(src)) {
        logTwinFailure(src, "bad desc");
        s_twins[key] = nullptr;
        return nullptr;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC twin = src;
    twin.DepthStencilState.DepthEnable = FALSE;
    twin.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    twin.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    twin.CachedPSO.pCachedBlob = nullptr;
    twin.CachedPSO.CachedBlobSizeInBytes = 0;
    if (kind == TwinKind::Wire)
        twin.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    if (kind == TwinKind::Tint) {
        // out = src*1 + dst*factor.
        twin.BlendState.AlphaToCoverageEnable = FALSE;
        twin.BlendState.IndependentBlendEnable = FALSE;
        D3D12_RENDER_TARGET_BLEND_DESC& rt = twin.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_BLEND_FACTOR;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.LogicOpEnable = FALSE;
    }
    ID3D12PipelineState* out = nullptr;
    s_creatingTwin = true;
    const bool ok = sehCreateTwin(s_d3d12Device, &twin, &out);
    s_creatingTwin = false;
    if (!ok) {
        logTwinFailure(src, "driver rejected/faulted");
        s_twins[key] = nullptr;
        return nullptr;
    }
    s_twins[key] = out;
    s_twinSet.insert(out);
    ++s_twinsMade;
    return out;
}

ImColor wallColorFor(ID3D12PipelineState* pso) {
    const uint64_t nowMs = ColorUtils::GetTimeMs();
    const float idx = (float)((uintptr_t)pso & 0xFFFu);
    return ColorUtils::getThemedColor(idx, nowMs);
}

void chamsDebugTick() {
    Chams* mod = chamsModule();
    if (!mod || !mod->IsEnabled()) return;
    if (!Chams::ShowDebug()) return;
    const unsigned long long now = GetTickCount64();
    if (now - s_lastDbgMs < 5000) return;
    s_lastDbgMs = now;
    std::string hist, sems;
    {
        std::lock_guard<std::recursive_mutex> lock(s_psoMutex);
        int shown = 0;
        for (const auto& kv : s_strideHist) {
            if (shown++ >= 10) break;
            hist += " " + std::to_string(kv.first) + ":" + std::to_string(kv.second);
        }
        for (size_t i = 0; i < s_semanticSets.size() && i < 10; ++i)
            sems += " [" + s_semanticSets[i] + "]";
    }
    LOG_INFO("Chams dx12: scopes=" + std::to_string(s_scopes.load()) +
             " draws=" + std::to_string(s_draws.load()) +
             " scoped=" + std::to_string(s_scopeDraws.load()) +
             " window=" + std::to_string(s_windowDraws.load()) +
             " stride24=" + std::to_string(s_stride24.load()) +
             " subs=" + std::to_string(s_subs.load()) +
             " twins=" + std::to_string(s_twinsMade.load()) +
             " unknownPSO=" + std::to_string(s_unknownPSO.load()) +
             " cls(p/m/i/fail)=" + std::to_string(s_clsPlayer.load()) + "/" +
             std::to_string(s_clsMob.load()) + "/" + std::to_string(s_clsItem.load()) + "/" +
             std::to_string(s_clsFail.load()) +
             " gate(nps/nw/win/nsk/tw)=" + std::to_string(s_gateNoPSO.load()) + "/" +
             std::to_string(s_gateNotWant.load()) + "/" + std::to_string(s_gateWindow.load()) + "/" +
             std::to_string(s_gateNoSkin.load()) + "/" + std::to_string(s_gateTwin.load()) +
             " strides={" + hist + " } sems={" + sems + " }", "Chams");
}
} // namespace

// d3d12 hooks
static HRESULT __stdcall hkCreateGraphicsPipelineState(
    ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    REFIID riid, void** ppPipelineState) {
    HRESULT hr = oCreateGPS
        ? oCreateGPS(self, pDesc, riid, ppPipelineState)
        : E_FAIL;
    if (SUCCEEDED(hr) && ppPipelineState && *ppPipelineState && pDesc && !s_creatingTwin) {
        std::lock_guard<std::recursive_mutex> lock(s_psoMutex);
        if (!s_d3d12Device) s_d3d12Device = self;
        PSOInfo info;
        info.valid = snapshotPSODesc(*pDesc, info);
        if (info.valid) recordSemantics(info);
        s_psos[(ID3D12PipelineState*)*ppPipelineState] = std::move(info);
    }
    return hr;
}

static void __stdcall hkSetPipelineState(
    ID3D12GraphicsCommandList* self, ID3D12PipelineState* pPipelineState) {
    if (s_chamsOn.load(std::memory_order_relaxed) && self && pPipelineState) {
        std::lock_guard<std::recursive_mutex> lock(s_psoMutex);
        if (!s_twinSet.count(pPipelineState)) {
            s_lastPSO[self] = { pPipelineState, ++s_lastPSOStamp };
            t_lastSetList = self;
            if (s_lastPSO.size() > kMaxLastPSO) {
                auto oldest = s_lastPSO.end();
                for (auto it = s_lastPSO.begin(); it != s_lastPSO.end(); ++it)
                    if (it->first != self &&
                        (oldest == s_lastPSO.end() ||
                         it->second.second < oldest->second.second))
                        oldest = it;
                if (oldest != s_lastPSO.end()) s_lastPSO.erase(oldest);
            }
        }
    }
    if (oSetPipelineState) oSetPipelineState(self, pPipelineState);
}

UINT sehReadStride(const D3D12_VERTEX_BUFFER_VIEW* pViews) {
    __try { return pViews[0].StrideInBytes; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void __stdcall hkIASetVertexBuffers(
    ID3D12GraphicsCommandList* self, UINT StartSlot, UINT NumBuffers,
    const D3D12_VERTEX_BUFFER_VIEW* pViews) {
    if (s_chamsOn.load(std::memory_order_relaxed) && self && StartSlot == 0 &&
        NumBuffers >= 1 && pViews) {
        const UINT stride = sehReadStride(pViews);   // game-owned array
        if (stride != 0 && stride < 4096) {
            std::lock_guard<std::recursive_mutex> lock(s_psoMutex);
            s_stride[self] = stride;
            if (s_stride.size() > 128) s_stride.clear();   // transient lists
            ++s_strideHist[stride];
        }
    }
    if (oIASetVB) oIASetVB(self, StartSlot, NumBuffers, pViews);
}

static void omSetBlendFactor(ID3D12GraphicsCommandList* self, float r, float g, float b, float a) {
    if (!self) return;
    __try {
        void** vtbl = *(void***)self;
        if (!vtbl) return;
        auto fn = (OMSetBlendFactor_fn)vtbl[23];
        if (!fn) return;
        const FLOAT f[4] = { r, g, b, a };
        fn(self, f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// one entity draw with depth-off twin(s) around it
template <typename DrawCall>
static void chamsDraw(ID3D12GraphicsCommandList* self, DrawCall drawOriginal) {
    ++s_draws;
    ID3D12PipelineState* twin = nullptr;   // primary substituted PSO (or null)
    ID3D12PipelineState* orig = nullptr;
    bool twoPass = false;
    FLOAT wall[4] = { 1, 1, 1, 1 };
    {
        std::lock_guard<std::recursive_mutex> lock(s_psoMutex);
        do {
            auto sit = s_stride.find(self);
            if (sit != s_stride.end() && sit->second == kEntityStride) ++s_stride24;
            auto pit = s_lastPSO.find(self);
            ID3D12PipelineState* cur =
                (pit == s_lastPSO.end()) ? nullptr : pit->second.first;
            if (!cur) {
                if (t_lastSetList == self) ++s_gateNoPSO;
                break;
            }
            if (s_twinSet.count(cur)) break;
            const bool inScope = (t_scopeDepth > 0);
            if (inScope && !t_scopeWant) { ++s_gateNotWant; break; }
            if (!inScope) {
                if (GetTickCount64() >= s_wantUntilMs.load(std::memory_order_relaxed)) { ++s_gateWindow; break; }
                auto dit = s_psos.find(cur);
                if (dit == s_psos.end() || !dit->second.valid || !dit->second.hasSkin) { ++s_gateNoSkin; break; }
                ++s_windowDraws;
            } else {
                ++s_scopeDraws;
            }
            const int style = Chams::CurrentStyle();
            if (style == 2) {
                // test themed wall pass + normal visible pass
                const TwinKind kind = TwinKind::Tint;
                ID3D12PipelineState* t = findOrCreateTwin(cur, kind, style);
                if (!t || t == cur) { ++s_gateTwin; break; }
                orig = cur;
                twin = t;
                twoPass = true;
                const ImColor wc = wallColorFor(cur);
                wall[0] = wc.Value.x; wall[1] = wc.Value.y;
                wall[2] = wc.Value.z; wall[3] = 1.0f;
            } else {
                const TwinKind kind = (style == 1) ? TwinKind::Wire : TwinKind::Solid;
                ID3D12PipelineState* t = findOrCreateTwin(cur, kind, style);
                if (!t || t == cur) break;
                orig = cur;
                twin = t;
            }
            if (oSetPipelineState) oSetPipelineState(self, twin);
            ++s_subs;
            if (!s_firstSubLogged) {
                s_firstSubLogged = true;
                LOG_INFO("Chams depth-off substitution active", "Chams");
            }
        } while (0);
    }
    if (!twin || !orig) { drawOriginal(); return; }
    if (twoPass) {
        omSetBlendFactor(self, wall[0], wall[1], wall[2], wall[3]);
        drawOriginal();
        omSetBlendFactor(self, 1.0f, 1.0f, 1.0f, 1.0f);
        if (oSetPipelineState) oSetPipelineState(self, orig);
        drawOriginal();
        ++s_subs;   // second pass drawn too
    } else {
        drawOriginal();
        if (oSetPipelineState) {
            std::lock_guard<std::recursive_mutex> lock(s_psoMutex);
            oSetPipelineState(self, orig);
        }
    }
    chamsDebugTick();
}

static void __stdcall hkDrawIndexedInstanced(
    ID3D12GraphicsCommandList* self, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) {
    if (!s_chamsOn.load(std::memory_order_relaxed) || !oDrawIndexed) {
        if (oDrawIndexed) oDrawIndexed(self, IndexCountPerInstance, InstanceCount,
                                       StartIndexLocation, BaseVertexLocation,
                                       StartInstanceLocation);
        return;
    }
    chamsDraw(self, [&]() {
        oDrawIndexed(self, IndexCountPerInstance, InstanceCount,
                     StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    });
}

static void __stdcall hkDrawInstanced(
    ID3D12GraphicsCommandList* self, UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation) {
    if (!s_chamsOn.load(std::memory_order_relaxed) || !oDraw) {
        if (oDraw) oDraw(self, VertexCountPerInstance, InstanceCount,
                         StartVertexLocation, StartInstanceLocation);
        return;
    }
    chamsDraw(self, [&]() {
        oDraw(self, VertexCountPerInstance, InstanceCount,
              StartVertexLocation, StartInstanceLocation);
    });
}

// dx11 depth
static ID3D11DepthStencilState* g_noDepthState = nullptr;
static ID3D11DepthStencilState* g_restoreState = nullptr;

static bool chamsEnsure(ID3D11Device* device) {
    if (g_noDepthState) return true;
    if (!device) return false;
    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable    = FALSE;
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc      = D3D11_COMPARISON_ALWAYS;
    desc.StencilEnable  = FALSE;
    if (FAILED(device->CreateDepthStencilState(&desc, &g_noDepthState))) {
        g_noDepthState = nullptr;
        return false;
    }
    return true;
}

static void swapDepth(ID3D11DeviceContext* ctx, bool enable) {
    if (!ctx) return;
    if (enable) {
        ID3D11DepthStencilState* prev = nullptr;
        UINT stencilRef = 0;
        ctx->OMGetDepthStencilState(&prev, &stencilRef);
        if (g_restoreState) g_restoreState->Release();
        g_restoreState = prev;
        (void)stencilRef;
        if (g_noDepthState) ctx->OMSetDepthStencilState(g_noDepthState, 0);
    } else if (g_restoreState) {
        ctx->OMSetDepthStencilState(g_restoreState, 0);
        g_restoreState->Release();
        g_restoreState = nullptr;
    }
}

// test part for freecam
static bool sehActorGetRot(SDK::Actor* lp, Vec2* out) {
    __try {
        if (lp && out) *out = lp->getRot();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool sehGhostRender(void* self_ard, void* barc, SDK::Actor* lp,
                           __int64 cameraPos, const Vec3* origin, const Vec2* rot) {
    StateVectorComponent* sv = nullptr;
    Vec3 savedPos{}, savedOld{};
    bool patched = false;
    __try {
        if (!lp || !origin || !rot) return false;
        sv = lp->getStateVector();
        if (sv) {
            savedPos = sv->pos;
            savedOld = sv->posOld;
            sv->pos = *origin;
            sv->posOld = *origin;
            patched = true;
        }
        Vec3 entryPos = *origin;
        oActorRender(self_ard, barc, lp, cameraPos,
                     reinterpret_cast<__int64>(&entryPos),
                     reinterpret_cast<__int64>(const_cast<Vec2*>(rot)), 0);
        if (patched) {
            sv->pos = savedPos;
            sv->posOld = savedOld;
            patched = false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (patched && sv) {
            sv->pos = savedPos;
            sv->posOld = savedOld;
        }
        return false;
    }
}

static bool sehInjectFreeCamGhost(void* self_ard, void* barc, __int64 cameraPos, void* skipActor) {
    // one heartbeat per sec before every gate
    const bool drawn = GameHooks::g_freeCamGhostDrawn.load(std::memory_order_relaxed);
    const int persp = GameHooks::LivePerspectiveForGhost();
    const bool hud = SDK::isHudScreen();
    Vec3 ghost{};
    const bool want = FreeCam::GhostOrigin(ghost);
    SDK::Actor* lp = SDK::ClientInstance::get() ? SDK::ClientInstance::get()->getLocalPlayer() : nullptr;
    {
        static unsigned s_fcDiag = 0;
        if (++s_fcDiag >= 3000) {
            s_fcDiag = 0;
            FreeCam* fc = static_cast<FreeCam*>(ModuleManager::GetInstance().GetModuleByName("FreeCam"));
            const bool fcEn = fc && fc->IsEnabled();
            const bool fcActive = fc && fc->IsEnabled() && fc->IsActiveForDiag();
            const bool fcShowModel = fc && fc->IsEnabled() && fc->GetShowModelForDiag();
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "fc diag: persp=%d hud=%d wanted=%d [en=%d active=%d lagbacked=%d showModel=%d] lp=%s ard=%s barc=%s",
                persp, hud ? 1 : 0, want ? 1 : 0,
                fcEn ? 1 : 0, fcActive ? 1 : 0,
                (fc && fc->IsLagbackedForDiag()) ? 1 : 0,
                fcShowModel ? 1 : 0,
                lp ? "ok" : "NULL",
                self_ard ? "ok" : "NULL",
                barc ? "ok" : "NULL");
            LOG_INFO(buf, "Chams");
        }
    }
    if (!oActorRender || !self_ard || !barc) return false;
    if (drawn) return false;
    if (persp != 0) return false;
    if (!hud) return false;
    if (!want) return false;
    if (!lp || static_cast<void*>(lp) == skipActor) return false;
    Vec2 lpRot{};
    sehActorGetRot(lp, &lpRot);
    GameHooks::g_freeCamGhostDrawn.store(true, std::memory_order_relaxed);
    static unsigned long long s_ghostInjections = 0;
    if (++s_ghostInjections == 1 || (s_ghostInjections % 600) == 0)
        LOG_INFO("FreeCam ghost body: player model injected (call " +
                 std::to_string(s_ghostInjections) + ")", "Chams");
    return sehGhostRender(self_ard, barc, lp, cameraPos, &ghost, &lpRot);
}

void Chams::InjectFreeCamGhost(void* self_ard, void* barc, __int64 cameraPos, void* skipActor) {
    sehInjectFreeCamGhost(self_ard, barc, cameraPos, skipActor);
}

void Chams::InjectFreeCamGhostCached() {
    if (!s_ghostArd || !s_ghostBarc) return;
    sehInjectFreeCamGhost(s_ghostArd, s_ghostBarc, s_ghostCam, nullptr);
}

static bool chamsClassifyActor(SDK::Actor* actor) {
    Chams* mod = chamsModule();
    const bool enabled = mod && mod->IsEnabled();
    s_chamsOn.store(enabled, std::memory_order_relaxed);
    if (!enabled || !actor) return false;
    char ident[40] = {};
    if (sehReadActorIdent(actor, ident, sizeof(ident)) >= 0) {
        const ActorClass cls = classifyActorIdent(ident);
        switch (cls) {
            case ActorClass::Player: ++s_clsPlayer; break;
            case ActorClass::Item:   ++s_clsItem;   break;
            case ActorClass::Mob:    ++s_clsMob;    break;
            default:                 ++s_clsFail;   break;
        }
        logNewActorIdent(ident, 69, cls);
    } else {
        ++s_clsFail;
    }
    return true;   // every actor, unconditional
}

// dx12 window scope
template <typename Call>
static auto chamsDx12Bracket(Call call) {
    Chams::EnsureDeviceHooks();
    ++s_scopes;
    s_wantUntilMs.store(GetTickCount64() + kWantWindowMs, std::memory_order_relaxed);
    ++t_scopeDepth;
    const bool prevWant = t_scopeWant;
    t_scopeWant = true;
    auto r = call();
    t_scopeWant = prevWant;
    --t_scopeDepth;
    return r;
}

static int hkActorRenderDispatcher_render(
    void* self_ard, void* barc, SDK::Actor* actor,
    __int64 cameraPos, __int64 pos, __int64 rotPtr, char ui) {
    if (!oActorRender) return 0;
    s_chamsOn.store(chamsModule() && chamsModule()->IsEnabled(), std::memory_order_relaxed);
    s_ghostArd = self_ard;
    s_ghostBarc = barc;
    s_ghostCam = cameraPos;
    {
        Vec3 ghost{};
        if (SDK::isHudScreen() && FreeCam::GhostPosFor(actor, ghost)) {
            return oActorRender(self_ard, barc, actor, cameraPos,
                                reinterpret_cast<__int64>(&ghost), rotPtr, ui);
        }
    }
    sehInjectFreeCamGhost(self_ard, barc, cameraPos, actor);

    if (!chamsClassifyActor(actor))
        return oActorRender(self_ard, barc, actor, cameraPos, pos, rotPtr, ui);

    RenderManager& rm = RenderManager::GetInstance();
    if (rm.IsInitialized() && rm.IsDX12())
        return chamsDx12Bracket([&] {
            return oActorRender(self_ard, barc, actor, cameraPos, pos, rotPtr, ui);
        });
    ID3D11DeviceContext* ctx = rm.GetContext();
    bool swapped = false;
    if (rm.IsInitialized() && rm.GetDevice() && ctx && chamsEnsure(rm.GetDevice())) {
        swapDepth(ctx, true);
        swapped = true;
    }
    const int r = oActorRender(self_ard, barc, actor, cameraPos, pos, rotPtr, ui);
    if (swapped) swapDepth(ctx, false);
    return r;
}

static unsigned long long* __fastcall hkActorRenderSiblingB(
    void* self_ard, __int64 a2, void* actor, float* camOffsets) {
    if (!oActorRenderB) return nullptr;
    if (!chamsClassifyActor((SDK::Actor*)actor))
        return oActorRenderB(self_ard, a2, actor, camOffsets);
    RenderManager& rm = RenderManager::GetInstance();
    if (rm.IsInitialized() && rm.IsDX12())
        return chamsDx12Bracket([&] {
            return oActorRenderB(self_ard, a2, actor, camOffsets);
        });
    ID3D11DeviceContext* ctx = rm.GetContext();
    bool swapped = false;
    if (rm.IsInitialized() && rm.GetDevice() && ctx && chamsEnsure(rm.GetDevice())) {
        swapDepth(ctx, true);
        swapped = true;
    }
    unsigned long long* r = oActorRenderB(self_ard, a2, actor, camOffsets);
    if (swapped) swapDepth(ctx, false);
    return r;
}

static void __fastcall hkActorRenderSiblingC(
    void* self_ard, __int64 a2, void* actor) {
    if (!oActorRenderC) return;
    if (!chamsClassifyActor((SDK::Actor*)actor)) {
        oActorRenderC(self_ard, a2, actor);
        return;
    }
    RenderManager& rm = RenderManager::GetInstance();
    if (rm.IsInitialized() && rm.IsDX12()) {
        chamsDx12Bracket([&] {
            oActorRenderC(self_ard, a2, actor);
            return 0;
        });
        return;
    }
    ID3D11DeviceContext* ctx = rm.GetContext();
    bool swapped = false;
    if (rm.IsInitialized() && rm.GetDevice() && ctx && chamsEnsure(rm.GetDevice())) {
        swapDepth(ctx, true);
        swapped = true;
    }
    oActorRenderC(self_ard, a2, actor);
    if (swapped) swapDepth(ctx, false);
}

int Chams::CurrentStyle() {
    Chams* mod = chamsModule();
    if (!mod || mod->mSettings.empty()) return 0;
    return static_cast<EnumSetting*>(mod->mSettings[0])->mValue;
}

bool Chams::ShowDebug() {
    Chams* mod = chamsModule();
    if (!mod || mod->mSettings.size() <= 1) return false;
    return static_cast<BoolSetting*>(mod->mSettings[1])->mValue;
}

void Chams::EnsureDeviceHooks() {
    std::lock_guard<std::recursive_mutex> lock(s_psoMutex);
    RenderManager& rm = RenderManager::GetInstance();
    if (!rm.IsInitialized() || !rm.IsDX12()) return;
    ID3D12Device* dev = rm.GetD3D12Device();
    if (!dev) return;
    chamsMHInit();
    if (!s_cgpsHooked) {
        void** vtbl = *(void***)dev;
        if (vtbl && vtbl[10] &&
            MH_CreateHook((LPVOID)vtbl[10], (LPVOID)&hkCreateGraphicsPipelineState,
                          (LPVOID*)&oCreateGPS) == MH_OK &&
            MH_EnableHook((LPVOID)vtbl[10]) == MH_OK) {
            s_cgpsHooked = true;
            s_d3d12Device = dev;
            LOG_INFO("Chams: CreateGraphicsPipelineState hooked", "Chams");
        } else {
            LOG_ERROR("Chams: CreateGraphicsPipelineState hook failed", "Chams");
        }
    }
    if (!s_listHooksHooked) s_armListHooks.store(true, std::memory_order_relaxed);
}

static bool hookListVtbl(void** vtbl) {
    struct Entry { int idx; void* detour; void** orig; };
    Entry entries[4] = {
        { 25, (void*)&hkSetPipelineState, (void**)&oSetPipelineState },
        { 44, (void*)&hkIASetVertexBuffers, (void**)&oIASetVB },
        { 13, (void*)&hkDrawIndexedInstanced, (void**)&oDrawIndexed },
        { 12, (void*)&hkDrawInstanced, (void**)&oDraw },
    };
    LPVOID created[4] = {};
    int ncreated = 0;
    for (const Entry& e : entries) {
        if (!vtbl[e.idx] || !sdk_safe_ptr(vtbl[e.idx], 16))
            break;
        if (MH_CreateHook((LPVOID)vtbl[e.idx], (LPVOID)e.detour, (LPVOID*)e.orig) != MH_OK ||
            MH_EnableHook((LPVOID)vtbl[e.idx]) != MH_OK)
            break;
        created[ncreated++] = (LPVOID)vtbl[e.idx];
    }
    if (ncreated == 4) return true;
    for (int i = 0; i < ncreated; ++i) {
        MH_DisableHook(created[i]);
        MH_RemoveHook(created[i]);
    }
    oSetPipelineState = nullptr; oIASetVB = nullptr;
    oDrawIndexed = nullptr; oDraw = nullptr;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG_ERROR("Chams: command-list hook failed, hooks aborted", "Chams");
    }
    return false;
}

void Chams::NoteCommandList(ID3D12CommandList* list) {
    Chams::EnsureDeviceHooks();
    if (s_listHooksHooked || !s_armListHooks.load(std::memory_order_relaxed) || !list) return;
    ID3D12GraphicsCommandList* gl = nullptr;
    if (FAILED(list->QueryInterface(__uuidof(ID3D12GraphicsCommandList), (void**)&gl)) || !gl)
        return;
    void** vtbl = *(void***)gl;
    gl->Release();
    if (!vtbl) return;
    chamsMHInit();
    if (hookListVtbl(vtbl)) {
        s_listHooksHooked = true;
        s_armListHooks.store(false, std::memory_order_relaxed);
        LOG_INFO("Chams: command-list hooks installed", "Chams");
    }
}

void Chams::InstallHook() {
    chamsMHInit();
    if (oActorRender && s_dispatcherFn) {
        MH_EnableHook((LPVOID)s_dispatcherFn);
        EnsureDeviceHooks();
        return;
    }
    uintptr_t fn = SigManager::GetInstance().ActorRenderDispatcher_render;
    if (!fn) {
        LOG_WARN("Chams: ActorRenderDispatcher::render signature unresolved; hook not installed", "Chams");
        return;
    }
    if (MH_CreateHook((LPVOID)fn, &hkActorRenderDispatcher_render,
                      (LPVOID*)&oActorRender) == MH_OK &&
        MH_EnableHook((LPVOID)fn) == MH_OK) {
        s_dispatcherFn = fn;
        LOG_INFO("Chams: hooked ActorRenderDispatcher::render", "Chams");
    } else {
        LOG_ERROR("Chams: dispatcher hook failed", "Chams");
    }

    if (!s_dispatcherFnB) {
        const uintptr_t fb = SigManager::GetInstance().ActorRenderDispatcher_renderB;
        if (fb && MH_CreateHook((LPVOID)fb, &hkActorRenderSiblingB,
                                (LPVOID*)&oActorRenderB) == MH_OK &&
            MH_EnableHook((LPVOID)fb) == MH_OK) {
            s_dispatcherFnB = fb;
            LOG_INFO("Chams: hooked sibling dispatcher B (slot-48 pass)", "Chams");
        } else {
            LOG_WARN("Chams: sibling dispatcher B hook failed/unresolved", "Chams");
        }
    } else {
        MH_EnableHook((LPVOID)s_dispatcherFnB);
    }
    if (!s_dispatcherFnC) {
        const uintptr_t fc = SigManager::GetInstance().ActorRenderDispatcher_renderC;
        if (fc && MH_CreateHook((LPVOID)fc, &hkActorRenderSiblingC,
                                (LPVOID*)&oActorRenderC) == MH_OK &&
            MH_EnableHook((LPVOID)fc) == MH_OK) {
            s_dispatcherFnC = fc;
            LOG_INFO("Chams: hooked sibling dispatcher C (depth-sorted pass)", "Chams");
        } else {
            LOG_WARN("Chams: sibling dispatcher C hook failed/unresolved", "Chams");
        }
    } else {
        MH_EnableHook((LPVOID)s_dispatcherFnC);
    }
    EnsureDeviceHooks();
}

void Chams::UninstallHook() {
    s_armListHooks.store(false, std::memory_order_relaxed);
    s_chamsOn.store(false, std::memory_order_relaxed);
    if (s_dispatcherFn) MH_DisableHook((LPVOID)s_dispatcherFn);
    if (s_dispatcherFnB) MH_DisableHook((LPVOID)s_dispatcherFnB);
    if (s_dispatcherFnC) MH_DisableHook((LPVOID)s_dispatcherFnC);
}

void Chams::OnEnable() { Chams::InstallHook(); }
void Chams::OnDisable() {
    s_chamsOn.store(false, std::memory_order_relaxed);
}
