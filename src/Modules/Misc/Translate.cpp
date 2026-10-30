#include "Translate.h"
#include "../Setting.h"
#include "../Visual/Chat.h"
#include "../../SDK/Client/Chat.h"
#include "../../SDK/Network/Packet.h"
#include "../../SDK/Network/Packets/ChatPackets.h"
#include "../../SDK/Types.h"
#include "../../Core/Logger.h"
#include "../../Hooks/GameHooks.h"
#include "../../SDK/Client/ClientInstance.h"

#include <Windows.h>
#include <winhttp.h>

#include <cstdio>
#include <string>

namespace {

// `sl=auto` leaves the source language unspecified: the service detects it.
constexpr wchar_t kHost[] = L"translate.googleapis.com";
constexpr DWORD kTimeoutMs = 6000;

// A Delay line that never gets a reply is shown verbatim after this long, so a
// dead connection cannot swallow chat.
constexpr unsigned long long kGiveUpMs = 8000;
// A held scoreboard packet past this age is delivered with whatever
// translations exist, so a dead provider can never freeze the sidebar.
constexpr unsigned long long kScoreGiveUpMs = 2500;

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string wideToUtf8(const wchar_t* w, int n) {
    if (!w || n <= 0) return {};
    const int out = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    if (out <= 0) return {};
    std::string s((size_t)out, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), out, nullptr, nullptr);
    return s;
}

// Percent-encode a query value. Space is %20 rather than '+'.
std::wstring urlEncode(const std::string& utf8) {
    std::wstring out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            out.push_back((wchar_t)c);
        } else {
            wchar_t esc[4];
            swprintf_s(esc, L"%%%02X", c);
            out += esc;
        }
    }
    return out;
}

// One HTTP round-trip with failure detail. A non-200 answer lands in
// resp.status; a transport failure lands in resp.fail as "stage err=<winhttp>",
// so the log can tell "Google 429" apart from "DNS dead" / "TLS reset".
struct HttpResp {
    bool ok = false;
    unsigned long status = 0;   // HTTP status when the server answered at all
    std::string body;
    std::string fail;           // "stage err=N" on transport failure
};

HttpResp httpRequest(bool post, const wchar_t* host, const std::wstring& path,
                     const std::string& bodyUtf8) {
    HttpResp res;
    auto failAt = [&](const char* stage, unsigned long err) {
        char b[80];
        std::snprintf(b, sizeof(b), "%s err=%lu", stage, err);
        res.fail = b;
    };

    HINTERNET session = WinHttpOpen(L"Sunstead/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { failAt("open", GetLastError()); return res; }
    WinHttpSetTimeouts(session, 5000, 5000, kTimeoutMs, kTimeoutMs);

    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        failAt("connect", GetLastError());
        WinHttpCloseHandle(session);
        return res;
    }

    HINTERNET request = WinHttpOpenRequest(connect, post ? L"POST" : L"GET", path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        failAt("openreq", GetLastError());
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return res;
    }

    if (!WinHttpSendRequest(request,
                            post ? L"Content-Type: application/json; charset=utf-8\r\n"
                                 : WINHTTP_NO_ADDITIONAL_HEADERS,
                            post ? (DWORD)-1L : 0,
                            post ? (void*)bodyUtf8.data() : WINHTTP_NO_REQUEST_DATA,
                            post ? (DWORD)bodyUtf8.size() : 0,
                            post ? (DWORD)bodyUtf8.size() : 0, 0)) {
        failAt("send", GetLastError());
        goto done;
    }
    if (!WinHttpReceiveResponse(request, nullptr)) {
        failAt("recv", GetLastError());
        goto done;
    }
    {
        DWORD status = 0, sz = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                 WINHTTP_NO_HEADER_INDEX)) {
            failAt("status", GetLastError());
            goto done;
        }
        res.status = status;
    }
    if (res.status != 200) goto done;
    for (;;) {
        char buf[2048];
        DWORD read = 0;
        if (!WinHttpReadData(request, buf, sizeof(buf), &read)) {
            failAt("read", GetLastError());
            res.body.clear();
            goto done;
        }
        if (read == 0) break;
        res.body.append(buf, read);
        if (res.body.size() > 64 * 1024) break;
    }
    if (res.body.empty()) failAt("empty", 0);
    else res.ok = true;

done:
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return res;
}

// Reads one JSON string starting at json[i] == '"'. Handles the standard
// escapes plus \uXXXX. i ends up one past the closing quote.
bool readJsonString(const std::string& json, size_t& i, std::string& val) {
    val.clear();
    if (i >= json.size() || json[i] != '"') return false;
    ++i;
    while (i < json.size() && json[i] != '"') {
        const unsigned char c = (unsigned char)json[i];
        if (c == '\\' && i + 1 < json.size()) {
            const char e = json[++i];
            switch (e) {
            case 'n': val += '\n'; break;
            case 't': val += '\t'; break;
            case 'r': val += '\r'; break;
            case '"': val += '"';  break;
            case '\\': val += '\\'; break;
            case '/': val += '/';  break;
            case 'u': {
                if (i + 4 >= json.size()) return false;
                unsigned cp = 0;
                for (int d = 1; d <= 4; ++d) {
                    const char h = json[i + d];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                i += 4;
                const wchar_t wch = (wchar_t)cp;
                val += wideToUtf8(&wch, 1);
                break;
            }
            default: val += e; break;
            }
        } else {
            val += (char)c;
        }
        ++i;
    }
    if (i >= json.size() || json[i] != '"') return false;
    ++i;
    return true;
}

// Pulls a top-level string field out of a flat JSON reply: Mozhi uses
// {"translated-text":"..."}, MyMemory nests {"responseData":{"translatedText":
// "..."}} — the field name is unique in both replies, so the first hit is the
// translation.
bool extractJsonField(const std::string& json, const char* field, std::string& out) {
    const std::string key = std::string("\"") + field + "\"";
    const size_t k = json.find(key);
    if (k == std::string::npos) return false;
    size_t i = k + key.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == ':')) ++i;
    return readJsonString(json, i, out) && !out.empty();
}

// Reply shape:
//   [[["translated","original",null,null,5], ...], null, "en", ...]
// Only the FIRST string of each sentence triple is the translation. The rest
// of that triple is the source text, and everything after the sentence list
// (nulls, the detected language, a hash) must not be read as text.
bool extractTranslation(const std::string& json, std::string& out) {
    const size_t root = json.find("[[[");
    if (root == std::string::npos) return false;

    std::string acc;
    size_t i = root + 2;   // on the '[' that opens the sentence list
    while (i < json.size() && (json[i] == '[' || json[i] == ',' || json[i] == ' ')) {
        if (json[i] != '[') { ++i; continue; }
        ++i;
        while (i < json.size() && json[i] == ' ') ++i;

        std::string piece;
        if (!readJsonString(json, i, piece)) break;
        if (!piece.empty()) {
            if (!acc.empty()) acc.push_back(' ');
            acc += piece;
        }

        // Drop the rest of this triple: source text, null, null, confidence.
        int depth = 1;
        for (; i < json.size() && depth > 0; ++i) {
            if (json[i] == '[') ++depth;
            else if (json[i] == ']') --depth;
        }
    }
    if (acc.empty()) return false;
    out = acc;
    return true;
}

// Some server/system lines arrive as rawtext JSON:
//   {"rawtext":[{"text":"You have 0 friends online."}]}
// Same decode as the Chat module's: concatenate every "text" member so the
// translation (and the fallback line) shows the message, never the JSON.
bool decodeRawTextJson(const std::string& in, std::string* out) {
    size_t p = in.find_first_not_of(" \t\r\n");
    if (p == std::string::npos || in[p] != '{') return false;
    const size_t key = in.find("\"rawtext\"", p);
    if (key == std::string::npos || key > p + 32) return false;

    std::string acc;
    size_t i = p;
    while (i < in.size()) {
        const size_t k = in.find("\"text\"", i);
        if (k == std::string::npos) break;
        size_t q = in.find_first_not_of(" \t\r\n:", k + 6);
        if (q == std::string::npos) break;
        if (in[q] != '"') { i = k + 6; continue; }
        std::string val;
        if (!readJsonString(in, q, val)) break;
        i = q;
        if (!val.empty()) {
            if (!acc.empty()) acc += ' ';
            acc += val;
        }
    }
    if (acc.empty()) return false;
    *out = acc;
    return true;
}

const char* langCodeOf(int idx) {
    switch (idx) {
    case 1: return "es";
    case 2: return "zh-CN";
    case 3: return "ru";
    default: return "en";
    }
}

const char* providerName(int p) {
    return p == 1 ? "mozhi" : p == 2 ? "mymemory" : "google";
}

// Punctuation and bare numbers are not worth a request.
bool worthTranslating(const std::string& s) {
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
        if (c >= 0xC0) return true;
    }
    return false;
}

// § formatting codes (C2 A7 + one ASCII code char) are decoration, not
// language. Weak engines mangle them when sent as-is (MyMemory turned "§7"
// into "§ 7" and ate glyphs), so stripping them off the request exists behind
// the "Remove color codes" setting; with it off the codes are sent through so
// the translation keeps its colours.
std::string stripForRequest(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if ((unsigned char)s[i] == 0xC2 && i + 2 < s.size() &&
            (unsigned char)s[i + 1] == 0xA7) {
            i += 3;   // § + the one-byte colour/format code
            continue;
        }
        out += s[i++];
    }
    return out;
}

std::string readPktString(void* pkt, uintptr_t offset) {
    char buf[1024] = {0};
    if (!SDK::sehCopyPacketString(pkt, offset, buf, sizeof(buf))) return {};
    return std::string(buf);
}

// One short line, Debug builds only (LOG_INFO compiles out of Release).
void dbg(const std::string& line) {
    LOG_INFO(line, "Translate");
}

std::string clip(const std::string& s, size_t n = 80) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "...";
}

// Authoritative packet id from the packet's own getId() virtual (vtable[1]).
// POD-only + SEH: the callers' frames own std::strings (C2712).
int readPacketIdTr(SDK::Packet* packet) {
    if (!packet || !sdk_safe_ptr(packet, sizeof(void*))) return -1;
    void** vtable = *reinterpret_cast<void***>(packet);
    if (!vtable || !sdk_safe_ptr(vtable, sizeof(void*) * 2)) return -1;
    using getId_t = int(__fastcall*)(SDK::Packet*);
    __try {
        return reinterpret_cast<getId_t>(vtable[1])(packet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

} // namespace

// ---------------------------------------------------------------------------

unsigned long __stdcall Translate::workerThunk(void* self) {
    static_cast<Translate*>(self)->workerLoop();
    return 0;
}

void Translate::startWorker() {
    if (mWorker) return;
    mStop = false;
    mWorker = CreateThread(nullptr, 0, &Translate::workerThunk, this, 0, nullptr);
}

void Translate::stopWorker() {
    mStop = true;
    if (mWorker) {
        WaitForSingleObject((HANDLE)mWorker, 4000);
        CloseHandle((HANDLE)mWorker);
        mWorker = nullptr;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    mQueue.clear();
    mResults.clear();
    mWaiting.clear();
}

// Forward declarations; the definitions live in the scoreboard section below.
namespace { bool sehWriteStrTr(void* ptr, const char* data, size_t len);
           bool sehWriteViewEntry(void* namePtrAddr, const char* data, uintptr_t lenAddr, size_t len); }

// mMutex must be held by the caller.
void Translate::deliverPending(PendingScorePacket& p, const char* how) {
    for (const PendingScoreRow& row : p.rows) {
        auto hit = mScoreCache.find(row.original);
        if (hit == mScoreCache.end() || hit->second.empty()) continue;
        if (row.lenAddr) {
            mScoreStringPool.push_back(hit->second);
            if (mScoreStringPool.size() > 512) mScoreStringPool.pop_front();
            std::string& s = mScoreStringPool.back();
            sehWriteViewEntry(row.namePtr, s.data(), row.lenAddr, s.size());
        } else {
            sehWriteStrTr(row.namePtr, hit->second.data(), hit->second.size());
        }
    }
    GameHooks::ResendIncomingPacket(p.raw, p.realId);
    dbg(std::string("setscore: delivered held packet (") + how +
        ", " + std::to_string(p.rows.size()) + " rows)");
}

void Translate::OnDisable() {
    stopWorker();
    // Deliver held scoreboard packets with whatever translations exist so
    // toggling the module can never swallow a sidebar row.
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& p : mPendingScores) deliverPending(p, "disable");
    mPendingScores.clear();
    mScoreInFlight.clear();
    mScoreFailed.clear();
}

bool Translate::suffixOn() const {
    return mSettings.size() > 2 && static_cast<BoolSetting*>(mSettings[2])->mValue;
}

void Translate::enqueue(Job job) {
    job.bornMs = GetTickCount64();
    job.token = mNextToken++;
    if (mNextToken == 0) mNextToken = 1;
    std::lock_guard<std::mutex> lock(mMutex);
    if (job.delay) mWaiting.push_back(job);
    mQueue.push_back(std::move(job));
    while (mQueue.size() > 32) mQueue.pop_front();
    startWorker();
}

void Translate::workerLoop() {
    for (;;) {
        Job job;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mQueue.empty()) {
                job = std::move(mQueue.front());
                mQueue.pop_front();
                have = true;
            } else if (mStop) {
                return;
            }
        }
        if (!have) { Sleep(25); continue; }

        HttpResp resp;
        std::string translated;
        bool parsed = false;
        const std::string plain = job.stripColors ? stripForRequest(job.body) : job.body;
        if (job.provider == 1) {
            // Mozhi: server-side proxy in front of Google — the request comes
            // from the instance, not the player's IP. Two instances, second is
            // the fallback when the first is down.
            const std::wstring path =
                std::wstring(L"/api/translate?engine=google&from=auto&to=") +
                utf8ToWide(job.lang) + L"&text=" + urlEncode(plain);
            static const wchar_t* kHosts[] = { L"mozhi.pussthecat.org", L"mozhi.adminforge.de" };
            for (const wchar_t* host : kHosts) {
                resp = httpRequest(false, host, path, std::string());
                parsed = resp.ok && extractJsonField(resp.body, "translated-text", translated);
                if (parsed) break;
            }
        } else if (job.provider == 2) {
            // MyMemory: anonymous quota (~5k chars/day/IP), langpair autodetect|target.
            const std::wstring path =
                std::wstring(L"/get?q=") + urlEncode(plain) +
                L"&langpair=autodetect|" + utf8ToWide(job.lang);
            resp = httpRequest(false, L"api.mymemory.translated.net", path, std::string());
            parsed = resp.ok && extractJsonField(resp.body, "translatedText", translated);
        } else {
            const std::wstring path =
                std::wstring(L"/translate_a/single?client=gtx&sl=auto&tl=") + utf8ToWide(job.lang) +
                L"&dt=t&q=" + urlEncode(plain);
            resp = httpRequest(false, kHost, path, std::string());
            parsed = resp.ok && extractTranslation(resp.body, translated);
        }
        const bool ok = parsed;
        dbg(std::string(job.delay ? "delay" : "resend") + " #" + std::to_string(job.token) +
            " [" + providerName(job.provider) + "] " + job.lang +
            (resp.ok ? " http=200"
                     : (resp.status != 0 ? " http=" + std::to_string(resp.status)
                                         : " net=" + resp.fail)) +
            " parsed=" + (parsed ? "yes" : "no") +
            " body=\"" + clip(job.body) + "\"" +
            (ok ? " -> \"" + clip(translated) + "\"" : std::string()) +
            ((resp.ok && !parsed) ? " raw=\"" + clip(resp.body, 120) + "\"" : std::string()));

        Result r;
        r.token = job.token;
        r.ok = ok && !translated.empty();
        r.prefix = std::move(job.prefix);
        r.text = r.ok ? std::move(translated) : std::string();
        std::lock_guard<std::mutex> lock(mMutex);
        mResults.push_back(std::move(r));
    }
}

void Translate::OnTick() {
    std::deque<Result> ready;
    std::deque<Job> waiting;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        ready.swap(mResults);
        waiting.swap(mWaiting);
    }
    if (ready.empty() && waiting.empty()) return;

    const bool suffix = suffixOn();
    const unsigned long long now = GetTickCount64();

    auto findWaiting = [&](unsigned token) -> Job* {
        for (auto& j : waiting)
            if (j.token == token) return &j;
        return nullptr;
    };

    // Delay already cancelled the packet, so this call is the line's only
    // chance to appear. ArmVanillaPassthrough makes the displayClientMessage
    // hook keep the line in the Chat panel and still forward it into the
    // real HUD chat. Without that forward, an enabled Chat module swallows
    // the call and vanilla chat stays empty.
    auto show = [&](const std::string& line) -> bool {
        ::Chat::ArmVanillaPassthrough();
        if (!SDK::Chat::displayClientMessage(line)) {
            ::Chat::DisarmVanillaPassthrough();
            ::Chat::AddLine(line);
            return false;
        }
        // The hook consumes the one-shot. If it is still armed, the call
        // never entered the hook: vanilla already has the line, the panel
        // does not.
        if (::Chat::DisarmVanillaPassthrough())
            ::Chat::AddLine(line);
        return true;
    };

    for (const Result& r : ready) {
        Job* j = findWaiting(r.token);
        if (j && j->scoreboard) {
            // Scoreboard job: cache the result. A HELD packet's rows resolve
            // from the cache in the flush below; failed texts are remembered
            // verbatim so a packet is never held forever.
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mScoreInFlight.erase(j->body);
                if (r.ok && !r.text.empty()) {
                    if (mScoreCache.size() >= 256) mScoreCache.clear();
                    mScoreCache[j->body] = r.text;
                    mScoreFailed.erase(j->body);
                } else if (!mScoreCache.count(j->body)) {
                    if (mScoreFailed.size() >= 256) mScoreFailed.clear();
                    mScoreFailed.insert(j->body);
                }
            }
            j->token = 0;
            dbg("score cache #" + std::to_string(r.token) + " \"" + clip(j->body) + "\"" +
                (r.ok ? " -> \"" + clip(r.text) + "\"" : " (failed)"));
            continue;
        }
        if (j) {
            // Delay: this line has not been shown yet. The packet was cancelled,
            // so the panel never received the original — this is its only copy.
            std::string line = j->prefix + (r.ok ? r.text : j->body);
            if (r.ok && suffix) line += " [T]";
            show(line);
            dbg("show delay #" + std::to_string(r.token) + " \"" + clip(line) + "\"");
            j->token = 0;
            continue;
        }
        // Resend: Chat already recorded the original from the same packet.
        // Append the translation as its own line, with the same prefix.
        if (!r.ok) {
            dbg("drop resend #" + std::to_string(r.token) + " (no translation)");
            continue;
        }
        std::string line = r.prefix + r.text;
        if (suffix) line += " [T]";
        show(line);
        dbg("show resend #" + std::to_string(r.token) + " \"" + clip(line) + "\"");
    }

    // Held SetScore packets: once every row text has a translation (or a
    // recorded failure), rewrite the held entries and re-deliver the packet
    // through its original handler. Past the give-up, deliver as-is.
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mPendingScores.begin();
        while (it != mPendingScores.end()) {
            PendingScorePacket& p = *it;
            bool ready = true;
            for (const PendingScoreRow& row : p.rows) {
                if (mScoreCache.count(row.original) || mScoreFailed.count(row.original)) continue;
                ready = false;
                break;
            }
            const bool giveUp = (now - p.bornMs) > kScoreGiveUpMs;
            if (!ready && !giveUp) { ++it; continue; }
            deliverPending(p, ready ? "translated" : "give-up");
            it = mPendingScores.erase(it);
        }
    }

    // Delay lines that outlived the give-up window go out verbatim.
    // Scoreboard jobs never print to chat — their give-up is the held-packet
    // flush above (rows delivered verbatim), so a late timeout just drops them.
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto& j : waiting) {
            if (j.token == 0) continue;
            if (now - j.bornMs > kGiveUpMs) {
                dbg("give up #" + std::to_string(j.token) + " \"" + clip(j.body) + "\"");
                if (!j.scoreboard) show(j.prefix + j.body);
                continue;
            }
            mWaiting.push_back(std::move(j));
        }
    }
}

void Translate::OnPacketReceive(SDK::Packet* packet, int packetId, bool& cancel) {
    if (!packet) return;
    // The hook-side id comes from a handler->id table where duplicate
    // handlers keep the FIRST id (several packet ids share one handler on
    // this build), so SetScore can arrive under a different number or -1.
    // The authoritative id is the packet's own getId() virtual (vtable[1])
    // — the same read NameProtect trusts for its working scoreboard path.
    const int realId = readPacketIdTr(packet);
    const int id = (realId >= 0) ? realId : packetId;

    // SetScore (108): scoreboard line entries (Scoreboard option).
    if (id == 108) {
        dbg("packet id=" + std::to_string(id) + " (hook " + std::to_string(packetId) +
            ") SetScore received");
        OnSetScore(packet, cancel);
        return;
    }
    // SetScoreboardIdentity (112): servers that register sidebar rows as fake
    // identities (Hive-style). Static rows the hook never saw via SetScore
    // travel HERE — same hold/translate/re-deliver pipeline as SetScore.
    if (id == 112) {
        dbg("packet id=" + std::to_string(id) + " (hook " + std::to_string(packetId) +
            ") SetScoreboardIdentity received");
        OnSetScoreboardIdentity(packet, cancel);
        return;
    }
    if (id != (int)SDK::PacketID::Text && id != (int)SDK::PacketID::CommandOutput)
        return;
    if (!sdk_safe_ptr(packet, 0x100)) return;
    void* pkt = packet;

    std::string body;
    std::string prefix;

    if (id == (int)SDK::PacketID::CommandOutput) {
        unsigned char outType = 0;
        if (!SDK::sehReadPacketByte(pkt, SDK::kCmdOutType, &outType) || outType == 1) return;
        uintptr_t begin = 0, end = 0;
        if (!SDK::sehReadPacketPtr(pkt, SDK::kCmdOutMsgVec, &begin)) return;
        if (!SDK::sehReadPacketPtr(pkt, SDK::kCmdOutMsgVec + 8, &end)) return;
        if (!begin || end < begin) return;
        char line[768] = {0};
        if (!SDK::sehFormatCommandOutputMsg((void*)begin, line, sizeof(line))) return;
        body = line;
    } else {
        unsigned char type = 0, disc = 0;
        SDK::sehReadPacketByte(pkt, SDK::kTextPktType, &type);
        SDK::sehReadPacketByte(pkt, SDK::kTextPktDisc, &disc);
        if (type == 3 || type == 4 || type == 5) return;   // popup / jukebox / tip

        if (disc == 1) {
            const std::string author = readPktString(pkt, SDK::kTextPktStr);
            body = readPktString(pkt, SDK::kTextPktData);
            if (!author.empty() && !body.empty()) prefix = author + " > ";
            else if (body.empty()) body = author;
        } else {
            body = readPktString(pkt, SDK::kTextPktStr);
        }
    }
    // rawtext JSON ({"rawtext":[{"text":"..."}]}) arrives undecoded on some
    // servers — decode it like the Chat module so the translation is of the
    // message, and a failed translation never shows the JSON itself.
    {
        std::string decoded;
        if (decodeRawTextJson(body, &decoded)) body = decoded;
    }
    if (!worthTranslating(body)) {
        dbg("skip (no letters) id=" + std::to_string(id) + " \"" + clip(body) + "\"");
        return;
    }

    const bool delay = mSettings.empty() || static_cast<EnumSetting*>(mSettings[0])->mValue == 0;

    Job job;
    job.body = std::move(body);
    job.prefix = std::move(prefix);
    job.provider = mSettings.size() > 3 ? static_cast<EnumSetting*>(mSettings[3])->mValue : 0;
    job.lang = langCodeOf(mSettings.size() > 1 ? static_cast<EnumSetting*>(mSettings[1])->mValue : 0);
    job.delay = delay;
    job.stripColors = mSettings.size() > 4 && static_cast<BoolSetting*>(mSettings[4])->mValue;
    dbg(std::string(delay ? "hold" : "pass") + " id=" + std::to_string(id) +
        " [" + providerName(job.provider) + "] \"" + clip(job.body) + "\"");
    enqueue(std::move(job));

    // Delay keeps the original off the client until the translation lands.
    if (delay) cancel = true;
}

// ---------------------------------------------------------------------------
// SCOREBOARD OPTION — translate SetScore line entries in the packet.
//
// MCBE SetScorePacket (id 108), layout REVERSED ON THE LIVE BUILD — the old
// 1.26.30.05 constants (action@+0x30, entries@+0x38, stride 0x68) silently
// read entries.end/entries.cap as begin/end, which differ by 0 on a
// right-sized vector, so every packet logged "0 entries (empty)".
// Verified on the 1.26.50.04 IDB; the entry-copy code is byte-identical on
// the live 1.26.51.01 exe (FakePlayer entry copy: 50.04 sub_143B49020 RVA
// 0x3B49020 / 51.01 RVA 0x3B4A150, unique sig in both):
//   SetScorePacket:
//     +0x30  std::vector<ScorePacketInfo> entries   (begin/end/cap)
//            — send-side ctor sub_14055F030; cereal read vfn sub_143A693D0
//              targets a1+0x30; +0x48 dword = serialization mode (5 = cereal)
//   ScorePacketInfo (stride 0x60 — vector copy sub_143B48650 `+= 96`,
//            per-type copy/destroy switch on the +0x58 byte):
//     +0x00  int64_t  scoreboardId      +0x30  int32_t score
//     +0x08  int64_t  padding           +0x38  std::string customName
//     +0x10  std::string objectiveName  +0x58  uint8_t identityType
//            (customName @+0x38 valid for FakePlayer(3); Player(1)/Entity(2)
//            keep the int64 actorUniqueId there — dtor sub_14056A9C0 frees
//            the +0x38 string only for type 3, +0x10 for types 0/1/2/3)
//
// FakePlayer entries carry the printable sidebar row text — that is what
// gets translated in place. Player/Entity entries resolve their name through
// the actor list; we translate via the Actor's own printable name string
// (Actor+0xBC0 — the same field getRawName() reads, which NameProtect's
// substitution already covers) and rewrite that.
//
// The TITLES (the scoreboard's main header lines) come through
// SetDisplayObjective packets, never through SetScore, so they are naturally
// untouched — "переводить все кроме названия".
//
// Translation is asynchronous, so a cache maps original -> translated: a hit
// rewrites the packet on the hook thread at once; a miss enqueues a job whose
// result lands in the cache for the NEXT update (scoreboards re-send every
// second, so the row goes translated within a beat). The cache write runs on
// the game thread in OnTick (guarded by mMutex), the read on the hook thread.
// ---------------------------------------------------------------------------

// SetScore packet layout constants — see the evidence block above. Sorted by
// offset: vector @+0x30 in the packet; stride 0x60; per-entry fields below.
namespace {
constexpr uintptr_t kScoreEntriesVec = 0x30;
constexpr size_t    kScoreEntryStride = 0x60;
constexpr uintptr_t kScoreIdentityType = 0x58;
constexpr uintptr_t kScoreCustomName = 0x38;
constexpr uint8_t   kScoreIdFakePlayer = 3;

// POD-only string write under SEH (same helper NameProtect ships).
bool sehWriteStrTr(void* ptr, const char* data, size_t len) {
    if (!ptr || !data) return false;
    __try {
        reinterpret_cast<std::string*>(ptr)->assign(data, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// POD-only reads of the entries vector [begin,end). Empty (both null) is a
// legal state — the caller distinguishes it from a corrupt/unreadable one.
// The span must be a whole multiple of the 0x60 stride: a partial multiple
// means the layout drifted again (log the verdict, read nothing).
bool sehScoreVec(void* pkt, uintptr_t* beginOut, uintptr_t* endOut) {
    if (!pkt || !beginOut || !endOut) return false;
    __try {
        void** vec = reinterpret_cast<void**>((uintptr_t)pkt + kScoreEntriesVec);
        uintptr_t begin = (uintptr_t)vec[0], end = (uintptr_t)vec[1];
        *beginOut = begin; *endOut = end;
        if (!begin && !end) return true;          // empty vector, 0 entries
        if (!begin || end < begin) return false;
        if ((end - begin) % kScoreEntryStride != 0) return false;
        if (end - begin > kScoreEntryStride * 4096) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool sehReadByteTr(uintptr_t addr, uint8_t* out) {
    if (!addr || !out) return false;
    __try { *out = *(uint8_t*)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- SetScoreboardIdentity (id 112) layout --------------------------------
// Verified on the 1.26.50.04 IDB (cereal loader/read vfn wiring):
//   +0x30  uint8_t action                    (ScoreboardIdentityPacketType)
//   +0x38  std::vector<Entry> entries        (begin/end/cap; stride 0x20 —
//          copy helper sub_143B64BF0 raw-copies qword+0,qword+8,oword+0x10)
// Entry (0x20 bytes, no owning std::string — fake names are string_views
// into the cereal read arena; copy the CONTENT at hook time):
//   +0x00  int64_t  scoreboardId
//   +0x08  const char* name ptr              (FakePlayer view)
//   +0x10  size_t     name len
//   +0x18  uint8_t    identityType           (3 = FakePlayer)
constexpr uintptr_t kIdiActionOff  = 0x30;
constexpr uintptr_t kIdiVecOff     = 0x38;
constexpr size_t    kIdiEntryStride = 0x20;
constexpr uintptr_t kIdiNamePtrOff = 0x08;
constexpr uintptr_t kIdiNameLenOff = 0x10;
constexpr uintptr_t kIdiTypeOff    = 0x18;
constexpr uint8_t   kIdiFake       = 3;

bool sehIdiVec(void* pkt, uintptr_t* beginOut, uintptr_t* endOut) {
    if (!pkt || !beginOut || !endOut) return false;
    __try {
        void** vec = reinterpret_cast<void**>((uintptr_t)pkt + kIdiVecOff);
        uintptr_t begin = (uintptr_t)vec[0], end = (uintptr_t)vec[1];
        *beginOut = begin; *endOut = end;
        if (!begin && !end) return true;
        if (!begin || end < begin) return false;
        if ((end - begin) % kIdiEntryStride != 0) return false;
        if (end - begin > kIdiEntryStride * 4096) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Reads a FakePlayer name view (ptr@+0x08, len@+0x10). All guarded; the view
// may dangle once the read arena is gone — treat failure as "untranslatable".
bool sehReadViewEntry(uintptr_t entry, std::string& out) {
    __try {
        const char* p = *reinterpret_cast<const char* const*>(entry + kIdiNamePtrOff);
        const size_t n = *reinterpret_cast<const size_t*>(entry + kIdiNameLenOff);
        if (!p || n == 0 || n > 256) return false;
        out.assign(p, n);   // faults here if the arena is dead → caught
        return !out.empty();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Points an entry's string_view at our pooled string (ptr + len).
bool sehWriteViewEntry(void* namePtrAddr, const char* data, uintptr_t lenAddr, size_t len) {
    if (!namePtrAddr || !lenAddr || (!data && len)) return false;
    __try {
        *reinterpret_cast<const char**>(namePtrAddr) = data;
        *reinterpret_cast<size_t*>(lenAddr) = len;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

// Scoreboard delivery design (the freeze fix):
//   The old code translated misses INLINE on the receive thread — a slow
//   provider froze every incoming packet for up to the HTTP timeout per new
//   line. Now the hook never does HTTP. Instead a SetScore packet whose
//   FakePlayer rows are not all cached is HELD (cancelled + owned by
//   GameHooks), its rows are enqueued to the async worker, and OnTick
//   re-delivers the packet through the original handler once the
//   translations land (or verbatim after kScoreGiveUpMs). The client
//   scoreboard model therefore only ever receives translated rows, including
//   static rows the server never re-sends after join.
void Translate::OnSetScore(SDK::Packet* packet, bool& cancel) {
    if (!packet) return;
    if (mSettings.empty() || !static_cast<BoolSetting*>(mSettings[5])->mValue) return;

    uintptr_t begin = 0, end = 0;
    if (!sehScoreVec(packet, &begin, &end)) {
        dbg("setscore: entries vector unreadable (layout drift?)");
        return;   // untouched — the engine still gets the packet
    }
    const size_t entryCount = (end - begin) / kScoreEntryStride;
    if (entryCount == 0) {
        dbg("setscore: 0 entries (empty)");
        return;
    }
    dbg("setscore: " + std::to_string(entryCount) + " entries");

    const std::string lang = langCodeOf(mSettings.size() > 1 ? static_cast<EnumSetting*>(mSettings[1])->mValue : 0);
    const int provider = mSettings.size() > 3 ? static_cast<EnumSetting*>(mSettings[3])->mValue : 0;
    const bool stripColors = mSettings.size() > 4 && static_cast<BoolSetting*>(mSettings[4])->mValue;

    // Collect the translatable rows first; a fully cached packet rewrites in
    // place and passes through untouched (the steady state, zero cost).
    PendingScorePacket pending;
    bool missing = false;
    for (uintptr_t entry = begin; entry + kScoreEntryStride <= end; entry += kScoreEntryStride) {
        uint8_t type = 0;
        if (!sehReadByteTr(entry + kScoreIdentityType, &type)) continue;

        // Player/Entity (1/2) entries resolve their name through the actor
        // list, which NameProtect's nametag substitution already covers —
        // only FakePlayer rows carry a translatable literal here.
        void* namePtr = (type == kScoreIdFakePlayer)
            ? (void*)(entry + kScoreCustomName) : nullptr;

        std::string original;
        if (!namePtr || !sdk_safe_string(namePtr, original) || original.empty()) {
            dbg("setscore: entry type=" + std::to_string((int)type) + " (no custom name)");
            continue;
        }
        dbg("setscore: entry type=" + std::to_string((int)type) + " \"" + clip(original) + "\"");

        std::string translated;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            auto it = mScoreCache.find(original);
            if (it != mScoreCache.end()) translated = it->second;
        }
        if (!translated.empty()) {
            dbg("setscore: cache hit -> \"" + clip(translated) + "\"");
            sehWriteStrTr(namePtr, translated.data(), translated.size());
            continue;
        }
        if (!worthTranslating(original)) {
            dbg("setscore: skip (no letters)");
            continue;
        }

        PendingScoreRow row;
        row.namePtr = namePtr;
        row.original = std::move(original);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mScoreInFlight.insert(row.original).second) row.queued = true;
        }
        if (row.queued) {
            Job job;
            job.body = row.original;
            job.provider = provider;
            job.lang = lang;
            job.delay = true;          // goes to mWaiting so OnTick can branch on it
            job.stripColors = stripColors;
            job.scoreboard = true;
            enqueue(std::move(job));
            dbg("setscore: queued \"" + clip(row.original) + "\"");
        }
        pending.rows.push_back(std::move(row));
        missing = true;
    }
    if (!missing || pending.rows.empty()) return;

    // Hold the packet and deliver it once translations are in — but only if
    // the hook can actually own the reference; otherwise pass it through with
    // the untranslated rows (never lose a scoreboard update).
    if (!GameHooks::HoldIncomingPacket(packet)) {
        dbg("setscore: hold failed, passing untranslated");
        return;   // jobs stay queued; their translations land in the cache
    }
    pending.raw = packet;
    pending.bornMs = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mPendingScores.push_back(std::move(pending));
        while (mPendingScores.size() > 32) {
            // Overflow: deliver the oldest as-is (rows possibly untranslated)
            // so no scoreboard update is ever lost; releases the GameHooks hold.
            PendingScorePacket old = std::move(mPendingScores.front());
            mPendingScores.pop_front();
            deliverPending(old, "overflow");
        }
    }
    cancel = true;
}

// SetScoreboardIdentity (112) — servers that register sidebar fake names
// through the identity table instead of carrying them inside SetScore. The
// flow is identical to OnSetScore, but the name field is a string_view whose
// content lives in the cereal read-arena, so the text is COPIED at hook
// time; the rewrite later points the view at a pooled translated string.
void Translate::OnSetScoreboardIdentity(SDK::Packet* packet, bool& cancel) {
    if (!packet) return;
    if (mSettings.empty() || !static_cast<BoolSetting*>(mSettings[5])->mValue) return;

    uintptr_t begin = 0, end = 0;
    uint8_t action = 0;
    sehReadByteTr((uintptr_t)packet + kIdiActionOff, &action);
    if (!sehIdiVec(packet, &begin, &end)) {
        dbg("identity: entries vector unreadable (layout drift?)");
        return;
    }
    const size_t entryCount = (end - begin) / kIdiEntryStride;
    dbg("identity: action=" + std::to_string((int)action) +
        " entries=" + std::to_string(entryCount));
    if (entryCount == 0) return;

    const std::string lang = langCodeOf(mSettings.size() > 1 ? static_cast<EnumSetting*>(mSettings[1])->mValue : 0);
    const int provider = mSettings.size() > 3 ? static_cast<EnumSetting*>(mSettings[3])->mValue : 0;
    const bool stripColors = mSettings.size() > 4 && static_cast<BoolSetting*>(mSettings[4])->mValue;

    PendingScorePacket pending;
    bool missing = false;
    for (uintptr_t entry = begin; entry + kIdiEntryStride <= end; entry += kIdiEntryStride) {
        uint8_t type = 0;
        if (!sehReadByteTr(entry + kIdiTypeOff, &type)) continue;
        if (type != kIdiFake) {
            dbg("identity: entry type=" + std::to_string((int)type) + " (skip)");
            continue;
        }
        std::string original;
        if (!sehReadViewEntry(entry, original)) {
            dbg("identity: entry type=3 (name view unreadable at hook time)");
            continue;
        }
        dbg("identity: entry type=3 \"" + clip(original) + "\"");

        std::string translated;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            auto it = mScoreCache.find(original);
            if (it != mScoreCache.end()) translated = it->second;
        }
        if (!translated.empty()) {
            // View row rewrite, right now: point at a pooled translated copy.
            std::lock_guard<std::mutex> lock(mMutex);
            mScoreStringPool.push_back(translated);
            if (mScoreStringPool.size() > 512) mScoreStringPool.pop_front();
            std::string& s = mScoreStringPool.back();
            sehWriteViewEntry((void*)(entry + kIdiNamePtrOff), s.data(),
                              entry + kIdiNameLenOff, s.size());
            dbg("identity: cache hit -> \"" + clip(translated) + "\"");
            continue;
        }
        if (!worthTranslating(original)) {
            dbg("identity: skip (no letters)");
            continue;
        }

        PendingScoreRow row;
        row.namePtr = (void*)(entry + kIdiNamePtrOff);
        row.lenAddr = entry + kIdiNameLenOff;
        row.original = std::move(original);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mScoreInFlight.insert(row.original).second) row.queued = true;
        }
        if (row.queued) {
            Job job;
            job.body = row.original;
            job.provider = provider;
            job.lang = lang;
            job.delay = true;
            job.stripColors = stripColors;
            job.scoreboard = true;
            enqueue(std::move(job));
            dbg("identity: queued \"" + clip(row.original) + "\"");
        }
        pending.rows.push_back(std::move(row));
        missing = true;
    }
    if (!missing || pending.rows.empty()) return;

    if (!GameHooks::HoldIncomingPacket(packet)) {
        dbg("identity: hold failed, passing untranslated");
        return;
    }
    pending.raw = packet;
    pending.realId = 112;
    pending.bornMs = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mPendingScores.push_back(std::move(pending));
        while (mPendingScores.size() > 32) {
            PendingScorePacket old = std::move(mPendingScores.front());
            mPendingScores.pop_front();
            deliverPending(old, "overflow");
        }
    }
    cancel = true;
}
