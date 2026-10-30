#pragma once
// Sunstead SDK — Minecraft Bedrock 1.26.x (master include).
// Layout: Types.h / Math.h / SDKRuntime.*, plus Actor/, Client/, Inventory/,
// World/ and Network/ subdirectories.

#include "Types.h"
#include "Actor/Actor.h"
#include "Inventory/Inventory.h"
#include "Client/ClientInstance.h"
#include "World/World.h"
#include "Client/ScreenView.h"
#include "Network/Packet.h"
#include "Network/PacketUtils.h"
#include "Network/ClientPackets.h"
#include "Network/Packets/TextPacket.h"
#include "Network/Packets/PlayerAuthInputPacket.h"
#include "Actor/MobEffect.h"
#include "Actor/MobEffects.h"
#include "Actor/RenderPos.h"
#include "Client/GameContext.h"
#include "Client/GameMode.h"
#include "Client/Chat.h"
#include "Client/LevelRendererPlayer.h"
#include "Inventory/InventoryAccess.h"
#include "Runtime/Guard.h"

namespace SDK {
    // Declared in InventoryAccess.h; defined here because it needs the complete
    // PacketUtils (which itself needs Actor + Inventory).
    namespace Inv {
        inline bool setSelectedSlot(Actor* lp, int slot) {
            if (!lp || slot < 0 || slot > 8) return false;
            return PacketUtils::sendHotbarSlot(lp, slot);
        }
    }


    // Strip § color codes and take first line (nametag often has "\nrank").
    inline std::string stripNameFormatting(std::string s){
        std::string out; out.reserve(s.size());
        for(size_t i=0;i<s.size();++i){
            if((unsigned char)s[i]==0xA7 && i+1<s.size()){ ++i; continue; } // §X
            if(s[i]=='\n'||s[i]=='\r') break;
            out.push_back(s[i]);
        }
        // trim
        while(!out.empty()&&(out.back()==' '||out.back()=='\t')) out.pop_back();
        size_t a=0; while(a<out.size()&&(out[a]==' '||out[a]=='\t')) ++a;
        return out.substr(a);
    }

    inline std::string Actor::getUsername(){
        if(isPlayer()){
            const uint64_t uid=getUniqueID();
            ClientInstance* ci=ClientInstance::get();
            Minecraft* mc=ci?ci->getMinecraft():nullptr;
            Level* level=mc?mc->getLevel():nullptr;
            auto* list=level?level->getPlayerList():nullptr;
            if(list){
                for(auto& kv:*list){
                    PlayerListEntry& e=kv.second;
                    if(uid!=0 && (uint64_t)e.mId==uid){
                        std::string n;
                        if(sdk_safe_string(&e.name,n)&&!n.empty()) return stripNameFormatting(n);
                    }
                }
                // Fallback: match stripped display name to a list entry (case-insensitive).
                std::string disp=stripNameFormatting(getDisplayName());
                if(!disp.empty()){
                    for(auto& kv:*list){
                        std::string n;
                        if(!sdk_safe_string(&kv.second.name,n)||n.empty()) continue;
                        std::string sn=stripNameFormatting(n);
                        if(_stricmp(sn.c_str(),disp.c_str())==0) return sn;
                    }
                }
            }
        }
        return stripNameFormatting(getDisplayName());
    }

    inline SerializedSkin* Actor::getSkin(){
        if(!Offsets().Actor_mSerializedSkin)return nullptr;
        auto**sb=(SerializedSkinBase**)((uintptr_t)this+Offsets().Actor_mSerializedSkin);
        if(!sb||!sdk_safe_ptr(sb,sizeof(void*))||!*sb)return nullptr;
        auto*s=*sb;return(sdk_safe_ptr(s,sizeof(SerializedSkinBase))&&s->Impl)?&s->Impl->Object:nullptr;
    }
    inline ContainerManagerModel* Actor::getContainerManagerModel(){
        // Primary first: verified 0x5A0 from setContainerManagerModel.
        static const int kOffs[] = {
            0x5A0, 0x598, 0x5A8, 0x590, 0x5B0,
            0x750, 0x748, 0x758, 0x740, 0x760, 0x700
        };
        static int s_off = -1;
        auto load = [&](int off)->ContainerManagerModel*{
            if(off<0||!sdk_safe_ptr(this,(size_t)off+sizeof(void*))) return nullptr;
            void* raw=*(void**)((uintptr_t)this+(size_t)off);
            if(!raw||!sdk_safe_ptr(raw,0x60)) return nullptr;
            void** vt=*(void***)raw;
            if(!vt||!sdk_safe_ptr(vt,sizeof(void*)*12)) return nullptr;
            return reinterpret_cast<ContainerManagerModel*>(raw);
        };
        if(s_off>=0){
            auto* c=load(s_off);
            if(c) return c;
            s_off=-1;
        }
        for(int off:kOffs){
            auto* cmm=load(off);
            if(!cmm) continue;
            // Prefer getSlot(0) success, but accept IDA-verified 0x5A0 even when
            // the chest is empty / getSlot returns null (still a live CMM).
            ItemStack* s=cmm->getSlot(0);
            if(s && sdk_safe_ptr(s,0x80)){ s_off=off; return cmm; }
            if(s){ s_off=off; return cmm; }
            if(off==0x5A0){ s_off=off; return cmm; }
        }
        return nullptr;
    }
    inline PlayerInventory* Actor::getSupplies(){
        constexpr uintptr_t kSupplies = ::Layout::Actor::Supplies;
        if(!sdk_safe_ptr(this,kSupplies+sizeof(void*)))return nullptr;
        auto* p=*(PlayerInventory**)((uintptr_t)this+kSupplies);
        if(!p||!sdk_safe_ptr(p,::Layout::PlayerInv::Container+sizeof(void*)))return nullptr;
        int s=*(int*)((uintptr_t)p+::Layout::PlayerInv::SelectedSlot);
        if(s<0||s>8)return nullptr;
        return p;
    }
    inline ItemStack* Actor::getArmorItem(int slot){
        if(slot<0||slot>3)return nullptr;
        auto* eq=tryGet<ActorEquipmentComponent>(ActorEquipmentComponent::type_hash);
        if(!eq)return nullptr;
        void* ac=eq->armorContainer;
        if(!ac||!sdk_safe_ptr(ac,sizeof(void*)))return nullptr;
        return ((Container*)ac)->getItem(slot);
    }
}
