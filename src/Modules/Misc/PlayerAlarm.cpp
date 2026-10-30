#include "PlayerAlarm.h"
#include "../../Utils/MCSDK.h"
#include "../Setting.h"
#include "../Visual/Notifications.h"
#include <vector>
#include <Windows.h>

// ===========================================================================
// SEH-guarded SDK access. std::string temporaries cannot cross a __try frame
// (MSVC C2712), so every helper returns PODs / writes through out-pointers.
// ===========================================================================
namespace {

struct AlarmCtx {
    SDK::ClientInstance* ci    = nullptr;
    SDK::Level*          level = nullptr;
    SDK::Actor*          lp    = nullptr;
};

bool resolveCtx(AlarmCtx& c) {
    c.ci = SDK::ClientInstance::get();
    if (!c.ci || !sdk_safe_ptr(c.ci, 0x200)) return false;
    SDK::Minecraft* mc = c.ci->getMinecraft();
    if (!mc || !sdk_safe_ptr(mc, 0xC0)) return false;
    c.level = mc->getLevel();
    if (!c.level || !sdk_safe_ptr(c.level, 0x10)) return false;
    c.lp = c.ci->getLocalPlayer();
    if (!c.lp || !sdk_safe_ptr(c.lp, 0x230)) return false;
    return true;
}

bool getActorList(SDK::Level* level, std::vector<SDK::Actor*>& out) {
    __try { level->getRuntimeActorListInto(&out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool isPlayer(SDK::Actor* a) {
    if (!a || !sdk_safe_ptr(a, 0x230)) return false;
    __try { return a->isPlayer(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Display name, NOT getRawName(): getRawName() == getUsername() iterates the
// Level player unordered_map with no lock and no SEH, so calling it here (and
// on Chams' render thread before that) faulted inside Sunstead.dll whenever
// the roster mutated mid-iteration. getDisplayName() is SEH-guarded
// (LocalName probe + guarded vtable nametag) and never touches the map.
// Must stay outside any __try frame (returns std::string, MSVC C2712).
bool getName(SDK::Actor* a, std::string& out) {
    if (!a) return false;
    out = SDK::stripNameFormatting(a->getDisplayName());
    return !out.empty() && out != "Player"; // "Player" == unresolved placeholder
}

// Collect the names of all in-range players except the local one.
void snapshotRoster(const AlarmCtx& c, const std::vector<SDK::Actor*>& actors,
                    std::unordered_set<std::string>& out) {
    out.reserve(actors.size());
    for (SDK::Actor* a : actors) {
        if (!a || a == c.lp) continue;
        if (!isPlayer(a)) continue;
        std::string name;
        if (getName(a, name)) out.insert(std::move(name));
    }
}

} // namespace

void PlayerAlarm::announce(Event ev, const std::string& name) {
    const bool join = (ev == Event::Join);
    const Output out = GetOutput();

    if (out == Output::Notification || out == Output::Both) {
        // Notifications draw plain text; keep the marker readable without codes.
        Notifications::AddNotification((join ? "[+] " : "[-] ") + name);
    }
    if (out == Output::Chat || out == Output::Both) {
        // §a green [+], §c red [-], §f white name, prefixed by the client tag.
        const char* mark = join ? "§a[+]§f " : "§c[-]§f ";
        SDK::Chat::displayClientMessage("§7[§bPlayerAlarm§7]§r " + std::string(mark) + name);
    }
}

void PlayerAlarm::OnDisable() {
    mRoster.clear();
    mPrimed = false;
}

void PlayerAlarm::OnUpdate() {
    if (!IsEnabled()) return;

    AlarmCtx c;
    if (!resolveCtx(c)) return;

    std::vector<SDK::Actor*> actors;
    if (!getActorList(c.level, actors)) return;

    std::unordered_set<std::string> current;
    snapshotRoster(c, actors, current);

    // First pass just records the baseline — no spam for players already loaded
    // when the module was toggled on.
    if (!mPrimed) {
        mRoster = std::move(current);
        mPrimed = true;
        return;
    }

    if (GetJoins()) {
        for (const std::string& n : current)
            if (mRoster.find(n) == mRoster.end()) announce(Event::Join, n);
    }
    if (GetLeaves()) {
        for (const std::string& n : mRoster)
            if (current.find(n) == current.end()) announce(Event::Leave, n);
    }

    mRoster = std::move(current);
}
