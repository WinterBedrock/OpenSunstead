#pragma once
#include "Command.h"
#include "CommandManager.h"
#include "../Modules/Visual/Player.h"

#include <string>
#include <vector>
#include <cstdlib>

// .player — SoundCloud now-playing widget control.
//
//   .player key <client_id>   store the api-v2 client_id (persisted setting)
//   .player play <query...>   search + resolve + play the first playable hit
//   .player user <link|name>  your SoundCloud profile: the wave then plays
//                             YOUR public likes (no login needed)
//   .player random|wave|next  the recommendation wave: your likes (when a
//                             profile is set) + the related-track chain
//   .player prev|back         step back through the play history
//   .player stop              stop playback and clear the widget
//   .player pause             toggle pause/resume
//   .player volume <0-100>    playback volume
//   .player status            what is loaded / playing
class PlayerCommand : public Command {
public:
    PlayerCommand() : Command("player", "SoundCloud player (.player key|play|user|wave|stop|pause|volume|status)",
                              { "music", "sc" }) {}

    void Execute(const std::vector<std::string>& args) override {
        auto& chat = CommandManager::GetInstance();
        if (args.empty()) {
            chat.PrintToChat("§7Usage: §b.player key <id> §7| §b.player play <query> §7| §b.player user <link> §7| §b.player wave §7| §b.player stop §7| §b.player pause §7| §b.player volume <0-100> §7| §b.player status");
            chat.PrintToChat(Player::StatusLine());
            return;
        }
        const std::string& sub = args[0];
        if (sub == "key") {
            if (args.size() < 2 || args[1].empty()) {
                chat.PrintToChat("§cUsage: §b.player key <client_id> §7(get one by registering an app at developers.soundcloud.com)");
                return;
            }
            Player::SetKey(args[1]);
            chat.PrintToChat("§aSoundCloud client_id stored.");
            return;
        }
        if (sub == "play" || sub == "p") {
            if (args.size() < 2) {
                chat.PrintToChat("§cUsage: §b.player play <query>");
                return;
            }
            std::string q;
            for (size_t i = 1; i < args.size(); ++i) {
                if (i > 1) q += " ";
                q += args[i];
            }
            Player::Play(q);
            chat.PrintToChat("§7Searching SoundCloud: §b" + q + "§7…");
            return;
        }
        if (sub == "random" || sub == "r" || sub == "wave" || sub == "w" ||
            sub == "next" || sub == "n" || sub == "skip") {
            Player::PlayRandom();
            chat.PrintToChat(std::string("§7Playing the wave") +
                             (Player::ProfileResolved() ? " §7(your likes + related)…" : " §7(related to your plays)…"));
            return;
        }
        if (sub == "user" || sub == "me" || sub == "profile") {
            if (args.size() < 2) {
                chat.PrintToChat("§cUsage: §b.player user <soundcloud.com/you or username> §7- then the wave plays YOUR public likes. Empty arg clears the profile.");
                Player::SetProfile("");
                return;
            }
            std::string p;
            for (size_t i = 1; i < args.size(); ++i) {
                if (i > 1) p += " ";
                p += args[i];
            }
            Player::SetProfile(p);
            chat.PrintToChat("§7Resolving profile: §b" + p + "§7…");
            return;
        }
        if (sub == "prev" || sub == "previous" || sub == "back") {
            Player::Prev();
            chat.PrintToChat("§7Previous track.");
            return;
        }
        if (sub == "stop" || sub == "s") {
            Player::Stop();
            chat.PrintToChat("§7Playback stopped.");
            return;
        }
        if (sub == "pause" || sub == "resume") {
            Player::PauseToggle();
            chat.PrintToChat(Player::StatusLine());
            return;
        }
        if (sub == "volume" || sub == "vol" || sub == "v") {
            if (args.size() < 2) {
                chat.PrintToChat("§cUsage: §b.player volume <0-100>");
                return;
            }
            char* end = nullptr;
            const long v = std::strtol(args[1].c_str(), &end, 10);
            if (!end || *end != '\0') {
                chat.PrintToChat("§cVolume must be a number 0-100.");
                return;
            }
            Player::SetVolume((int)v);
            chat.PrintToChat("§7Volume: §b" + std::to_string(v < 0 ? 0 : (v > 100 ? 100 : (int)v)));
            return;
        }
        if (sub == "status") {
            chat.PrintToChat(Player::StatusLine());
            return;
        }
        chat.PrintToChat("§cUnknown .player subcommand. Try §b.player§c for usage.");
    }
};
