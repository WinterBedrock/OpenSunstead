#include "AntiBot.h"
#include "../Setting.h"
#include "../../SDK/SDK.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <Windows.h>

// ===========================================================================
// AntiBot
// Provides IsBot(Actor*) so combat modules can skip fake/bot entities. All
// native reads are SEH-guarded; every path degrades to "not a bot" on failure
// so a bad read can never make the client refuse to hit real players.
// ===========================================================================

namespace {

// Context resolution is SDK::resolveContext (SDK/Client/GameContext.h).

// PlayerList access is SDK::sehGetPlayerList / sehGetPlayerListName (GameContext.h).

// sehIsPlayer is consolidated in SDK::GameContext (resolved via ADL).


// Read the actor's hitbox width/height. Returns false if unavailable.
bool sehGetHitbox(SDK::Actor* a, float* w, float* h) {
    if (!a || !w || !h) return false;
    __try {
        auto* c = a->getAabbs();
        if (!c) return false;
        *w = c->width;
        *h = c->height;
        // Sanity: reject garbage so we never flag a real player on bad data.
        if (*w <= 0.0f || *w > 4.0f || *h <= 0.0f || *h > 4.0f) return false;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Normal Bedrock player hitbox: 0.6 wide, 1.8 tall. Give generous tolerance so
// sneaking/riding players are not misclassified.
constexpr float PLAYER_WIDTH_MIN  = 0.50f;
constexpr float PLAYER_WIDTH_MAX  = 0.72f;
constexpr float PLAYER_HEIGHT_MIN = 1.30f;
constexpr float PLAYER_HEIGHT_MAX = 1.95f;

} // namespace

bool AntiBot::IsBot(SDK::Actor* actor) {
    // Disabled => never filter anything. Combat modules call this blindly.
    if (!IsEnabled()) return false;
    if (!actor || !sdk_safe_ptr(actor, 0x230)) return false;

    const bool isPlayer = sehIsPlayer(actor);

    // Non-player actors (mobs, mounts). Off by default — Aura already player-
    // gates its targets, but other callers may want this.
    if (GetNonPlayerCheck() && !isPlayer) return true;

    // Invisibility: a "player" carrying the synced Invisible flag. Server-side
    // bot spawns are commonly flagged invisible so their NPC models never
    // render; a real player under an invisibility potion is ALSO invisible,
    // but attacking them is pointless anyway — either way skip.
    // (getStatusFlag is SEH-guarded internally: -1 on any fault, never throws.)
    if (GetInvisibleCheck() && isPlayer) {
        if (actor->getStatusFlag(ActorFlags::Invisible) == 1) return true;
    }

    // Hitbox dimensions: a player-shaped entity with an abnormal box is a bot.
    if (GetHitboxCheck() && isPlayer) {
        float w = 0.0f, h = 0.0f;
        if (sehGetHitbox(actor, &w, &h)) {
            if (w < PLAYER_WIDTH_MIN || w > PLAYER_WIDTH_MAX ||
                h < PLAYER_HEIGHT_MIN || h > PLAYER_HEIGHT_MAX)
                return true;
        }
    }

    // PlayerList membership: a "player" not present in the server tab list is a
    // client-side fake (the classic Hive shadow bot). This is the strongest
    // signal now that the tab list is read correctly.
    if (GetPlayerListCheck() && isPlayer) {
        SDK::Level* level = nullptr; SDK::Actor* lp = nullptr;
        if (SDK::resolveContext(nullptr, &level, &lp)) {
            auto* playerList = SDK::sehGetPlayerList(level);
            std::string myName;
            if (SDK::sehGetName(actor, &myName) && !myName.empty()) {
                // Only trust the list when it actually has entries; an empty
                // list (not yet populated) must not flag everyone as a bot.
                if (playerList && !playerList->empty()) {
                    bool found = false;
                    for (auto& kv : *playerList) {
                        std::string nm;
                        if (!SDK::sehGetPlayerListName(&kv.second, &nm)) continue;
                        if (nm == myName) { found = true; break; }
                    }
                    if (!found) return true;
                }
            }
        }
    }

    return false;
}
