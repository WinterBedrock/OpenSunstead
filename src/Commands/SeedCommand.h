#pragma once
#include "Command.h"
#include "CommandManager.h"
#include "../Utils/MCSDK.h"
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <cstdint>

// .seed — prints the current worlds 64-bit seed and copies it to the clipboard
static bool sehResolveLevel(void** out) {
    __try {
        SDK::ClientInstance* cInst = SDK::ClientInstance::get();
        if (!cInst || !sdk_safe_ptr(cInst, sizeof(void*))) return false;
        SDK::MinecraftGame* mg = cInst->getMinecraftGame();
        if (!mg || !sdk_safe_ptr(mg, sizeof(void*))) return false;
        SDK::ClientInstance* ci = mg->getPrimaryClientInstance();
        if (!ci || !sdk_safe_ptr(ci, sizeof(void*))) return false;
        SDK::Minecraft* mc = ci->getMinecraft();
        if (!mc || !sdk_safe_ptr(mc, sizeof(void*))) return false;
        SDK::Level* level = mc->getLevel();
        if (!level || !sdk_safe_ptr(level, sizeof(void*))) return false;
        *out = level;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool sehCallGetSeed(void* level, long long* out) {
    __try {
        void** vt = *(void***)level;
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * 136)) return false;
        if (!vt[135] || !sdk_safe_ptr(vt[135], 16)) return false;
        ((void(__fastcall*)(void*, long long*))vt[135])(level, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

class SeedCommand : public Command {
public:
    SeedCommand() : Command("seed", "Shows the world seed and copies it to the clipboard") {}

    void Execute(const std::vector<std::string>& args) override {
        (void)args;

        void* level = nullptr;
        if (!sehResolveLevel(&level)) {
            CommandManager::GetInstance().PrintToChat("§cLevel is not available, join a world first.");
            return;
        }

        // The seed is a SIGNED 64-bit value: Level::getSeed returns int64_t and
        // negative worlds exist. Reading it as unsigned printed -2814821 as
        // 18446744073706736795; keep the storage signed so the printed number
        // matches what the game (and /locate math) actually uses.
        long long seed = 0;
        if (!sehCallGetSeed(level, &seed) || seed == -1) {
            CommandManager::GetInstance().PrintToChat("§cNo seed available for this world.");
            return;
        }

        std::ostringstream ss;
        ss << seed;
        const std::string text = ss.str();

        if (CopyToClipboard(text)) {
            CommandManager::GetInstance().PrintToChat("§7Seed: §z" + text + " §7(copied to §rclipboard§7)§r");
        } else {
            CommandManager::GetInstance().PrintToChat("§7Seed: §z" + text + " §c(clipboard copy failed)§r");
        }
    }

private:
    static bool CopyToClipboard(const std::string& text) {
        if (!OpenClipboard(nullptr)) return false;
        bool ok = false;
        if (EmptyClipboard()) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
            if (h) {
                char* p = (char*)GlobalLock(h);
                if (p) {
                    memcpy(p, text.c_str(), text.size() + 1);
                    GlobalUnlock(h);
                    ok = SetClipboardData(CF_TEXT, h) != nullptr;   // system owns h on success
                    if (!ok) GlobalFree(h);
                } else {
                    GlobalFree(h);
                }
            }
        }
        CloseClipboard();
        return ok;
    }
};
