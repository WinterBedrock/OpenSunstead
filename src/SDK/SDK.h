#pragma once

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
    namespace Inv {
        inline bool setSelectedSlot(Actor* lp, int slot) {
            if (!lp || slot < 0 || slot > 8) return false;
            return PacketUtils::sendHotbarSlot(lp, slot);
        }
    }

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
        constexpr uintptr_t offset = 0x5A0;
        if(!sdk_safe_ptr(this,offset+sizeof(void*))) return nullptr;
        auto* model=*reinterpret_cast<ContainerManagerModel**>(
            reinterpret_cast<uintptr_t>(this)+offset);
        if(!sdk_safe_ptr(model,0x60)) return nullptr;
        void** vt=*reinterpret_cast<void***>(model);
        if(!sdk_safe_ptr(vt,sizeof(void*)*12)) return nullptr;
        return model;
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
