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
extern uintptr_t g_Level_getRuntimeActorList;

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
extern uintptr_t g_GameSizedFree;
inline void sehGameSizedFree(void* raw, size_t size){
    if(!raw || !size || !g_GameSizedFree) return;
    __try{
        using fn_t=void(__fastcall*)(void*,size_t);
        ((fn_t)g_GameSizedFree)(raw,size);
    }__except(EXCEPTION_EXECUTE_HANDLER){}
}
// Matches the game's vector deallocator: allocations >= 0x1000 bytes are
// 32-byte-aligned with the original base pointer stored at [buf-8].
inline void sehFreeGameVec(void* begin, void* cap){
    if(!begin || !cap || (char*)cap<=(char*)begin) return;
    size_t bytes=(size_t)((char*)cap-(char*)begin);
    void* blk=begin;
    if(bytes>=0x1000){
        __try{ blk=*(void**)((char*)begin-8); }__except(EXCEPTION_EXECUTE_HANDLER){ return; }
        bytes+=0x27;
    }
    sehGameSizedFree(blk,bytes);
}
struct mcUUID {
    uint64_t mostSig = 0;
    uint64_t leastSig = 0;
    bool operator==(const mcUUID& o) const { return mostSig == o.mostSig && leastSig == o.leastSig; }
};

struct mcUUIDHasher {
    size_t operator()(const mcUUID& u) const noexcept {
        return u.mostSig ^ (u.leastSig * 1099511628211ULL);
    }
};

struct PlayerListEntry {
    int64_t     mId;        // +0x00 (entity unique id)
    mcUUID      mUuid;      // +0x08
    std::string name;       // +0x18 username
    // +0x80 was the 26.4 skin slot; 1.26.62 keeps SerializedSkinRef at +0xA0
    // (Latite-dumped: CLASS_FIELD(SerializedSkinRef, skin, 0xA0), entry size 0xB0).
    char _pad2[0xA0 - 0x38];             // to +0xA0
    struct { void* skinImpl; void* refCount; } skin; // +0xA0 SerializedSkinRef
};
static_assert(sizeof(PlayerListEntry) == 0xB0, "1.26.62 PlayerListEntry is 0xB0 bytes");

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
    HitResult* getHitResult(){
        if(!sdk_safe_ptr((void*)((uintptr_t)this+0x1F0),sizeof(void*)))return nullptr;
        return *(HitResult**)((uintptr_t)this+0x1F0);
    }
    void getRuntimeActorListInto(std::vector<Actor*>* out){
        if(!out) return;
        out->clear();
        constexpr int kSlot = Layout::VTable::Level_getRuntimeActorList;
        if(!sdk_safe_ptr(this,sizeof(void*))) return;
        void**vt=*(void***)this;
        if(!vt||!sdk_safe_ptr(vt,Layout::VTable::Guard(kSlot))) return;
        if(!g_Level_getRuntimeActorList || vt[kSlot] != (void*)g_Level_getRuntimeActorList) return;

        struct RawVec { void* b; void* e; void* c; } raw{};
        __try { ((void(__fastcall*)(void*,void*))vt[kSlot])(this,&raw); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            sehFreeGameVec(raw.b,raw.c);
            return;
        }
        const size_t n = (raw.b&&raw.e&&raw.e>=raw.b)
            ? (size_t)((char*)raw.e-(char*)raw.b)/sizeof(void*) : 0;
        if(n>16384 || (raw.c && raw.c<raw.e) ||
            (n>0 && !sdk_safe_ptr(raw.b,n*sizeof(void*)))){
            sehFreeGameVec(raw.b,raw.c);
            return;
        }
        if(n){
            __try{
                void** slots=(void**)raw.b;
                out->reserve(n);
                for(size_t i=0;i<n;++i){
                    if(slots[i]) out->push_back(reinterpret_cast<Actor*>(slots[i]));
                }
            }__except(EXCEPTION_EXECUTE_HANDLER){ out->clear(); }
        }
        sehFreeGameVec(raw.b,raw.c);
    }
    // Convenience wrapper (used by SkinStealer etc.).
    std::vector<Actor*> getRuntimeActorList(){
        std::vector<Actor*> list;
        getRuntimeActorListInto(&list);
        return list;
    }
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

class PacketSender{public:char pad[0x20];};
class NetworkSystem{public:void* remoteConnectorRaw(){return*(void**)((uintptr_t)this+Offsets().NS_RemoteConnector);}};
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
    float getGuiScale(){if(!sdk_safe_ptr(this,0x64))return 0.f;float s=*(float*)((uintptr_t)this+Offsets().GD_GuiScale);return(s>0.05f&&s<64.f)?s:0.f;}
    float getGuiScaleFrac(){if(!sdk_safe_ptr(this,0x64))return 0.f;float f=*(float*)((uintptr_t)this+Offsets().GD_GuiScaleFrac);return(f>0.f&&f<64.f)?f:0.f;}
    void updateGuiScale(float s){if(s<=0.0f)s=1.0f;float frac=1.0f/s;*(float*)((uintptr_t)this+Offsets().GD_GuiScale)=s;*(float*)((uintptr_t)this+Offsets().GD_GuiScaleFrac)=frac;float sx=*(float*)((uintptr_t)this+Offsets().GD_Width);float sy=*(float*)((uintptr_t)this+Offsets().GD_Height);*(float*)((uintptr_t)this+Offsets().GD_ScaledWidth)=sx*frac;*(float*)((uintptr_t)this+Offsets().GD_ScaledHeight)=sy*frac;}void displayClientMessage(const std::string& msg){if(g_GuiData_displayClientMessage){using f=void(*)(GuiData*,const std::string&,std::optional<std::string>,bool);((f)g_GuiData_displayClientMessage)(this,msg,{},false);}}};

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
inline ClientInstance* MinecraftGame::getPrimaryClientInstance(){
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
