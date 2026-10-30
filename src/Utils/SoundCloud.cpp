#include "SoundCloud.h"
#include <Windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cwctype>
#include <random>
#include <algorithm>
#include <fstream>
#include <vector>

namespace SoundCloud {
namespace {

struct HttpResp {
    bool ok = false;
    unsigned long status = 0;
    std::string body;
    std::vector<unsigned char> bytes;
};

// Split an http(s) URL into host + path. Returns false when unparsable.
bool splitUrl(const std::string& url, bool& secure, std::wstring& host, std::wstring& path) {
    std::string u = url;
    secure = false;
    if (u.rfind("https://", 0) == 0) { secure = true; u = u.substr(8); }
    else if (u.rfind("http://", 0) == 0) { u = u.substr(7); }
    else return false;
    const size_t slash = u.find('/');
    const std::string h = (slash == std::string::npos) ? u : u.substr(0, slash);
    const std::string p = (slash == std::string::npos) ? "/" : u.substr(slash);
    if (h.empty()) return false;
    host.assign(h.begin(), h.end());
    path.assign(p.begin(), p.end());
    return true;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// GET with redirect following (SoundCloud stream/artwork URLs are 302s to a
// CDN). depth guards against redirect loops. `headers` (optional extra
// request headers, e.g. Authorization) is sent ONLY on the first hop — never
// forwarded across a redirect, so a user OAuth token can never leak to a CDN.
HttpResp httpGetDepth(const std::string& url, size_t capBytes, int depth,
                      const std::wstring& headers = {}) {
    HttpResp res;
    if (depth > 5) return res;
    bool secure = false;
    std::wstring host, path;
    if (!splitUrl(url, secure, host, path)) return res;

    HINTERNET session = WinHttpOpen(L"Sunstead/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return res;
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(),
                                       secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); return res; }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return res;
    }
    bool done = false;
    if (WinHttpSendRequest(request,
                           headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                           headers.empty() ? 0 : (DWORD)headers.size(),
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                WINHTTP_NO_HEADER_INDEX)) {
            res.status = status;
            if (status == 301 || status == 302 || status == 303 ||
                status == 307 || status == 308) {
                // Follow Location.
                DWORD locSz = 0;
                WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &locSz,
                                    WINHTTP_NO_HEADER_INDEX);
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && locSz > 0) {
                    std::wstring loc(locSz / sizeof(wchar_t), 0);
                    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                                            WINHTTP_HEADER_NAME_BY_INDEX, loc.data(), &locSz,
                                            WINHTTP_NO_HEADER_INDEX)) {
                        while (!loc.empty() && loc.back() == L'\0') loc.pop_back();
                        WinHttpCloseHandle(request);
                        WinHttpCloseHandle(connect);
                        WinHttpCloseHandle(session);
                        return httpGetDepth(narrow(loc), capBytes, depth + 1);
                    }
                }
            } else if (status == 200) {
                for (;;) {
                    char buf[8192];
                    DWORD read = 0;
                    if (!WinHttpReadData(request, buf, sizeof(buf), &read) || read == 0) break;
                    res.body.append(buf, read);
                    res.bytes.insert(res.bytes.end(), (unsigned char*)buf,
                                     (unsigned char*)buf + read);
                    if (res.body.size() > capBytes) break;
                }
                if (!res.body.empty()) { res.ok = true; done = true; }
            }
        }
    }
    (void)done;
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return res;
}

inline HttpResp httpGet(const std::string& url, size_t capBytes) {
    return httpGetDepth(url, capBytes, 0);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back((char)c);
        else if (c == ' ') out.push_back('+');
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    }
    return out;
}

std::string getStr(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) return it->get<std::string>();
    return {};
}

// Duration cap for non-search picks (see resolveTrackJson): a "random charts"
// pick can be a 1-2 hour mix that downloads for minutes, so skip anything
// longer than this — unless the pick came from searchFirst, which sets the
// flag below for the duration of its own resolve pass.
static constexpr long long kMaxTrackMs = 8LL * 60LL * 1000LL; // 8 minutes
static bool g_allowLong = false;

// Resolve one api-v2 track JSON object to a playable Track.
// Returns false when the object has no resolvable progressive stream.
bool resolveTrackJson(const nlohmann::json& t, const std::string& clientId, Track& out) {
    if (!t.is_object()) return false;
    if (t.contains("streamable") && t["streamable"].is_boolean() &&
        !t["streamable"].get<bool>())
        return false;
    // Duration cap: a "random charts" pick can be a 1-2 hour mix, which
    // downloads for minutes and monopolises the widget. Skip anything longer
    // than kMaxTrackMs unless the user explicitly searched for it
    // (allowLong = searchFirst's contract).
    long long dur = 0;
    try { dur = t.value("duration", 0LL); } catch (...) { dur = 0; }
    if (dur > kMaxTrackMs && !g_allowLong) return false;
    if (!t.contains("media") || !t["media"].is_object()) return false;
    const auto& media = t["media"];
    if (!media.contains("transcodings") || !media["transcodings"].is_array()) return false;

    std::string progressive, fallback;
    for (const auto& tr : media["transcodings"]) {
        if (!tr.is_object()) continue;
        const std::string turl = getStr(tr, "url");
        if (turl.empty()) continue;
        std::string proto, mime;
        if (tr.contains("format") && tr["format"].is_object()) {
            proto = getStr(tr["format"], "protocol");
            mime = getStr(tr["format"], "mime_type");
        }
        const bool isProg = (proto == "progressive");
        const bool isPreview = (getStr(tr, "preset").find("preview") != std::string::npos) ||
                               (mime.find("preview") != std::string::npos);
        if (isProg && fallback.empty()) fallback = turl;
        if (isProg && mime.find("mpeg") != std::string::npos) { progressive = turl; break; }
        if (isPreview && fallback.empty()) fallback = turl;
    }
    std::string pick = !progressive.empty() ? progressive : fallback;
    if (pick.empty()) return false;

    // Resolve the transcoding to the real stream URL.
    HttpResp rs = httpGet(pick + (pick.find('?') == std::string::npos ? "?" : "&") +
                          "client_id=" + urlEncode(clientId), 16 * 1024);
    if (!rs.ok) return false;
    std::string stream;
    try {
        auto sj = nlohmann::json::parse(rs.body);
        stream = getStr(sj, "url");
    } catch (...) { return false; }
    if (stream.empty()) return false;

    Track track;
    try { track.id = t.value("id", 0LL); } catch (...) {}
    track.title = getStr(t, "title");
    if (t.contains("user") && t["user"].is_object())
        track.artist = getStr(t["user"], "username");
    try { track.durationMs = t.value("duration", 0LL); } catch (...) {}
    track.artworkUrl = upgradeArtwork(getStr(t, "artwork_url"));
    if (track.artworkUrl.empty() && t.contains("user") && t["user"].is_object())
        track.artworkUrl = upgradeArtwork(getStr(t["user"], "avatar_url"));
    track.streamUrl = stream;
    track.isPreview = (pick == fallback && progressive.empty());
    if (track.title.empty()) track.title = "Unknown title";
    if (track.artist.empty()) track.artist = "Unknown artist";
    out = std::move(track);
    return true;
}

// Try track JSON objects in random order, resolving each until one plays.
// Caps resolve attempts so a page of geo-blocked hits doesn't stall.
bool resolveRandomOrder(const std::vector<nlohmann::json>& items,
                        const std::string& clientId, Track& out, int maxAttempts = 8) {
    if (items.empty()) return false;
    std::vector<size_t> idx(items.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(idx.begin(), idx.end(), rng);
    int tried = 0;
    for (size_t i : idx) {
        if (tried++ >= maxAttempts) break;
        if (resolveTrackJson(items[i], clientId, out)) return true;
    }
    return false;
}

// Pull track JSON objects out of an api-v2 listing body. Both bare track
// objects (search/related) and wrappers ({track:{...}} — charts, likes) are
// unwrapped into the same flat list resolveRandomOrder() consumes.
void collectTracks(const nlohmann::json& j, std::vector<nlohmann::json>& items) {
    if (!j.is_object() || !j.contains("collection") || !j["collection"].is_array()) return;
    for (const auto& e : j["collection"]) {
        if (!e.is_object()) continue;
        auto it = e.find("track");
        if (it != e.end() && it->is_object()) { items.push_back(*it); continue; }
        if (e.contains("media") || e.contains("title")) items.push_back(e);
    }
}

} // namespace

std::string upgradeArtwork(const std::string& url) {
    const std::string from = "-large.";
    const size_t p = url.find(from);
    if (p == std::string::npos) return url;
    return url.substr(0, p) + "-t500x500." + url.substr(p + from.size());
}

Result searchFirst(const std::string& clientId, const std::string& query) {
    Result r;
    if (clientId.empty()) { r.error = "no client_id (.player key <id>)"; return r; }
    if (query.empty()) { r.error = "empty query"; return r; }
    // Explicit search: honor whatever the user typed, no duration filter.
    g_allowLong = true;

    const std::string url = "https://api-v2.soundcloud.com/search/tracks?q=" +
        urlEncode(query) + "&client_id=" + urlEncode(clientId) + "&limit=5";
    HttpResp resp = httpGet(url, 256 * 1024);
    if (!resp.ok) {
        char b[64];
        std::snprintf(b, sizeof(b), "search http=%lu", resp.status);
        r.error = b;
        return r;
    }

    nlohmann::json j;
    try { j = nlohmann::json::parse(resp.body); }
    catch (...) { r.error = "bad search json"; return r; }
    if (!j.is_object() || !j.contains("collection") || !j["collection"].is_array()) {
        r.error = "no results";
        return r;
    }

    // First hit with usable transcodings wins (prefer fully streamable).
    for (const auto& t : j["collection"]) {
        Track track;
        if (!resolveTrackJson(t, clientId, track)) continue;
        r.track = std::move(track);
        r.ok = true;
        return r;
    }
    r.error = "no playable hit (try another query)";
    return r;
}

Result searchRandom(const std::string& clientId, const std::string& query) {
    Result r;
    if (clientId.empty()) { r.error = "no client_id (.player key <id>)"; return r; }
    // Auto-pick path (autoplay/fallback): keep the duration cap on.
    g_allowLong = false;
    const std::string q = query.empty() ? "wave" : query;

    const std::string url = "https://api-v2.soundcloud.com/search/tracks?q=" +
        urlEncode(q) + "&client_id=" + urlEncode(clientId) + "&limit=24";
    HttpResp resp = httpGet(url, 512 * 1024);
    if (!resp.ok) {
        char b[64];
        std::snprintf(b, sizeof(b), "search http=%lu", resp.status);
        r.error = b;
        return r;
    }
    nlohmann::json j;
    try { j = nlohmann::json::parse(resp.body); }
    catch (...) { r.error = "bad search json"; return r; }
    if (!j.is_object() || !j.contains("collection") || !j["collection"].is_array()) {
        r.error = "no results";
        return r;
    }
    std::vector<nlohmann::json> items;
    for (const auto& t : j["collection"])
        if (t.is_object()) items.push_back(t);
    if (items.empty()) { r.error = "no results"; return r; }

    Track track;
    if (!resolveRandomOrder(items, clientId, track)) {
        r.error = "no playable hit (try again)";
        return r;
    }
    r.track = std::move(track);
    r.ok = true;
    return r;
}

Result chartsRandom(const std::string& clientId) {
    Result r;
    if (clientId.empty()) { r.error = "no client_id (.player key <id>)"; return r; }
    // Auto-pick path (autoplay): keep the duration cap on.
    g_allowLong = false;

    // Charts = the trending feed (baseline pick when nothing personal exists
    // yet). Verified live 2026-10: kind=top is GONE (400), the working form is
    // kind=trending + genre=soundcloud%3Agenres%3Aall-music.
    const std::string url = "https://api-v2.soundcloud.com/charts?kind=trending"
        "&genre=soundcloud%3Agenres%3Aall-music"
        "&client_id=" + urlEncode(clientId) + "&limit=50";
    HttpResp resp = httpGet(url, 512 * 1024);
    if (!resp.ok) {
        char b[64];
        std::snprintf(b, sizeof(b), "charts http=%lu", resp.status);
        r.error = b;
        return r;
    }
    nlohmann::json j;
    try { j = nlohmann::json::parse(resp.body); }
    catch (...) { r.error = "bad charts json"; return r; }
    if (!j.is_object() || !j.contains("collection") || !j["collection"].is_array()) {
        r.error = "no charts";
        return r;
    }
    std::vector<nlohmann::json> items;
    for (const auto& e : j["collection"]) {
        if (!e.is_object()) continue;
        auto it = e.find("track");
        if (it != e.end() && it->is_object()) items.push_back(*it);
    }
    if (items.empty()) { r.error = "empty charts"; return r; }

    Track track;
    if (!resolveRandomOrder(items, clientId, track)) {
        r.error = "no playable chart hit (try again)";
        return r;
    }
    r.track = std::move(track);
    r.ok = true;
    return r;
}

bool downloadToFile(const std::string& url, const std::string& path) {
    // Manual redirect loop: stream the final 200 response straight to disk
    // (audio files are megabytes — never buffer them fully in RAM).
    std::string cur = url;
    for (int hop = 0; hop <= 5; ++hop) {
        bool secure = false;
        std::wstring host, wpath;
        if (!splitUrl(cur, secure, host, wpath)) return false;

        HINTERNET session = WinHttpOpen(L"Sunstead/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        WinHttpSetTimeouts(session, 5000, 5000, 15000, 15000);
        bool hopDone = false;
        HINTERNET connect = WinHttpConnect(session, host.c_str(),
                                           secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
        HINTERNET request = nullptr;
        if (connect) {
            request = WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0);
        }
        if (connect && request &&
            WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                    WINHTTP_NO_HEADER_INDEX)) {
                if (status == 301 || status == 302 || status == 303 ||
                    status == 307 || status == 308) {
                    DWORD locSz = 0;
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                                        WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &locSz,
                                        WINHTTP_NO_HEADER_INDEX);
                    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && locSz > 0) {
                        std::wstring loc(locSz / sizeof(wchar_t), 0);
                        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                                                WINHTTP_HEADER_NAME_BY_INDEX, loc.data(), &locSz,
                                                WINHTTP_NO_HEADER_INDEX)) {
                            while (!loc.empty() && loc.back() == L'\0') loc.pop_back();
                            cur = narrow(loc);
                            hopDone = true; // continue outer loop with new URL
                        }
                    }
                } else if (status == 200) {
                    std::ofstream f(path, std::ios::binary | std::ios::trunc);
                    if (f) {
                        size_t total = 0;
                        const size_t kCap = 40 * 1024 * 1024;
                        bool writeFail = false;
                        for (;;) {
                            char buf[8192];
                            DWORD read = 0;
                            if (!WinHttpReadData(request, buf, sizeof(buf), &read) || read == 0) break;
                            f.write(buf, read);
                            if (!f) { writeFail = true; break; }
                            total += read;
                            if (total > kCap) { writeFail = true; break; }
                        }
                        f.close();
                        if (!writeFail && total > 0) {
                            if (request) WinHttpCloseHandle(request);
                            if (connect) WinHttpCloseHandle(connect);
                            WinHttpCloseHandle(session);
                            return true;
                        }
                        DeleteFileA(path.c_str());
                    }
                }
            }
        }
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (hopDone) continue; // followed a redirect
        return false;
    }
    return false;
}

std::vector<unsigned char> downloadBytes(const std::string& url) {
    HttpResp r = httpGet(url, 2 * 1024 * 1024);
    if (!r.ok) return {};
    return r.bytes;
}

// --- Recommendations --------------------------------------------------------

Result relatedRandom(const std::string& clientId, long long trackId) {
    Result r;
    if (clientId.empty()) { r.error = "no client_id (.player key <id>)"; return r; }
    if (trackId <= 0) { r.error = "no seed track"; return r; }
    // Auto-pick path (the wave): keep the duration cap on.
    g_allowLong = false;

    const std::string url = "https://api-v2.soundcloud.com/tracks/" +
        std::to_string(trackId) + "/related?client_id=" + urlEncode(clientId) + "&limit=32";
    HttpResp resp = httpGet(url, 512 * 1024);
    if (!resp.ok) {
        char b[64];
        std::snprintf(b, sizeof(b), "related http=%lu", resp.status);
        r.error = b;
        return r;
    }
    nlohmann::json j;
    try { j = nlohmann::json::parse(resp.body); }
    catch (...) { r.error = "bad related json"; return r; }

    std::vector<nlohmann::json> items;
    collectTracks(j, items);
    if (items.empty()) { r.error = "no related tracks"; return r; }

    Track track;
    if (!resolveRandomOrder(items, clientId, track)) {
        r.error = "no playable related hit (try again)";
        return r;
    }
    r.track = std::move(track);
    r.ok = true;
    return r;
}

UserInfo resolveUser(const std::string& clientId, const std::string& urlOrName) {
    UserInfo u;
    if (clientId.empty()) { u.error = "no client_id (.player key <id>)"; return u; }
    std::string target = urlOrName;
    // Bare username / partial input -> a canonical profile URL.
    if (target.find("soundcloud.com") == std::string::npos) {
        if (target.rfind("http://", 0) != 0 && target.rfind("https://", 0) != 0)
            target = "https://soundcloud.com/" + target;
    }
    if (target.empty()) { u.error = "empty profile"; return u; }

    const std::string url = "https://api-v2.soundcloud.com/resolve?url=" +
        urlEncode(target) + "&client_id=" + urlEncode(clientId);
    HttpResp resp = httpGet(url, 128 * 1024);
    if (!resp.ok) {
        char b[64];
        std::snprintf(b, sizeof(b), "resolve http=%lu", resp.status);
        u.error = b;
        if (resp.status == 404) u.error = "profile not found (check the link)";
        return u;
    }
    try {
        auto j = nlohmann::json::parse(resp.body);
        const std::string kind = getStr(j, "kind");
        if (!kind.empty() && kind != "user") { u.error = "that link is not a user profile"; return u; }
        u.id = j.value("id", 0LL);
        u.username = getStr(j, "username");
        u.permalinkUrl = getStr(j, "permalink_url");
        u.ok = (u.id > 0);
        if (!u.ok) u.error = "resolve returned no user id";
    } catch (...) {
        u.error = "bad resolve json";
    }
    return u;
}

Result userLikesRandom(const std::string& clientId, long long userId) {
    Result r;
    if (clientId.empty()) { r.error = "no client_id (.player key <id>)"; return r; }
    if (userId <= 0) { r.error = "no profile resolved (.player user <link>)"; return r; }
    g_allowLong = false;

    // Verified against live api-v2 (2026-10): the listing endpoint is
    // /users/{id}/likes (NOT /likes/tracks, which 404s) — its items are
    // like-objects {created_at, kind, track|playlist}.
    const std::string url = "https://api-v2.soundcloud.com/users/" +
        std::to_string(userId) + "/likes?client_id=" + urlEncode(clientId) + "&limit=48";
    HttpResp resp = httpGet(url, 768 * 1024);
    if (!resp.ok) {
        char b[64];
        std::snprintf(b, sizeof(b), "likes http=%lu", resp.status);
        r.error = b;
        if (resp.status == 401 || resp.status == 403)
            r.error = "likes are private on that profile";
        return r;
    }
    nlohmann::json j;
    try { j = nlohmann::json::parse(resp.body); }
    catch (...) { r.error = "bad likes json"; return r; }

    std::vector<nlohmann::json> items;
    collectTracks(j, items);
    if (items.empty()) { r.error = "no public liked tracks"; return r; }

    Track track;
    if (!resolveRandomOrder(items, clientId, track)) {
        r.error = "no playable like (try again)";
        return r;
    }
    r.track = std::move(track);
    r.ok = true;
    return r;
}

} // namespace SoundCloud
