#pragma once
#include "../Types.h"
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <cstring>
#include <imgui.h>

#include "../../Memory/OffsetManager.h"
#include "../../Core/Logger.h"

extern uintptr_t g_PlatformGameCoreAddr,g_ClientInstance_grabCursor,g_ClientInstance_releaseCursor;
extern uintptr_t g_RakNetInstance_ctor;
extern uintptr_t g_GuiData_displayClientMessage;

namespace SDK {

class MinecraftGame; class Minecraft; class LevelRenderer; class LevelRendererPlayer; class Actor;
class ClientInstance;
class GuiData; class GameRenderer; class ScreenView; class VisualTree;
class PacketSender; class RakNetConnector;
class NetworkSystem;
class BlockSource;

class Platform_GameCore{public:MinecraftGame* getMinecraftGame();static Platform_GameCore* get();};
// Map values retain two interface pointers, the concrete client and its control block.
struct ClientInstanceCtnr{
    uintptr_t subObject=0; uintptr_t weakSubObject=0;
    ClientInstance* object=nullptr; uintptr_t ctrlBlock=0;
    ClientInstance* getObject()const;
};
class ClientInstance{public:static ClientInstance* get();
    // DEAD on 1.26.50.04: CI no longer holds the view matrix or FOV.
    // CameraSetup::writeMatrices (0x1404C0DA0) resolves the owner via CI
    // vt[186] and writes view(+0x388)/proj(+0x408) there; FOV lives on
    // LevelRendererPlayer (+0xF58/+0xF6C). Do NOT wire new call sites to these
    // getters — use FrameView or GameRenderer::lastViewMatrix.
    Matrix4* getViewMatrix(){return(Matrix4*)((uintptr_t)this+Offsets().CI_ViewMatrix);}ImVec2 getFov(){return{*(float*)((uintptr_t)this+Offsets().CI_FovX),*(float*)((uintptr_t)this+Offsets().CI_FovY)};}
    MinecraftGame* getMinecraftGame(){return*(MinecraftGame**)((uintptr_t)this+Offsets().CI_MinecraftGame);}Minecraft* getMinecraft(){return*(Minecraft**)((uintptr_t)this+Offsets().CI_Minecraft);}
    LevelRenderer* getLevelRenderer(){return*(LevelRenderer**)((uintptr_t)this+Offsets().CI_LevelRenderer);}
    // The virtual returns a NonOwnerPointer; its third qword is the GuiData object.
    GuiData* getGuiData(){
        void** vt = *(void***)this;
        if (vt && sdk_safe_ptr(vt,Layout::VTable::Guard(Layout::VTable::ClientInstance_getGuiData))) {
            struct GuiDataNOP { void* ctrl; void* weak; GuiData* obj; } nop{};
            __try { ((void(__fastcall*)(ClientInstance*, void*))vt[Layout::VTable::ClientInstance_getGuiData])(this, &nop); }
            __except (EXCEPTION_EXECUTE_HANDLER) { nop.obj = nullptr; }
            if (nop.obj) return nop.obj;
        }
        return *(GuiData**)((uintptr_t)this+Offsets().CI_GuiData);
    }
    PacketSender* getPacketSender(){return*(PacketSender**)((uintptr_t)this+Offsets().CI_PacketSender);}
    // The NetworkSystem is reached through the IClientInstance virtual
    // (slot 292 on this build). Definition below — needs the NetworkSystem type.
    NetworkSystem* getNetworkSystem();
    Actor* getLocalPlayer(){void**vt=*(void***)this;return((Actor*(__fastcall*)(ClientInstance*))vt[Layout::VTable::ClientInstance_getLocalPlayer])(this);}
    BlockSource* getBlockSource();
    LevelRendererPlayer* getLevelRendererPlayer();
    void grabCursor(){if(g_ClientInstance_grabCursor){__try{((void(__fastcall*)(ClientInstance*))g_ClientInstance_grabCursor)(this);}__except(EXCEPTION_EXECUTE_HANDLER){}}}
    void releaseCursor(){if(g_ClientInstance_releaseCursor){__try{((void(__fastcall*)(ClientInstance*))g_ClientInstance_releaseCursor)(this);}__except(EXCEPTION_EXECUTE_HANDLER){}}}
};
class MinecraftGame{public:ClientInstance* getPrimaryClientInstance();GameRenderer* getGameRenderer();
    // getSoundSystem/playUi: no call sites existed and the playUi pattern died
    // on 1.26.51 — client sounds go through SoundUtils, not the game's SoundSystem.
};

class Timer{public:float tps,ticks,alpha,timeScale,passedTime;};
// The game's sized free (rcx=block, rdx=size) — all game-grown buffers
// MUST be released through it (thunk RVA 0xE934B00 on this build).
inline void sehGameSizedFree(void* raw, size_t size){
    if(!raw||!size) return;
    __try{
        const uintptr_t base=(uintptr_t)GetModuleHandleA(nullptr);
        using fn_t=void(__fastcall*)(void*,size_t);
        ((fn_t)(base+0xE934B00))(raw,size);
    }__except(EXCEPTION_EXECUTE_HANDLER){}
}
// mcUUID — the 128-bit UUID that is the KEY of the server-side player-list map.
// MUST be 16 bytes, like the mce::UUID the game uses: an 8-byte key shifts
// std::pair<const Key, PlayerListEntry> by 8 and every ::name reads garbage.
// sizeof == 16, alignof == 8 matches the game layout.
struct mcUUID {
    uint64_t mostSig = 0;
    uint64_t leastSig = 0;
    bool operator==(const mcUUID& o) const { return mostSig == o.mostSig && leastSig == o.leastSig; }
};

// Custom hasher passed explicitly as the unordered_map's Hash template arg.
// Deliberately NOT a std::hash<mcUUID> specialization: getPlayerList()
// instantiates the map inside namespace SDK before any specialization would be
// visible (C2908/C2766). The map is only iterated, never looked up, so hash
// quality is irrelevant. Hash/KeyEqual are empty types (EBO) and do not
// affect the map layout matching the game ABI.
struct mcUUIDHasher {
    size_t operator()(const mcUUID& u) const noexcept {
        return u.mostSig ^ (u.leastSig * 1099511628211ULL);
    }
};

// PlayerListEntry — a server-side player-list record (PlayerListPacket), Latite
// 1.26.30 layout: mId@+0x00 (entity unique id), mUuid@+0x08, mName@+0x18,
// skin@+0x80. sizeof(std::string) on MSVC x64 = 0x20, so pad2 = 0x48. Used by
// TabList to enumerate ALL connected players (not just nearby actors).
struct PlayerListEntry {
    int64_t     mId;        // +0x00
    mcUUID      mUuid;      // +0x08
    std::string name;       // +0x18 username
    char _pad2[0x48];       // to +0x80
    void* skinPtr;          // +0x80
};

enum class HitType : int { BLOCK = 0, ENTITY = 1, NOTHING = 3 };
struct HitResult {
    Vec3          mStartPos;    // +0x00
    Vec3          mRayDir;      // +0x0C
    HitType       mType;        // +0x18
    unsigned char mFacing;      // +0x1C
    int           mBlockPos[3]; // +0x20
    Vec3          mPos;         // +0x2C
    char          _padEntity[0x10]; // +0x38 WeakEntityRef leading pad
    unsigned int  mEntityId;    // +0x48 EntityId (entt entity id, low 32 bits)
    char          _pad[0x60 - 0x4C]; // to end of the 0x60-byte record
};

class Level{public:
    // getHitResult — the Level's reusable ray-cast HitResult
    HitResult* getHitResult(){
        if(!sdk_safe_ptr(this,sizeof(void*)))return nullptr;
        void**vt=*(void***)this;
        if(!vt||!sdk_safe_ptr(vt,sizeof(void*)*(0x14F+1)))return nullptr;
        void* fn=vt[0x14F];
        if(!fn||!sdk_safe_ptr(fn,16))return nullptr;
        using fn_t=HitResult*(__fastcall*)(Level*);
        __try{return ((fn_t)fn)(this);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    // getRuntimeActorList — the game's own actor-list accessor via the Level
    // virtual at vtable index 0x145 (ABI: void(Level*, std::vector<Actor*>&)).
    // More stable than direct entt storage iteration (dead hash 0x132A5818).
    // The *Into form populates a caller-owned vector, keeping the SEH frame
    // free of C++ objects (C2712).
    void getRuntimeActorListInto(std::vector<Actor*>* out){
        if(!out) return;
        out->clear();
        constexpr int kSlot = Layout::VTable::Level_getRuntimeActorList;
        if constexpr (kSlot < 0) { return; }
        if(!sdk_safe_ptr(this,sizeof(void*))) return;
        void**vt=*(void***)this;
        if(!vt||!sdk_safe_ptr(vt,Layout::VTable::Guard(kSlot))) return;
        if(!vt[kSlot]||!sdk_safe_ptr(vt[kSlot],16)) return;
        // Runtime family validators (actor-list-1.26.60.json): on 1.26.60.05
        // Level vt[30] is `mov rax,[rcx+0x150]; ret` and vt[0x14F] is
        // `mov rax,[rcx+0x1F0]; ret`. A mismatch means a future build moved
        // things — return empty rather than guess.
        static constexpr unsigned char kGetBS[8]  ={0x48,0x8B,0x81,0x50,0x01,0x00,0x00,0xC3};
        static constexpr unsigned char kGetHit[8] ={0x48,0x8B,0x81,0xF0,0x01,0x00,0x00,0xC3};
        if(!sdk_safe_ptr(vt[30],8)||!sdk_safe_ptr(vt[0x14F],8)) return;
        if(memcmp(vt[30],kGetBS,8)!=0||memcmp(vt[0x14F],kGetHit,8)!=0) return;

        // Raw POD storage: the callee ZEROES {begin,end,cap} and grows the
        // buffer on the game allocator. NEVER pass a real std::vector — its
        // CRT destructor would free a game-heap block (0xC0000374 heap
        // corruption).
        struct RawVec { void* b; void* e; void* c; } raw{};
        __try { ((void(__fastcall*)(void*,void*))vt[kSlot])(this,&raw); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            sehGameSizedFree(raw.b,(size_t)((char*)raw.c-(char*)raw.b));
            return;
        }
        const size_t n = (raw.b&&raw.e&&raw.e>=raw.b)
            ? (size_t)((char*)raw.e-(char*)raw.b)/sizeof(void*) : 0;
        if(n){
            __try{
                void** slots=(void**)raw.b;
                out->reserve(n);
                for(size_t i=0;i<n;++i){
                    if(slots[i]) out->push_back(reinterpret_cast<Actor*>(slots[i]));
                }
            }__except(EXCEPTION_EXECUTE_HANDLER){ out->clear(); }
        }
        if(raw.b&&raw.c&&(char*)raw.c>(char*)raw.b){
            size_t bytes=(size_t)((char*)raw.c-(char*)raw.b);
            void* blk=raw.b; size_t sz=bytes;
            if(bytes>=0x1000){ blk=*(void**)((char*)raw.b-8); sz=bytes+39; }
            sehGameSizedFree(blk,sz);
        }
    }
    // Convenience wrapper (used by SkinStealer etc.).
    std::vector<Actor*> getRuntimeActorList(){
        std::vector<Actor*> list;
        getRuntimeActorListInto(&list);
        return list;
    }
    // getPlayerList — the server-side player-list map (UUID -> PlayerListEntry),
    // Latite 1.26.30 offset 0x4E0. Contains ALL connected players, not just
    // nearby actors. nullptr when unavailable.
    std::unordered_map<mcUUID, PlayerListEntry, mcUUIDHasher>* getPlayerList(){
        const int pl=Offsets().LVL_PlayerList;
        if(!pl||!sdk_safe_ptr(this,pl+sizeof(void*)))return nullptr;
        auto* p=reinterpret_cast<std::unordered_map<mcUUID,PlayerListEntry,mcUUIDHasher>**>((uintptr_t)this+pl);
        return sdk_safe_ptr(p,sizeof(void*))?*p:nullptr;
    }
};
class GameSession{public:Level** getLevelPtr(){return(Level**)((uintptr_t)this+Offsets().GS_Level);}};
class Minecraft{public:GameSession* getGameSession(){return*(GameSession**)((uintptr_t)this+Offsets().MC_GameSession);}Timer* getTimer(){return*(Timer**)((uintptr_t)this+Offsets().MC_Timer);}Level* getLevel(){auto*gs=getGameSession();return gs?*gs->getLevelPtr():nullptr;}};

class LevelRendererPlayer{public:Vec3& getOrigin(){const int cam=Offsets().LevelRendererPlayer_mCameraPos;if(!sdk_safe_ptr((void*)((uintptr_t)this+cam),sizeof(Vec3))){static Vec3 z(0,0,0);return z;}return*(Vec3*)((uintptr_t)this+cam);}
void setOrigin(const Vec3& v){const int cam=Offsets().LevelRendererPlayer_mCameraPos;if(sdk_safe_ptr((void*)((uintptr_t)this+cam),sizeof(Vec3)))*(Vec3*)((uintptr_t)this+cam)=v;}};
class LevelRenderer{public:LevelRendererPlayer* getLevelRendererPlayer(){const int rp=Offsets().LevelRenderer_mRendererPlayer;return sdk_safe_ptr((void*)((uintptr_t)this+rp),sizeof(void*))?*(LevelRendererPlayer**)((uintptr_t)this+rp):nullptr;}};
inline LevelRendererPlayer* ClientInstance::getLevelRendererPlayer(){auto*lr=getLevelRenderer();return lr?lr->getLevelRendererPlayer():nullptr;}
class GameRenderer{public:char pad[0x3C0];Matrix4 lastViewMatrix;char pad2[0x40];Matrix4 lastProjectionMatrix;};
static_assert(offsetof(GameRenderer,lastViewMatrix)==0x3C0);
static_assert(offsetof(GameRenderer,lastProjectionMatrix)==0x440);
static_assert(sizeof(ClientInstanceCtnr)==0x20);

// Network accessors (1.26.60.05 layout):
//   ClientInstance::getNetworkSystem()   vtable[292] (byte offset 0x920)
//   NetworkSystem::remoteConnectorComposite @ 0x120
//   composite vt[3] getConnectedGameInfo() -> Social::GameConnectionInfo*
// There is NO plain composite->RakNetConnector pointer on this build:
// composite+0x70 is the NonOwnerPointer CONTROL BLOCK — never treat it as the
// connector (calling vt[3] on it crashes every frame).
class PacketSender{public:char pad[0x20];};
class NetworkSystem{public:void* remoteConnectorRaw(){return*(void**)((uintptr_t)this+Offsets().NS_RemoteConnector);}};
// Social::GameConnectionInfo (offsets per current Latite / 1.26.4x):
//   +0x08 hostIpAddress, +0x28 unresolvedUrl (DNS), +0x8C port (int),
//   +0xD0 ThirdPartyInfo -> +0x60 creatorName (featured-server label)
class GameConnectionInfo{public:
    std::string getHostIpAddress() const {
        if (!sdk_safe_ptr((void*)this, 0x8 + sizeof(std::string))) return {};
        std::string out;
        if (!sdk_safe_string((void*)((uintptr_t)this + 0x8), out)) return {};
        return out;
    }
    std::string getUnresolvedUrl() const {
        if (!sdk_safe_ptr((void*)this, 0x28 + sizeof(std::string))) return {};
        std::string out;
        if (!sdk_safe_string((void*)((uintptr_t)this + 0x28), out)) return {};
        return out;
    }
    int getPortInt() const {
        if (!sdk_safe_ptr((void*)((uintptr_t)this + 0x8C), sizeof(int))) return 0;
        __try { return *(int*)((uintptr_t)this + 0x8C); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    // 1.26.60.05 GameConnectionInfoEx (verified, sub_140280FB0): +0x130 is
    // the CreatorName literal of the join-success telemetry.
    std::string getCreatorName() const {
        if (!sdk_safe_ptr((void*)this, 0x130 + sizeof(std::string))) return {};
        std::string out;
        if (!sdk_safe_string((void*)((uintptr_t)this + 0x130), out)) return {};
        return out;
    }
    // +0x68: ServerName string (third-party join path fills it from
    // NetworkWorldInfo; base ctor sub_1404ED020 writes it).
    std::string getServerName() const {
        if (!sdk_safe_ptr((void*)this, 0x68 + sizeof(std::string))) return {};
        std::string out;
        if (!sdk_safe_string((void*)((uintptr_t)this + 0x68), out)) return {};
        return out;
    }
};
class RakNetConnector{public:
    static RakNetConnector* get();
    // RemoteConnector::getConnectedGameInfo() — vtable[3].
    GameConnectionInfo* getConnectedGameInfo() {
        if(!sdk_safe_ptr(this,sizeof(void*)))return nullptr;
        void**vt=*(void***)this;
        if(!vt||!sdk_safe_ptr(vt,sizeof(void*)*4))return nullptr;
        using fn_t=GameConnectionInfo*(__fastcall*)(void*);
        fn_t f=(fn_t)vt[3];
        if(!f)return nullptr;
        __try{return f(this);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    std::string getIpAddress() {
        GameConnectionInfo* ci = getConnectedGameInfo();
        return ci ? ci->getHostIpAddress() : std::string();
    }
    std::string getDns() {
        GameConnectionInfo* ci = getConnectedGameInfo();
        return ci ? ci->getUnresolvedUrl() : std::string();
    }
    uint16_t getPort() {
        GameConnectionInfo* ci = getConnectedGameInfo();
        int p = ci ? ci->getPortInt() : 0;
        if (p <= 0 || p > 65535) return 0;
        return (uint16_t)p;
    }
    std::string getFeaturedServer() {
        GameConnectionInfo* ci = getConnectedGameInfo();
        if (!ci) return std::string();
        // Native label priority: ServerName (+0x68), then CreatorName (+0x130).
        std::string name = ci->getServerName();
        return !name.empty() ? name : ci->getCreatorName();
    }
    // Prefer dns hostname, else raw IP.
    std::string getServerDisplay() {
        std::string d = getDns();
        if (!d.empty()) return d;
        return getIpAddress();
    }
};

class GuiData{public:float getWidth(){return*(float*)((uintptr_t)this+0x30);}float getHeight(){return*(float*)((uintptr_t)this+0x34);}
    // guiScale @0x5C, guiScaleFrac (= 1/guiScale) @0x60 — Latite's GuiData
    // layout for this build (updateGuiScale below writes both). Screen pixels
    // * guiScaleFrac = UI-space units; UI units * guiScale = pixels.
    float getGuiScale(){if(!sdk_safe_ptr(this,0x64))return 0.f;float s=*(float*)((uintptr_t)this+Offsets().GD_GuiScale);return(s>0.05f&&s<64.f)?s:0.f;}
    float getGuiScaleFrac(){if(!sdk_safe_ptr(this,0x64))return 0.f;float f=*(float*)((uintptr_t)this+Offsets().GD_GuiScaleFrac);return(f>0.f&&f<64.f)?f:0.f;}
    void updateGuiScale(float s){if(s<=0.0f)s=1.0f;float frac=1.0f/s;*(float*)((uintptr_t)this+Offsets().GD_GuiScale)=s;*(float*)((uintptr_t)this+Offsets().GD_GuiScaleFrac)=frac;float sx=*(float*)((uintptr_t)this+Offsets().GD_Width);float sy=*(float*)((uintptr_t)this+Offsets().GD_Height);*(float*)((uintptr_t)this+Offsets().GD_ScaledWidth)=sx*frac;*(float*)((uintptr_t)this+Offsets().GD_ScaledHeight)=sy*frac;}void displayClientMessage(const std::string& msg){if(g_GuiData_displayClientMessage){using f=void(*)(GuiData*,const std::string&,std::optional<std::string>,bool);((f)g_GuiData_displayClientMessage)(this,msg,{},false);}}};

// Inlines requiring complete types.
// 1.26.50.04/1.26.51.01: MinecraftGame is not a field of Platform_GameCore
// (+0x18 is a 32-bit scalar; vt[68] = `mov eax,[rcx+18h]; ret`). Resolve it
// the way the game does: *(pgc + PGC_GameHolder) is the holder,
// *(holder + PGC_MinecraftGame) is the object.
inline MinecraftGame* Platform_GameCore::getMinecraftGame(){
    const int holderOff=Offsets().PGC_GameHolder;
    const int gameOff=Offsets().PGC_MinecraftGame;
    if(!holderOff||!gameOff||!sdk_safe_ptr((void*)this,(size_t)holderOff+sizeof(void*)))return nullptr;
    void* holder=*(void**)((uintptr_t)this+(size_t)holderOff);
    if(!holder||!sdk_safe_ptr(holder,(size_t)gameOff+sizeof(void*)))return nullptr;
    return *(MinecraftGame**)((uintptr_t)holder+(size_t)gameOff);
}
inline Platform_GameCore* Platform_GameCore::get(){if(!g_PlatformGameCoreAddr)return nullptr;void*bp=*(void**)g_PlatformGameCoreAddr;return bp?*(Platform_GameCore**)((uintptr_t)bp+Offsets().PGC_Self):nullptr;}
inline ClientInstance* ClientInstance::get(){auto* pgc=Platform_GameCore::get();if(!pgc)return nullptr;auto* mcg=pgc->getMinecraftGame();return mcg?mcg->getPrimaryClientInstance():nullptr;}
inline ClientInstance* ClientInstanceCtnr::getObject()const{return object&&sdk_safe_ptr((void*)object,sizeof(void*))?object:nullptr;}
// 1.26.50.04: the split-screen client map moved MG+0x938 -> MG+0x970 and the
// value is a 3-qword Bedrock::NonOwnerPointer-like struct {obj+0x18, obj,
// ctrlblk}. Node size grew 0x38 -> 0x40; the byte key stays at node+0x20 and
// isNil at node+0x19.
inline ClientInstance* MinecraftGame::getPrimaryClientInstance(){
    // The client map walk MUST be SEH-guarded: on inject it can run before the
    // map at MG+0x970 is a constructed std::map, and an unguarded
    // std::map::find on a garbage _Myhead faults reading the key at head+0x20
    // with no __except on the stack. Probe every hop; a bad read degrades to
    // nullptr like every other accessor here.
    __try{
        const uintptr_t mg=(uintptr_t)this;
        const int mapOff=Offsets().MG_ClientMap;
        if(!mapOff||!sdk_safe_ptr((void*)mg,mapOff+0x10))return nullptr;
        const uintptr_t head=*(uintptr_t*)(mg+mapOff);      // map::_Myhead (sentinel)
        if(!head||!sdk_safe_ptr((void*)head,0x48))return nullptr;
        if(*(const uint8_t*)(head+0x19)==0)return nullptr;  // head must be the nil sentinel
        const size_t n=*(const size_t*)(mg+Offsets().MG_ClientMapSize);
        if(n>16)return nullptr;                             // split-screen client count is tiny
        auto& map=*(std::map<uint8_t,ClientInstanceCtnr>*)(mg+mapOff);
        auto it=map.find(0);
        return it!=map.end()?it->second.getObject():nullptr;
    }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
inline GameRenderer* MinecraftGame::getGameRenderer(){const int gr=Offsets().MG_GameRenderer;return gr&&sdk_safe_ptr((void*)((uintptr_t)this+gr),sizeof(void*))?*(GameRenderer**)((uintptr_t)this+gr):nullptr;}

// NetworkSystem comes from IClientInstance::vt[292]; the connector is the
// RemoteConnectorComposite at NS+NS_RemoteConnector (composite+0x70 is its
// NonOwnerPointer control block — never a connector).
inline NetworkSystem* ClientInstance::getNetworkSystem(){
    if(!sdk_safe_ptr(this,sizeof(void*)*296)) return nullptr;
    void**vt=*(void***)this;
    if(!vt||!sdk_safe_ptr(vt,sizeof(void*)*296)) return nullptr;
    using fn_t=NetworkSystem*(__fastcall*)(ClientInstance*);
    fn_t f=(fn_t)vt[Layout::VTable::ClientInstance_getNetworkSystem];
    if(!f) return nullptr;
    __try{return f(this);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
inline RakNetConnector* RakNetConnector::get(){
    __try{
        ClientInstance* ci=ClientInstance::get(); if(!ci) return nullptr;
        NetworkSystem* ns=ci->getNetworkSystem();
        if(!ns||!sdk_safe_ptr(ns,Offsets().NS_RemoteConnector+sizeof(void*))) return nullptr;
        // Native disconnected gate: without an active peer the game returns a
        // default GameConnectionInfo instead of consulting the composite.
        if(Offsets().NS_PeerHolder){
            uintptr_t holder=*(uintptr_t*)((uintptr_t)ns+Offsets().NS_PeerHolder);
            if(sdk_safe_ptr((void*)holder,Offsets().NS_PeerHolderActive+sizeof(void*)) &&
               !*(uintptr_t*)((uintptr_t)holder+Offsets().NS_PeerHolderActive)) return nullptr;
        }
        RakNetConnector* rc=(RakNetConnector*)ns->remoteConnectorRaw();
        if(!rc||!sdk_safe_ptr(rc,sizeof(void*))) return nullptr;
        return rc;
    }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}

} // SDK
