#pragma once
#include "../Types.h"
#include "../../Memory/OffsetManager.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern uintptr_t g_MinecraftPackets_createPacket;
extern uintptr_t g_mouseDeviceAddr;
extern uintptr_t g_MouseDevice_feed;

namespace SDK {

// Ids the client actually reacts to. The full table lives in PacketTypes.h
// (TypePacket, decimal); the values here are the same numbers in hex.
//   CorrectPlayerMovePrediction = 161 = 0xA1 — server-authoritative movement
//   rejection (the modern lagback; MovePlayer is the legacy teleport).
// EndId = 0x169: the highest id createPacket handles on 26.60 is 0x168 (case
// 360), and the receive-hook init iterates 0..EndId.
enum class PacketID:int{Unknown=-1,Text=0x09,SetTime=0x0A,MoveActorAbsolute=0x12,MovePlayer=0x13,SetEntityMotion=0x28,LevelChunk=0x3A,PlayerList=0x3F,PlaySound=0x56,PlayerSkin=0x5D,MoveActorDelta=0x6F,CommandOutput=0x4F,PlayerAuthInput=0x90,CorrectPlayerMovePrediction=0xA1,EndId=0x169};
class PacketHandlerDispatcher{public:virtual ~PacketHandlerDispatcher()=default;virtual void handle(void*,void*,std::shared_ptr<class Packet>){}
    uintptr_t getPacketHandler(){void**vt=*(void***)this;constexpr int kSlot=::Layout::VTable::Packet_getId;return(vt&&sdk_safe_ptr(vt,::Layout::VTable::Guard(kSlot)))?(uintptr_t)vt[kSlot]:0;}
};
class Packet{public:int32_t priority=2,reliability=1;uint64_t clientSubId=0;char pad0[0x8]{};PacketHandlerDispatcher* dispatcher=nullptr;int32_t compressibility=0;char pad1[0x4]{};
    virtual ~Packet()=default;virtual PacketID getId(){return PacketID::Unknown;}virtual std::string getName(){return"Unknown";}
};
class MinecraftPackets{public:static std::shared_ptr<Packet> createPacket(PacketID id){if(!g_MinecraftPackets_createPacket)return{};return((std::shared_ptr<Packet>(*)(PacketID))g_MinecraftPackets_createPacket)(id);}};

struct MouseAction{int16_t x,y,dx,dy;int8_t action,data;int pointerId;bool forceMotionlessPointer;};
class MouseDevice{public:int16_t clickX,clickY,x,y,dx,dy,xOld,yOld;bool buttonStates[7];std::vector<MouseAction> inputs;int32_t firstMovementType;
    static MouseDevice* get(){auto*m=reinterpret_cast<MouseDevice*>(g_mouseDeviceAddr);return(m&&sdk_safe_ptr(m,64))?m:nullptr;}

    // MouseDevice::feed — 0x140361AD0 on 1.26.50.04. action 0=move 1=left
    // 2=right 3=middle 4=wheel; data 1=down 0=up. The function GameCore::
    // handleMouseInput calls per physical event: appends a MouseAction to the
    // input queue and sets buttonStates[action]; the queue is drained on the
    // same thread, so a down+up pair is a complete game-native click.
    void feed(int8_t action, int8_t data, int16_t x, int16_t y,
              int16_t dx = 0, int16_t dy = 0, int8_t pointerId = 0) {
        if (!g_MouseDevice_feed || !sdk_safe_ptr(this, 64)) return;
        ((void(__fastcall*)(void*, int8_t, int8_t, int16_t, int16_t, int16_t, int16_t, int8_t))
            g_MouseDevice_feed)(this, action, data, x, y, dx, dy, pointerId);
    }
};

// One synthetic right-click (down+up) via the game's own mouse primitive;
// false when the device or feed function is unavailable. SEH-guarded (live
// queue). MUST be called from the render thread — the input queue is produced
// and drained there, so pushing from the game thread races the drain (game-
// thread code uses requestRightClick()).
inline bool injectRightClick() {
    MouseDevice* m = MouseDevice::get();
    if (!m || !g_MouseDevice_feed) return false;
    __try {
        const int16_t x = m->x, y = m->y;
        m->feed(2, 1, x, y);
        m->feed(2, 0, x, y);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Click mailbox: game-thread modules bump this; the Present hook drains it
// and feeds the clicks on the render thread. Atomic because the two threads
// genuinely overlap.
extern std::atomic<int> g_pendingRightClicks;
inline void requestRightClick(int n = 1) {
    if (n > 0) g_pendingRightClicks.fetch_add(n, std::memory_order_relaxed);
}

namespace mce{
    struct UUID{uint64_t most,least;std::string toString()const{return std::to_string(most)+"-"+std::to_string(least);}};
    enum class ImageFormat:int{None=0,R8Unorm=1,RGB8Unorm=2,RGBA8Unorm=3};
    enum class ImageUsage:unsigned char{Unknown=0,sRGB=1,Data=2};
    class Blob{public:using del_fn=void(*)(unsigned char*);struct Deleter{del_fn mFn=nullptr;void operator()(unsigned char* x)const{if(mFn)mFn(x);}};using ptr_type=std::unique_ptr<unsigned char[],Deleter>;ptr_type mBlob{};size_t mSize{};unsigned char*data()const{return mBlob.get();}size_t size()const{return mSize;}};
    struct Image{ImageFormat format;unsigned mWidth,mHeight,mDepth;ImageUsage mUsage;Blob mImageBytes;};
}
class SerializedSkin{public:std::string mId,mPlayFabId,mFullId,mResourcePatch,mDefaultGeometryName;mce::Image mSkinImage,mCapeImage;};
class SerializedSkinBase{public:struct ThreadOwner{SerializedSkin Object;bool ThreadIdInitialized;std::thread::id ThreadId;unsigned ThreadCheckIndex;};std::shared_ptr<ThreadOwner> Impl;};

} // SDK
