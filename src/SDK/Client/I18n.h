#pragma once
// I18n — resolves the %translation.key placeholders servers put on the wire
// (vanilla chat renders them at draw time; a raw key is otherwise shown
// literally). Thin SEH-guarded wrapper around I18n::get(repo, std::string&
// out, const std::string& key, const std::string& lang) — sub_146583F00 on
// 1.26.50.04 — plus SigManager's global repo pointer (I18n_repo): one call
// per distinct key, results validated before use.

#include "../../Memory/SigManager.h"
#include <string>
#include <vector>

namespace SDK::I18n {

// int/void*(__fastcall*)(repo, std::string* out, key, lang) — sub_146583F00.
using GetFn = void*(__fastcall*)(void*, void*, const void*, const void*);

// POD-only SEH shim (MSVC C2712: a __try function may not own unwindable
// locals). On fault the CALLER leaks the result buffer instead of unwinding
// a std::string the game may have torn mid-write.
inline bool sehCallGet(const void* fn, void* repo, void* out,
                       const std::string* key, const std::string* lang) {
    __try {
        ((GetFn)fn)(repo, out, key, lang);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Translate one key (WITHOUT the leading '%', e.g. "multiplayer.player.left").
// Tries the lang chain ["", "en_US", "en_GB"]. A result equal to the key
// (the repo's not-found answer) counts as failure.
inline bool translateKey(const std::string& key, std::string& out) {
    out.clear();
    SigManager& sigs = SigManager::GetInstance();
    const uintptr_t fn = sigs.I18n_get;
    const uintptr_t repo = sigs.I18n_repo;
    if (!fn || !repo || key.empty()) {
        // One-shot: make a silent "never resolved" visible in the log.
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            LOG_WARN(std::string("I18n not ready: get=") +
                     (fn ? "ok" : "0") + " repo=" + (repo ? "ok" : "0"),
                     "I18n");
        }
        return false;
    }

    static const char* kLangs[] = { "", "en_US", "en_GB" };
    for (const char* langCode : kLangs) {
        // The result std::string lives on the heap: if the game call faults
        // halfway through writing it we leak the 32-byte buffer instead of
        // unwinding a torn object inside an SEH handler.
        void* mem = operator new(sizeof(std::string));
        std::string* res = new (mem) std::string();
        std::string lang(langCode);
        const bool called = sehCallGet((const void*)fn, (void*)repo, res, &key, &lang);
        bool ok = false;
        if (called) {
            // Bedrock hands back the key itself when the repo has no entry —
            // treat that (and empty) as "no translation available".
            if (!res->empty() && *res != key) {
                out = *res;
                ok = true;
            }
            res->~basic_string();
        }
        operator delete(mem);
        if (ok) return true;
    }
    return false;
}

// Replace every "%some.translation.key" token in a chat line with the game's
// translation; unknown keys stay as-is, and %s/%d printf tokens are left
// untouched (their parameters are not part of the wire text). Keys are
// [a-z0-9_.] with at least one dot, so the scan is a simple state machine.
inline std::string replacePlaceholders(const std::string& text) {
    if (text.size() < 5 || text.find('%') == std::string::npos) return text;

    SigManager& sigs = SigManager::GetInstance();
    if (!sigs.I18n_get || !sigs.I18n_repo) return text;

    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '%') { out.push_back(text[i++]); continue; }

        // Collect the candidate key: [a-z0-9_.A-Z]+ starting after '%'.
        size_t j = i + 1;
        bool dotted = false;
        while (j < text.size()) {
            const char c = text[j];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '.';
            if (!ok) break;
            if (c == '.') dotted = true;
            ++j;
        }
        const size_t keyLen = j - (i + 1);
        // Real translation keys carry a dot and are at least "a.b" long;
        // shorter runs are printf tokens or stray percent signs.
        if (keyLen < 3 || !dotted) {
            out.push_back(text[i++]);
            continue;
        }
        std::string key = text.substr(i + 1, keyLen);
        std::string translated;
        if (translateKey(key, translated)) {
            out += translated;
            i = j;
        } else {
            out.push_back(text[i++]);
        }
    }
    return out;
}

} // namespace SDK::I18n
