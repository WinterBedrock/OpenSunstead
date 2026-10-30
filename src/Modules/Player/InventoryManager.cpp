#include "InventoryManager.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../Setting.h"
#include "../../Core/Logger.h"
#include <string>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <chrono>
#include <vector>
#include <Windows.h>

SUNSTEAD_DEFINE_DBG(imDbg, "InventoryManager", 300)

extern uintptr_t g_ContainerScreenController_getItemStack;
extern uintptr_t g_ItemStackNetManager_requestDrop;

// Collection names that exist in 1.26.50.04's string table. NOTE: there is no
// "inventory_items" on this build — the player screen exposes the main
// inventory only through the combined grid.
static const char* kHotbarItems = "hotbar_items";
static const char* kCombined    = "combined_hotbar_and_inventory_items";
static const char* kArmorItems  = "armor_items";

static const char* kInventoryScreen = "inventory_screen";

// {collection name, slot} as the slot model takes it (reversed from
// sub_1433E52A0 / sub_1433EF210: std::string @+0, int slot @+0x20).
struct NetSlotRef {
    std::string collection;
    int slot = 0;
    int pad = 0;
};
static_assert(sizeof(std::string) == 32, "MSVC std::string layout expected");
static_assert(offsetof(NetSlotRef, slot) == 32, "slot must sit right after the name");

static uint64_t NowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- item classification (name based) --------------------------------------

static int armorTier(const std::string& n) {
    if (n.find("netherite") != std::string::npos) return 7;
    if (n.find("diamond")  != std::string::npos) return 6;
    if (n.find("iron")    != std::string::npos) return 5;
    if (n.find("turtle")  != std::string::npos) return 5;
    if (n.find("chain")   != std::string::npos) return 4;
    if (n.find("golden")  != std::string::npos) return 3;
    if (n.find("gold_")   != std::string::npos) return 3;
    if (n.find("leather") != std::string::npos) return 1;
    return 0;
}

static int armorFamily(const std::string& n) {
    if (n.find("horse") != std::string::npos) return -1;
    if (n.find("helmet")       != std::string::npos) return 0;
    if (n.find("turtle_shell") != std::string::npos) return 0;
    if (n.find("chestplate")   != std::string::npos) return 1;
    if (n.find("leggings")     != std::string::npos) return 2;
    if (n.find("boots")        != std::string::npos) return 3;
    return -1;
}

static int toolFamily(const std::string& n) {
    if (n.find("sword")   != std::string::npos) return 0;
    if (n.find("pickaxe") != std::string::npos) return 1;
    if (n.find("_axe")    != std::string::npos) return 2;
    if (n.find("shovel")  != std::string::npos) return 3;
    return -1;
}

static int toolTier(const std::string& n) {
    if (n.find("netherite") != std::string::npos) return 6;
    if (n.find("diamond")  != std::string::npos) return 5;
    if (n.find("iron")    != std::string::npos) return 4;
    if (n.find("stone")    != std::string::npos) return 3;
    if (n.find("golden")   != std::string::npos) return 2;
    if (n.find("wood")     != std::string::npos) return 1;
    return 0;
}

// ---- SEH helpers (POD frames only) -------------------------------------------

static SDK::Actor* sehLocalPlayer(SDK::ClientInstance* ci) {
    if (!ci) return nullptr;
    SDK::Actor* lp = nullptr;
    __try { lp = ci->getLocalPlayer(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return (lp && sdk_safe_ptr(lp, Layout::Probe::Actor)) ? lp : nullptr;
}

static SDK::ItemStack* sehArmorAt(SDK::Actor* lp, int slot) {
    if (!lp || slot < 0 || slot > 3) return nullptr;
    __try { return lp->getArmorItem(slot); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static int sehCount(SDK::ItemStack* st) {
    if (!st || !sdk_safe_ptr(st, 0x24)) return 0;
    __try { return st->getCount(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static std::string nameOfStack(SDK::ItemStack* st) {
    if (!st || !st->isValidStack()) return {};
    std::string n = st->getItemName();
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    return n;
}

// The slot-model action entry points (ItemStackNetManagerScreen vtable on
// 1.26.50.04, verified in IDA): place = drop-8, swap = drop-1, relative to
// requestDrop's slot in the object's REAL vtable. Each must start with its
// exact prologue; any mismatch (different build) disables the module instead
// of calling a stranger function.
static const unsigned char kPlaceProlog[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00,
    0x48, 0x8D, 0xAC, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x45, 0x20, 0xFE, 0xFF, 0xFF, 0xFF,
    0x4C, 0x89, 0xCF, 0x44, 0x89, 0xC6, 0x49, 0x89, 0xD1, 0x48, 0x83, 0xB9, 0xA0, 0x00, 0x00, 0x00, 0x00 };
static const unsigned char kSwapProlog[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00,
    0x48, 0x8D, 0xAC, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x45, 0x60, 0xFE, 0xFF, 0xFF, 0xFF,
    0x4C, 0x89, 0xC7, 0x48, 0x89, 0xD3, 0x48, 0x89, 0xCE, 0x48, 0x83, 0xB9, 0xA0 };

static bool sehResolveActions(void* model, uintptr_t dropFn, void*** vtOut,
                              uintptr_t* placeOut, uintptr_t* swapOut, int* dropIdxOut) {
    *placeOut = 0; *swapOut = 0; *dropIdxOut = -1; *vtOut = nullptr;
    if (!model || !dropFn) return false;
    __try {
        void** vt = *(void***)model;
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * 32)) return false;
        *vtOut = vt;
        int di = -1;
        for (int i = 0; i < 512; ++i)
            if ((uintptr_t)vt[i] == dropFn) { di = i; break; }
        *dropIdxOut = di;
        if (di < 8) return false;
        const uintptr_t place = (uintptr_t)vt[di - 8];
        const uintptr_t swap  = (uintptr_t)vt[di - 1];
        if (place && sdk_safe_ptr((void*)place, sizeof(kPlaceProlog)) &&
            memcmp((void*)place, kPlaceProlog, sizeof(kPlaceProlog)) == 0)
            *placeOut = place;
        if (swap && sdk_safe_ptr((void*)swap, sizeof(kSwapProlog)) &&
            memcmp((void*)swap, kSwapProlog, sizeof(kSwapProlog)) == 0)
            *swapOut = swap;
        return *placeOut != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void** sehVtOf(void* obj) {
    if (!obj) return nullptr;
    __try { return *(void***)obj; } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool sehCallPlace(uintptr_t fn, void* model, void* src, int count, void* dst) {
    __try {
        using fn_t = unsigned char(__fastcall*)(void*, void*, int, void*);
        ((fn_t)fn)(model, src, count, dst);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool sehCallSwap(uintptr_t fn, void* model, void* a, void* b) {
    __try {
        using fn_t = unsigned char(__fastcall*)(void*, void*, void*);
        ((fn_t)fn)(model, a, b);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool sehCallDrop(uintptr_t fn, void* model, void* src, int* params) {
    __try {
        using fn_t = unsigned char(__fastcall*)(void*, void*, void*);
        ((fn_t)fn)(model, src, params);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// Settings (index order pinned — configs depend on it):
//   [0] Equip Armor  [1] Tools To Hotbar  [2] Blocks To Inventory
//   [3] Drop Trash   [4] Trash Items      [5] Delay  [6] Debug
// ---------------------------------------------------------------------------
InventoryManager::InventoryManager()
    : Module("InventoryManager",
             "Equips your best armor, moves your best tools into the hotbar and drops trash while your inventory screen is open",
             Category::Player, 0) {
    AddSetting(new BoolSetting("Equip Armor", "Put the best armor piece of each slot on (swaps out a worse worn piece)", true));
    AddSetting(new BoolSetting("Tools To Hotbar", "Move the best sword/pickaxe/axe/shovel into the hotbar (empty slot, or swap with a worse copy)", true));
    AddSetting(new BoolSetting("Blocks To Inventory", "Move placeable blocks out of your hotbar into the main inventory", false));
    AddSetting(new BoolSetting("Drop Trash", "Drop whole stacks of items matching the Trash Items list", false));
    AddSetting(new StringSetting("Trash Items", "Comma-separated lowercase name fragments to drop (substring match)", "rotten_flesh,spider_eye,poisonous_potato,pufferfish,tropical_fish,dead_bush,cobweb"));
    AddSetting(new NumberSetting("Delay", "Milliseconds between two inventory actions", 80, 0, 1000, 10));
    AddSetting(new BoolSetting("Debug", "Print action diagnostics to the console", false));
}

bool InventoryManager::getEquipArmor() const         { return static_cast<BoolSetting*>(mSettings[0])->mValue; }
bool InventoryManager::getToolsToHotbar() const      { return static_cast<BoolSetting*>(mSettings[1])->mValue; }
bool InventoryManager::getBlocksToInventory() const  { return static_cast<BoolSetting*>(mSettings[2])->mValue; }
bool InventoryManager::getDropTrash() const          { return static_cast<BoolSetting*>(mSettings[3])->mValue; }
uint64_t InventoryManager::getDelayMs() const        { return (uint64_t)static_cast<NumberSetting*>(mSettings[5])->mValue; }
bool InventoryManager::isDebug() const              { return static_cast<BoolSetting*>(mSettings[6])->mValue; }
std::string InventoryManager::getTrashListRaw() const { return static_cast<StringSetting*>(mSettings[4])->mValue; }

bool InventoryManager::isKeyBlocked(int key) const {
    for (int i = 0; i < mBlockedCount; ++i)
        if (mBlocked[i] == key) return true;
    return false;
}

void InventoryManager::blockKey(int key) {
    if (isKeyBlocked(key)) return;
    if (mBlockedCount >= (int)(sizeof(mBlocked) / sizeof(mBlocked[0]))) return;
    mBlocked[mBlockedCount++] = key;
}

int InventoryManager::strikesFor(int key) const {
    for (int i = 0; i < mStrikeN; ++i) if (mStrikeKey[i] == key) return mStrikeCnt[i];
    return 0;
}

int InventoryManager::addStrike(int key) {
    for (int i = 0; i < mStrikeN; ++i)
        if (mStrikeKey[i] == key) return ++mStrikeCnt[i];
    if (mStrikeN < (int)(sizeof(mStrikeKey) / sizeof(mStrikeKey[0]))) {
        mStrikeKey[mStrikeN] = key;
        mStrikeCnt[mStrikeN] = 1;
        ++mStrikeN;
    }
    return 1;
}

void InventoryManager::resetOpenState() {
    mProbed = false;
    mProbeMs = 0;
    mProbeTries = 0;
    mCombinedOk = false;
    mHotbarOk = false;
    for (int i = 0; i < 4; ++i) mArmorUiSlot[i] = i;
    mWarnedThisOpen = false;
    mActionsWarned = false;
    mRegDumped = false;
    mDumpTries = 0;
    mLastDumpMs = 0;
    mLastActionMs = 0;
    mPendingKey = -1;
    mPendingMs = 0;
    mPendingSrcName.clear();
    mPendingSrc = -1;
    mPendingVariant = 0;
    mStrikeN = 0;
    mBlockedCount = 0;
}

void InventoryManager::OnEnable()  { resetOpenState(); }
void InventoryManager::OnDisable() { resetOpenState(); }

void InventoryManager::OnUpdate() {
    if (!IsEnabled()) return;
    if (SDK::ScreenView::currentScreenName() != kInventoryScreen)
        resetOpenState();
}

// ---------------------------------------------------------------------------
// Diagnostics: dump the collection-name registries (controller = action side,
// model = read/drop side). Layout reversed from sub_144DFA8E0 / sub_1433E52A0.
// ---------------------------------------------------------------------------
struct FnvRegistryLayout { uintptr_t sentinel; uintptr_t buckets; uintptr_t mask; };

static bool sehDumpRegistryNames(void* obj, const FnvRegistryLayout& L,
                                 char* out, size_t outCap) {
    if (!obj || !out || outCap < 4) return false;
    out[0] = 0;
    size_t used = 0;
    __try {
        const uintptr_t sentinel = *(uintptr_t*)((uintptr_t)obj + L.sentinel);
        const uintptr_t buckets   = *(uintptr_t*)((uintptr_t)obj + L.buckets);
        const uintptr_t mask      = *(uintptr_t*)((uintptr_t)obj + L.mask);
        if (!buckets || !mask || mask > 0x10000) return false;
        int printed = 0;
        for (uintptr_t b = 0; b <= mask && printed < 64; ++b) {
            if (!sdk_safe_ptr((void*)buckets, (size_t)(16 * (b + 1)))) return false;
            uintptr_t p = *(uintptr_t*)(buckets + 16 * b + 8);
            const uintptr_t end = *(uintptr_t*)(buckets + 16 * b);
            for (int hops = 0; p && p != end && p != sentinel && hops < 64; ++hops) {
                if (!sdk_safe_ptr((void*)p, 48 + 1)) break;
                const size_t size = *(size_t*)(p + 32);
                const size_t cap  = *(size_t*)(p + 40);
                if (size > 0 && size <= 48) {
                    const char* buf = (cap >= 0x10) ? *(const char**)(p + 16)
                                                   : (const char*)(p + 16);
                    if (buf && sdk_safe_ptr((void*)buf, size)) {
                        if (used + size + 2 >= outCap) return printed > 0;
                        if (used) out[used++] = ',';
                        for (size_t i = 0; i < size; ++i) out[used++] = buf[i];
                        out[used] = 0;
                        ++printed;
                    }
                }
                p = *(uintptr_t*)(p + 8);
            }
        }
        return printed > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void InventoryManager::dumpRegistries(void* cscRaw) {
    auto* csc = static_cast<SDK::ContainerScreenController*>(cscRaw);
    bool ctrlOk = false, modelOk = false;
    {
        static const FnvRegistryLayout kCtrlReg = { 0x1150, 0x1160, 0x1178 };
        char buf[2048];
        ctrlOk = sehDumpRegistryNames(cscRaw, kCtrlReg, buf, sizeof(buf));
        if (ctrlOk) LOG_INFO("controller collections: " + std::string(buf), "InventoryManager");
    }
    void* model = csc ? csc->getSlotModel() : nullptr;
    if (model) {
        static const FnvRegistryLayout kModelReg = { 0x30, 0x40, 0x58 };
        char mbuf[2048];
        modelOk = sehDumpRegistryNames(model, kModelReg, mbuf, sizeof(mbuf));
        if (modelOk) LOG_INFO("model collections: " + std::string(mbuf), "InventoryManager");
    } else {
        modelOk = true;
    }
    mRegDumped = ctrlOk && modelOk;
}

// ---------------------------------------------------------------------------
// Which UI collections mirror the player container on this screen. Cross-
// checks ContainerScreenController::getItemStack against the direct container
// read. Retried until the screen has bound its grids (first frames are empty).
// ---------------------------------------------------------------------------
bool InventoryManager::probeCollections(void* cscRaw, void* lpRaw) {
    auto* csc = static_cast<SDK::ContainerScreenController*>(cscRaw);
    auto* lp  = static_cast<SDK::Actor*>(lpRaw);
    SDK::Container* cont = SDK::Inv::container(lp);
    if (!cont || !csc) return false;

    std::string names[36];
    for (int s = 0; s < 36; ++s) names[s] = SDK::Inv::lowerNameAt(cont, s);

    if (!g_ContainerScreenController_getItemStack) {
        mCombinedOk = true;
        mHotbarOk = true;
        return true;
    }

    auto check = [&](const char* coll, int lo, int hi, int base) -> int {
        int agree = 0, bad = 0;
        for (int s = lo; s < hi; ++s) {
            std::string ui = nameOfStack(csc->getItemStack(coll, s - base));
            if (ui.empty() && names[s].empty()) continue;
            if (ui == names[s]) ++agree; else ++bad;
        }
        if (isDebug())
            LOG_INFO(std::string("probe '") + coll + "': " + std::to_string(agree) + " agree, "
                     + std::to_string(bad) + " mismatch", "InventoryManager");
        if (bad) return -1;
        return agree;
    };
    bool anyItem = false;
    for (int s = 0; s < 36; ++s) if (!names[s].empty()) { anyItem = true; break; }

    const int c = check(kCombined, 0, 36, 0);
    const int h = check(kHotbarItems, 0, 9, 0);
    mCombinedOk = (c > 0) || (c == 0 && !anyItem);
    mHotbarOk   = (h > 0);
    if (!mCombinedOk && !mHotbarOk) return false;

    // Armor grid order: identity (helmet, chest, legs, boots) unless a worn
    // piece proves otherwise.
    for (int fam = 0; fam < 4; ++fam) {
        std::string worn = nameOfStack(sehArmorAt(lp, fam));
        if (worn.empty()) continue;
        for (int ui = 0; ui < 4; ++ui)
            if (nameOfStack(csc->getItemStack(kArmorItems, ui)) == worn) { mArmorUiSlot[fam] = ui; break; }
    }
    return true;
}

bool InventoryManager::resolveModelActions(void* model) {
    if (!model) return false;
    void** vt = sehVtOf(model);
    if (!vt) return false;
    if (vt == mModelVt) return mPlaceFn != 0;
    uintptr_t place = 0, swap = 0;
    int dropIdx = -1;
    void** gotVt = nullptr;
    sehResolveActions(model, g_ItemStackNetManager_requestDrop, &gotVt, &place, &swap, &dropIdx);
    mModelVt = gotVt;
    mPlaceFn = place;
    mSwapFn = swap;
    mDropFn = (dropIdx >= 0) ? g_ItemStackNetManager_requestDrop : 0;
    LOG_INFO("slot model actions: drop idx=" + std::to_string(dropIdx)
             + " place=" + (place ? "ok" : "MISSING") + " swap=" + (swap ? "ok" : "MISSING"),
             "InventoryManager");
    return mPlaceFn != 0;
}

// variant 0: combined grid for everything (verified mirror of container slots)
// variant 1: hotbar slots through hotbar_items, the rest through combined
InventoryManager::SlotRef InventoryManager::containerRef(int s, int variant) const {
    const bool useHotbar = (s <= 8) && mHotbarOk && (variant == 1 || !mCombinedOk);
    if (useHotbar) return { kHotbarItems, s };
    return { kCombined, s };
}

InventoryManager::SlotRef InventoryManager::armorRef(int family) const {
    return { kArmorItems, mArmorUiSlot[family & 3] };
}

// ---- trash list --------------------------------------------------------------
static const std::vector<std::string>& trashFragments(const std::string& raw) {
    static std::string s_lastRaw{ "\x01" };
    static std::vector<std::string> s_frags;
    if (raw != s_lastRaw) {
        s_lastRaw = raw;
        s_frags.clear();
        std::string cur;
        for (char ch : raw) {
            if (ch == ',' || ch == ';') {
                if (!cur.empty()) s_frags.push_back(cur);
                cur.clear();
            } else if (ch != ' ' && ch != '\t') {
                cur.push_back((char)tolower((unsigned char)ch));
            }
        }
        if (!cur.empty()) s_frags.push_back(cur);
    }
    return s_frags;
}

static bool isTrash(const std::string& name, const std::vector<std::string>& frags) {
    if (name.empty()) return false;
    for (const auto& f : frags)
        if (!f.empty() && name.find(f) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Next action by priority: armor, tools, blocks out, trash.
// ---------------------------------------------------------------------------
bool InventoryManager::pickCandidate(void* lpRaw, Candidate& out) {
    auto* lp = static_cast<SDK::Actor*>(lpRaw);
    if (!lp) return false;
    SDK::Container* cont = SDK::Inv::container(lp);
    if (!cont) return false;

    std::string names[36];
    for (int s = 0; s < 36; ++s) names[s] = SDK::Inv::lowerNameAt(cont, s);

    auto offer = [&](int kind, int src, int dst, bool swap, int count) -> bool {
        const int key = (kind << 12) | (src << 6) | (dst & 63);
        if (isKeyBlocked(key)) return false;
        out.kind = kind; out.srcSlot = src; out.dstSlot = dst;
        out.swap = swap; out.count = count < 1 ? 1 : count; out.key = key;
        return true;
    };

    // ---- armor ----------------------------------------------------------------
    if (getEquipArmor()) {
        for (int f = 0; f < 4; ++f) {
            std::string wornName = nameOfStack(sehArmorAt(lp, f));
            const bool occupied = !wornName.empty();
            const int wornTier = (occupied && armorFamily(wornName) == f) ? armorTier(wornName) : 0;
            int bestTier = 0, bestSlot = -1;
            for (int s = 0; s < 36; ++s) {
                if (names[s].empty() || armorFamily(names[s]) != f) continue;
                const int t = armorTier(names[s]);
                if (t > bestTier) { bestTier = t; bestSlot = s; }
            }
            if (bestSlot < 0 || bestTier <= wornTier) continue;
            if (occupied && !mSwapFn) continue;   // can't swap on this build
            if (offer(kEquip, bestSlot, f, occupied, 1)) return true;
        }
    }

    // ---- tools ----------------------------------------------------------------
    if (getToolsToHotbar()) {
        for (int f = 0; f < 4; ++f) {
            int bestTier = 0, bestSlot = -1;
            int hotTier = 0;
            int worstHot = -1, worstHotTier = 99;
            for (int s = 0; s < 36; ++s) {
                if (names[s].empty() || toolFamily(names[s]) != f) continue;
                const int t = toolTier(names[s]);
                if (t > bestTier) { bestTier = t; bestSlot = s; }
                if (s <= 8) {
                    if (t > hotTier) hotTier = t;
                    if (t < worstHotTier) { worstHotTier = t; worstHot = s; }
                }
            }
            if (bestSlot < 9 || bestTier <= hotTier) continue;
            int empty = -1;
            for (int s = 0; s < 9; ++s) if (names[s].empty()) { empty = s; break; }
            if (worstHot >= 0 && mSwapFn) {
                // Replace the worse copy in place: keeps the player's layout.
                if (offer(kTool, bestSlot, worstHot, true, 1)) return true;
            } else if (empty >= 0) {
                if (offer(kTool, bestSlot, empty, false, sehCount(cont->getItem(bestSlot)))) return true;
            }
        }
    }

    // ---- blocks out of the hotbar ----------------------------------------------
    if (getBlocksToInventory()) {
        int empty = -1;
        for (int s = 9; s < 36; ++s) if (names[s].empty()) { empty = s; break; }
        if (empty >= 0) {
            for (int s = 0; s < 9; ++s) {
                if (names[s].empty()) continue;
                SDK::ItemStack* st = cont->getItem(s);
                if (!st || !st->isPlaceableBlock()) continue;
                if (offer(kBlockOut, s, empty, false, sehCount(st))) return true;
            }
        }
    }

    // ---- trash --------------------------------------------------------------------
    if (getDropTrash() && mDropFn) {
        const auto& frags = trashFragments(getTrashListRaw());
        for (int s = 0; s < 36; ++s) {
            if (!isTrash(names[s], frags)) continue;
            if (offer(kTrash, s, 0, false, 0)) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Fire one explicit request on the slot model.
// ---------------------------------------------------------------------------
bool InventoryManager::issue(void* cscRaw, const Candidate& c) {
    auto* csc = static_cast<SDK::ContainerScreenController*>(cscRaw);
    void* model = csc ? csc->getSlotModel() : nullptr;
    if (!model) return false;

    // Alternate the hotbar mapping after each failed attempt of this action.
    const int variant = strikesFor(c.key) & 1;
    const SlotRef s = containerRef(c.srcSlot, variant);
    NetSlotRef src; src.collection = s.collection; src.slot = s.slot;

    bool ok = false;
    std::string what;
    if (c.kind == kTrash) {
        int params[3] = { 1, 0, 0 };   // whole stack (mode 1 / amount 0)
        ok = sehCallDrop(mDropFn, model, &src, params);
        what = "drop";
    } else {
        const SlotRef d = (c.kind == kEquip) ? armorRef(c.dstSlot) : containerRef(c.dstSlot, variant);
        NetSlotRef dst; dst.collection = d.collection; dst.slot = d.slot;
        if (c.swap) {
            if (!mSwapFn) return false;
            ok = sehCallSwap(mSwapFn, model, &src, &dst);
            what = "swap";
        } else {
            ok = sehCallPlace(mPlaceFn, model, &src, c.count, &dst);
            what = "place x" + std::to_string(c.count);
        }
        if (isDebug())
            LOG_INFO(what + " " + s.collection + "[" + std::to_string(s.slot) + "] -> "
                     + d.collection + "[" + std::to_string(d.slot) + "] kind=" + std::to_string(c.kind),
                     "InventoryManager");
    }
    if (!ok) {
        LOG_WARN("slot model call faulted (" + what + ")", "InventoryManager");
        blockKey(c.key);
    }
    mPendingVariant = variant;
    return ok;
}

// ---------------------------------------------------------------------------
// UI thread, once per frame while the inventory screen is open.
// ---------------------------------------------------------------------------
void InventoryManager::OnContainerFrame(void* cscRaw, const std::string& screenName) {
    if (!IsEnabled()) return;
    if (screenName != kInventoryScreen) return;
    if (!cscRaw) return;
    if (!SDK::ContainerScreenController::isContainerScreenController(cscRaw)) return;

    auto* csc = static_cast<SDK::ContainerScreenController*>(cscRaw);
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    SDK::Actor* lp = sehLocalPlayer(ci);
    if (!lp) return;

    const uint64_t now = NowMs();

    if (isDebug() && !mRegDumped && mDumpTries < 10 && now - mLastDumpMs >= 1000) {
        mLastDumpMs = now;
        ++mDumpTries;
        dumpRegistries(cscRaw);
    }

    if (!g_ItemStackNetManager_requestDrop) {
        if (!mActionsWarned) {
            mActionsWarned = true;
            LOG_WARN("requestDrop signature unresolved - cannot locate slot-model actions, module idle",
                     "InventoryManager");
        }
        return;
    }
    if (!resolveModelActions(csc->getSlotModel())) {
        if (!mActionsWarned) {
            mActionsWarned = true;
            LOG_WARN("slot-model place/swap not found on this build - module idle", "InventoryManager");
        }
        return;
    }

    // Probe (retried while the screen binds its grids).
    if (!mProbed) {
        if (mProbeMs == 0) { mProbeMs = now; return; }        // let the screen settle a frame
        if (now - mProbeMs < 120) return;
        if (probeCollections(cscRaw, lp)) {
            mProbed = true;
            if (isDebug())
                LOG_INFO(std::string("mapping: combined=") + (mCombinedOk ? "ok" : "no")
                         + " hotbar=" + (mHotbarOk ? "ok" : "no")
                         + " armor=" + std::to_string(mArmorUiSlot[0]) + std::to_string(mArmorUiSlot[1])
                         + std::to_string(mArmorUiSlot[2]) + std::to_string(mArmorUiSlot[3]),
                         "InventoryManager");
        } else {
            mProbeMs = now;
            if (++mProbeTries >= 20) {
                mProbed = true;          // give up the verification, act on defaults
                mCombinedOk = true;
                if (!mWarnedThisOpen) {
                    mWarnedThisOpen = true;
                    LOG_WARN("collection probe never agreed - using combined grid unverified", "InventoryManager");
                }
            }
            return;
        }
    }

    SDK::Container* cont = SDK::Inv::container(lp);
    if (!cont) return;

    // ---- confirm the pending action -------------------------------------------
    if (mPendingKey >= 0) {
        const std::string cur = SDK::Inv::lowerNameAt(cont, mPendingSrc);
        if (cur != mPendingSrcName) {
            imDbg(isDebug(), "action %d confirmed after %llums", mPendingKey,
                  (unsigned long long)(now - mPendingMs));
            mPendingKey = -1;
            mLastActionMs = now;
        } else if (now - mPendingMs >= kConfirmMs) {
            const int n = addStrike(mPendingKey);
            if (n >= kMaxStrikes) {
                blockKey(mPendingKey);
                LOG_WARN("action key=" + std::to_string(mPendingKey) + " rejected "
                         + std::to_string(n) + " times - skipped for this open", "InventoryManager");
            } else if (isDebug()) {
                LOG_INFO("action key=" + std::to_string(mPendingKey) + " not applied, retry "
                         + std::to_string(n) + "/" + std::to_string(kMaxStrikes), "InventoryManager");
            }
            mPendingKey = -1;
            mLastActionMs = now;
        } else {
            return;   // still waiting for this one
        }
    }

    if (now - mLastActionMs < getDelayMs()) return;

    Candidate c;
    if (!pickCandidate(lp, c)) return;

    const std::string srcName = SDK::Inv::lowerNameAt(cont, c.srcSlot);
    if (!issue(cscRaw, c)) return;
    mPendingKey = c.key;
    mPendingMs = now;
    mPendingSrc = c.srcSlot;
    mPendingSrcName = srcName;
    mLastActionMs = now;
}
