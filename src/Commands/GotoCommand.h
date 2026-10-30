#pragma once
#include "Command.h"
#include "CommandManager.h"
#include "../Modules/Movement/Baritone.h"
#include "../Utils/MCSDK.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// .goto — drives the Baritone module.
//
//   .goto                  status of the current path
//   .goto <x> <y> <z>      walk to an absolute block position
//   .goto <x> <z>          same, with y taken from where you stand
//   .goto here             walk to the block you are standing on (a no-op that
//                          is useful for confirming the search works at all)
//   .goto stop             stop and clear the path
//
// Coordinates accept the vanilla `~` relative form, e.g. `.goto ~10 ~ ~-20`.
class GotoCommand : public Command {
public:
    GotoCommand() : Command("goto", "Pathfind and walk to a position (.goto x y z | stop)",
                            { "path", "baritone" }) {}

    void Execute(const std::vector<std::string>& args) override {
        auto& chat = CommandManager::GetInstance();

        if (args.empty()) {
            chat.PrintToChat(Baritone::StatusLine());
            chat.PrintToChat("§7Usage: §b.goto <x> <y> <z> §7| §b.goto here §7| §b.goto stop");
            return;
        }

        const std::string& first = args[0];
        if (first == "stop" || first == "cancel") {
            Baritone::RequestStop();
            chat.PrintToChat("§7Path cancelled.");
            return;
        }

        // Everything below needs the player's position, either as the target
        // itself or as the base for relative/omitted coordinates.
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
        if (!lp) {
            chat.PrintToChat("§cNot in a world.");
            return;
        }
        Vec3 pos{};
        if (!SDK::sehGetPos(lp, &pos)) {
            chat.PrintToChat("§cCould not read your position.");
            return;
        }
        // getPos returns the eye position; the feet block is 1.62 below it.
        const int px = (int)std::floor(pos.x);
        const int py = (int)std::floor(pos.y - 1.62f);
        const int pz = (int)std::floor(pos.z);

        int gx = px, gy = py, gz = pz;

        if (first == "here") {
            // leave gx/gy/gz at the player's own cell
        } else if (args.size() >= 3) {
            if (!parseCoord(args[0], px, &gx) ||
                !parseCoord(args[1], py, &gy) ||
                !parseCoord(args[2], pz, &gz)) {
                chat.PrintToChat("§cBad coordinates. Try §b.goto 100 64 -200§c or §b.goto ~20 ~ ~");
                return;
            }
        } else if (args.size() == 2) {
            // Two args are read as X and Z: asking for a horizontal destination
            // without caring about height is the common case.
            if (!parseCoord(args[0], px, &gx) ||
                !parseCoord(args[1], pz, &gz)) {
                chat.PrintToChat("§cBad coordinates. Try §b.goto 100 -200");
                return;
            }
            gy = py;
        } else {
            chat.PrintToChat("§cUsage: §b.goto <x> <y> <z> §7| §b.goto here §7| §b.goto stop");
            return;
        }

        std::string why;
        if (!Baritone::RequestGoto(gx, gy, gz, &why)) {
            chat.PrintToChat("§cCould not start: " + why);
            return;
        }
        chat.PrintToChat("§7Pathing to §b" + std::to_string(gx) + " " +
                         std::to_string(gy) + " " + std::to_string(gz) + "§7…");
    }

private:
    // Vanilla-style coordinate: "123", "~" (= base), "~-5" (= base - 5).
    static bool parseCoord(const std::string& s, int base, int* out) {
        if (s.empty()) return false;
        if (s[0] == '~') {
            if (s.size() == 1) { *out = base; return true; }
            char* end = nullptr;
            const long v = std::strtol(s.c_str() + 1, &end, 10);
            if (!end || *end != '\0') return false;
            *out = base + (int)v;
            return true;
        }
        char* end = nullptr;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (!end || *end != '\0') return false;
        *out = (int)v;
        return true;
    }
};
