#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

class Offhand : public Module {
public:
    Offhand() : Module("Offhand", "Automatically equips a totem/shield in the offhand", Category::Player, 0) {
        AddSetting(new EnumSetting("Item", "Item to keep equipped", 0,
            { "Totem", "Shield" }));                                                    // [0]
        AddSetting(new BoolSetting("Only Low HP", "Only equip when health is at or below the threshold", false)); // [1]
        AddSetting(new NumberSetting("HP Threshold", "Health at/below which the totem is equipped", 10.0f, 1.0f, 20.0f, 1.0f)); // [2]
        AddSetting(new BoolSetting("Only In Inventory", "Only equip while an inventory/crafting screen is open", true)); // [3]
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;

    int   GetItem()   const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }  // 0=totem 1=shield
    bool  GetOnlyLow() const { return static_cast<BoolSetting*>(mSettings[1])->mValue; }
    float GetHpThr()  const { return static_cast<NumberSetting*>(mSettings[2])->mValue; }
    bool  GetOnlyInv() const { return static_cast<BoolSetting*>(mSettings[3])->mValue; }

    static bool TryEquipTotem(SDK::Actor* lp);

private:
    static bool TryEquip(SDK::Actor* lp, int itemMode);

    int  mEquipSlot = -1;
    long long mLastFailTick = -1;
};
